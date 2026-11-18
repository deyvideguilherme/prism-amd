#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>
#include <drm/drm.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <cerrno>

#include "drm_ctm.hpp"
#include "config.hpp"

#define CHROMA_EXPORT __attribute__((visibility("default")))

typedef int (*pfn_drmModeAtomicCommit)(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data);
typedef int (*pfn_drmModeSetCrtc)(int fd, uint32_t crtcId, uint32_t bufferId,
                                  uint32_t x, uint32_t y, uint32_t *connectors, int count,
                                  drmModeModeInfoPtr mode);
typedef int (*pfn_ioctl)(int fd, unsigned long request, void *arg);

static pfn_drmModeAtomicCommit real_drmModeAtomicCommit = nullptr;
static pfn_drmModeSetCrtc real_drmModeSetCrtc = nullptr;
static pfn_ioctl real_ioctl = nullptr;

static std::mutex g_log_mutex;

static void log_drm(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* f = fopen("/tmp/chroma-drm.log", "a");
    if (!f) return;

    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf{};
    localtime_r(&ts.tv_sec, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(f, "[%s.%03ld] [PID %d] [chroma-drm] ", time_str, ts.tv_nsec / 1000000, getpid());

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

// Layout interno da struct _drmModeAtomicReq da libdrm
struct DrmAtomicReqItem {
    uint32_t object_id;
    uint32_t property_id;
    uint64_t value;
    uint32_t cursor;
};

struct DrmAtomicReq {
    uint32_t cursor;
    uint32_t size_items;
    DrmAtomicReqItem *items;
};

struct CrtcInfo {
    uint32_t crtc_id{0};
    uint32_t ctm_prop_id{0};
    uint32_t gamma_lut_prop_id{0};
    uint32_t degamma_lut_prop_id{0};
    uint64_t gamma_lut_size{0};
    uint64_t degamma_lut_size{0};
    bool is_active{false};

    uint32_t active_ctm_blob_id{0};
    uint32_t active_gamma_blob_id{0};

    uint32_t staged_ctm_blob_id{0};
    uint32_t staged_gamma_blob_id{0};
    uint32_t old_ctm_blob_id{0};
    uint32_t old_gamma_blob_id{0};

    bool pending_update{true};
    bool first_ctm_applied{false};
};

struct DrmDeviceInfo {
    int fd{-1};
    dev_t dev_id{0};
    bool inspected{false};
    bool has_ctm{false};
    std::vector<CrtcInfo> crtcs;
};

static std::mutex g_state_mutex;
static std::unordered_map<dev_t, DrmDeviceInfo> g_devices;
static thread_local bool t_in_hook = false;

static void init_real_functions() {
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        real_drmModeAtomicCommit = reinterpret_cast<pfn_drmModeAtomicCommit>(dlsym(RTLD_NEXT, "drmModeAtomicCommit"));
        real_drmModeSetCrtc = reinterpret_cast<pfn_drmModeSetCrtc>(dlsym(RTLD_NEXT, "drmModeSetCrtc"));
        real_ioctl = reinterpret_cast<pfn_ioctl>(dlsym(RTLD_NEXT, "ioctl"));

        if (!real_drmModeAtomicCommit) {
            log_drm("Aviso: falha ao resolver drmModeAtomicCommit via RTLD_NEXT: %s", dlerror());
        }
        if (!real_ioctl) {
            log_drm("Aviso: falha ao resolver ioctl via RTLD_NEXT: %s", dlerror());
        }
        log_drm("Funções nativas resolvidas com sucesso via dlsym(RTLD_NEXT).");
    });
}

__attribute__((constructor))
static void chroma_drm_constructor() {
    log_drm("=== Biblioteca libchroma_drm.so carregada via LD_PRELOAD ===");
}

static void inspect_device(int fd, DrmDeviceInfo& dev) {
    if (dev.inspected) return;

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        // Dispositivo sem suporte a modos KMS (ex: nó de renderização /dev/dri/renderD128)
        return;
    }

    dev.crtcs.clear();
    dev.has_ctm = false;

    log_drm("Inspecionando propriedades DRM para fd=%d (total CRTCs=%d)...", fd, res->count_crtcs);

    for (int i = 0; i < res->count_crtcs; ++i) {
        uint32_t crtc_id = res->crtcs[i];
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
        if (!props) continue;

        CrtcInfo crtc{};
        crtc.crtc_id = crtc_id;

        for (uint32_t j = 0; j < props->count_props; ++j) {
            drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
            if (!prop) continue;

            if (strcmp(prop->name, "CTM") == 0) {
                crtc.ctm_prop_id = prop->prop_id;
                dev.has_ctm = true;
            } else if (strcmp(prop->name, "GAMMA_LUT") == 0) {
                crtc.gamma_lut_prop_id = prop->prop_id;
            } else if (strcmp(prop->name, "GAMMA_LUT_SIZE") == 0) {
                crtc.gamma_lut_size = props->prop_values[j];
            } else if (strcmp(prop->name, "DEGAMMA_LUT") == 0) {
                crtc.degamma_lut_prop_id = prop->prop_id;
            } else if (strcmp(prop->name, "DEGAMMA_LUT_SIZE") == 0) {
                crtc.degamma_lut_size = props->prop_values[j];
            } else if (strcmp(prop->name, "ACTIVE") == 0) {
                crtc.is_active = (props->prop_values[j] != 0);
            }
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);

        if (crtc.ctm_prop_id != 0) {
            dev.crtcs.push_back(crtc);
            log_drm("CRTC %u detectado: CTM prop=%u, GAMMA_LUT prop=%u (size=%lu), DEGAMMA_LUT prop=%u (size=%lu), ACTIVE=%d",
                    crtc.crtc_id, crtc.ctm_prop_id, crtc.gamma_lut_prop_id, crtc.gamma_lut_size,
                    crtc.degamma_lut_prop_id, crtc.degamma_lut_size, crtc.is_active);
        }
    }
    drmModeFreeResources(res);

    dev.inspected = true;
    if (dev.has_ctm) {
        log_drm("GPU AMD com suporte a DRM CTM pronta para aceleração de cor em hardware!");
    } else {
        log_drm("Dispositivo DRM não expõe propriedade CTM. Passando adiante sem intervir.");
    }
}

static bool req_contains_crtc(const drmModeAtomicReqPtr req_ptr, uint32_t crtc_id) {
    if (!req_ptr) return false;
    const auto* req = reinterpret_cast<const DrmAtomicReq*>(req_ptr);
    if (!req->items || req->cursor == 0) return false;
    for (uint32_t i = 0; i < req->cursor; ++i) {
        if (req->items[i].object_id == crtc_id || req->items[i].value == crtc_id) {
            return true;
        }
    }
    return false;
}

extern "C" {

CHROMA_EXPORT int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data) {
    init_real_functions();

    if (!real_drmModeAtomicCommit) {
        return -1;
    }

    static bool s_first_atomic_commit = true;
    if (s_first_atomic_commit) {
        s_first_atomic_commit = false;
        log_drm("Primeira chamada drmModeAtomicCommit interceptada! fd=%d, flags=0x%x", fd, flags);
    }

    // Proteção contra recursão
    if (t_in_hook || fd < 0 || !req) {
        return real_drmModeAtomicCommit(fd, req, flags, user_data);
    }

    struct HookGuard {
        HookGuard() { t_in_hook = true; }
        ~HookGuard() { t_in_hook = false; }
    } guard;

    try {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
            return real_drmModeAtomicCommit(fd, req, flags, user_data);
        }

        dev_t dev_id = st.st_rdev;
        auto& dev = g_devices[dev_id];
        dev.fd = fd;
        dev.dev_id = dev_id;

        if (!dev.inspected) {
            inspect_device(fd, dev);
        }

        if (!dev.has_ctm || dev.crtcs.empty()) {
            return real_drmModeAtomicCommit(fd, req, flags, user_data);
        }

        // Verifica se /tmp/chroma.conf foi modificado via stat/mtime
        bool config_changed = ConfigManager::instance().check_and_reload();
        ChromaPushConstants cfg = ConfigManager::instance().get();

        int orig_cursor = drmModeAtomicGetCursor(req);
        bool has_ctm_transition = false;
        bool any_injected = false;

        for (auto& crtc : dev.crtcs) {
            if (config_changed) {
                crtc.pending_update = true;
            }

            if (!crtc.pending_update) {
                continue;
            }

            // Decide se este CRTC é o alvo da requisição atômica
            bool should_target = false;
            if (dev.crtcs.size() == 1) {
                should_target = true;
            } else if (req_contains_crtc(req, crtc.crtc_id)) {
                should_target = true;
            } else if (crtc.is_active) {
                should_target = true;
            }

            if (!should_target) {
                continue;
            }

            // Calcula CTM e opcionalmente GAMMA_LUT para brilho/contraste
            bool use_gamma_lut = (crtc.gamma_lut_prop_id != 0 && crtc.gamma_lut_size > 0 &&
                                  (std::abs(cfg.brightness) > 1e-4f || std::abs(cfg.contrast - 1.0f) > 1e-4f));

            double ctm_contrast = use_gamma_lut ? 1.0 : cfg.contrast;
            drm_color_ctm ctm = chroma::calculate_ctm(cfg.saturation, ctm_contrast);

            uint32_t new_ctm_blob = 0;
            int ret_blob = drmModeCreatePropertyBlob(fd, &ctm, sizeof(ctm), &new_ctm_blob);
            if (ret_blob != 0 || new_ctm_blob == 0) {
                log_drm("Erro ao criar property blob CTM: %d (errno=%d: %s)", ret_blob, errno, strerror(errno));
                continue;
            }

            uint32_t new_gamma_blob = 0;
            if (use_gamma_lut) {
                auto lut = chroma::calculate_gamma_lut(crtc.gamma_lut_size, cfg.contrast, cfg.brightness);
                int ret_gamma = drmModeCreatePropertyBlob(fd, lut.data(), lut.size() * sizeof(drm_color_lut), &new_gamma_blob);
                if (ret_gamma != 0 || new_gamma_blob == 0) {
                    log_drm("Aviso: erro ao criar property blob GAMMA_LUT: %d", ret_gamma);
                }
            }

            // Injeta CTM no atomic req antes de repassar
            int add_ret = drmModeAtomicAddProperty(req, crtc.crtc_id, crtc.ctm_prop_id, new_ctm_blob);
            if (add_ret < 0) {
                log_drm("Erro ao adicionar propriedade CTM ao req no CRTC %u: %d", crtc.crtc_id, add_ret);
                drmModeDestroyPropertyBlob(fd, new_ctm_blob);
                if (new_gamma_blob != 0) {
                    drmModeDestroyPropertyBlob(fd, new_gamma_blob);
                }
                continue;
            }

            if (new_gamma_blob != 0) {
                int add_g = drmModeAtomicAddProperty(req, crtc.crtc_id, crtc.gamma_lut_prop_id, new_gamma_blob);
                if (add_g < 0) {
                    log_drm("Aviso: falha ao adicionar GAMMA_LUT ao req no CRTC %u: %d", crtc.crtc_id, add_g);
                    drmModeDestroyPropertyBlob(fd, new_gamma_blob);
                    new_gamma_blob = 0;
                }
            }

            // Detecta transição de CTM (primeira aplicação ou valor alterado)
            if (!crtc.first_ctm_applied || crtc.active_ctm_blob_id != new_ctm_blob) {
                has_ctm_transition = true;
            }

            crtc.staged_ctm_blob_id = new_ctm_blob;
            crtc.old_ctm_blob_id = crtc.active_ctm_blob_id;

            crtc.staged_gamma_blob_id = new_gamma_blob;
            crtc.old_gamma_blob_id = crtc.active_gamma_blob_id;
            any_injected = true;
        }

        // Se nada precisou ser injetado, executa direto
        if (!any_injected) {
            return real_drmModeAtomicCommit(fd, req, flags, user_data);
        }

        // Ajuste de flags para amdgpu DCN:
        // Na transição de CTM, DRM_MODE_ATOMIC_ALLOW_MODESET é necessário para habilitar
        // os blocos de cor sem retornar -EINVAL, removendo temporariamente NONBLOCK.
        uint32_t commit_flags = flags;
        if (has_ctm_transition) {
            commit_flags &= ~DRM_MODE_ATOMIC_NONBLOCK;
            commit_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
            log_drm("Transição CTM no amdgpu DCN: flags 0x%x -> 0x%x (ALLOW_MODESET ativado, NONBLOCK desativado)",
                    flags, commit_flags);
        }

        // Executa o commit atômico com CTM
        int ret = real_drmModeAtomicCommit(fd, req, commit_flags, user_data);

        // Se falhar, loga errno e aciona fallback
        if (ret != 0) {
            int commit_err = errno;
            log_drm("drmModeAtomicCommit com CTM falhou: ret=%d, errno=%d (%s)",
                    ret, commit_err, strerror(commit_err));

            // Tenta aplicar via drmModeObjectSetProperty antes do fallback
            bool fallback_prop_ok = false;
            for (auto& crtc : dev.crtcs) {
                if (crtc.staged_ctm_blob_id != 0) {
                    log_drm("Tentando fallback via drmModeObjectSetProperty(CRTC %u, CTM)...", crtc.crtc_id);
                    int sp_ret = drmModeObjectSetProperty(fd, crtc.crtc_id, DRM_MODE_OBJECT_CRTC,
                                                          crtc.ctm_prop_id, crtc.staged_ctm_blob_id);
                    if (sp_ret == 0) {
                        log_drm("drmModeObjectSetProperty no CRTC %u aplicado com SUCESSO!", crtc.crtc_id);
                        fallback_prop_ok = true;
                        if (crtc.active_ctm_blob_id != 0 && crtc.active_ctm_blob_id != crtc.staged_ctm_blob_id) {
                            drmModeDestroyPropertyBlob(fd, crtc.active_ctm_blob_id);
                        }
                        crtc.active_ctm_blob_id = crtc.staged_ctm_blob_id;
                        crtc.staged_ctm_blob_id = 0;
                        crtc.old_ctm_blob_id = 0;
                        crtc.first_ctm_applied = true;
                        crtc.pending_update = false;
                    } else {
                        log_drm("drmModeObjectSetProperty no CRTC %u falhou: ret=%d (errno=%d: %s)",
                                crtc.crtc_id, sp_ret, errno, strerror(errno));
                    }
                }
            }

            // Restaura o cursor original do req para remover as propriedades injetadas
            if (orig_cursor >= 0) {
                drmModeAtomicSetCursor(req, orig_cursor);
            }

            // Limpa staged blobs caso o drmModeObjectSetProperty não tenha aplicado
            if (!fallback_prop_ok) {
                for (auto& crtc : dev.crtcs) {
                    if (crtc.staged_ctm_blob_id != 0) {
                        drmModeDestroyPropertyBlob(fd, crtc.staged_ctm_blob_id);
                        crtc.staged_ctm_blob_id = 0;
                        crtc.old_ctm_blob_id = 0;
                    }
                    if (crtc.staged_gamma_blob_id != 0) {
                        drmModeDestroyPropertyBlob(fd, crtc.staged_gamma_blob_id);
                        crtc.staged_gamma_blob_id = 0;
                        crtc.old_gamma_blob_id = 0;
                    }
                }
            }

            // Executa o commit atômico original do compositor para garantir estabilidade absoluta
            log_drm("Repassando commit atômico original do compositor sem CTM (flags: 0x%x)...", flags);
            ret = real_drmModeAtomicCommit(fd, req, flags, user_data);
            if (ret != 0) {
                log_drm("Commit original do compositor também retornou erro: ret=%d, errno=%d (%s)",
                        ret, errno, strerror(errno));
            } else {
                log_drm("Commit original do compositor repassado com sucesso.");
            }
            return ret;
        }

        // Commit com CTM bem-sucedido!
        log_drm("drmModeAtomicCommit com CTM executado com sucesso!");
        if (!(flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
            for (auto& crtc : dev.crtcs) {
                if (crtc.staged_ctm_blob_id != 0) {
                    if (crtc.old_ctm_blob_id != 0 && crtc.old_ctm_blob_id != crtc.staged_ctm_blob_id) {
                        drmModeDestroyPropertyBlob(fd, crtc.old_ctm_blob_id);
                    }
                    crtc.active_ctm_blob_id = crtc.staged_ctm_blob_id;
                    crtc.staged_ctm_blob_id = 0;
                    crtc.old_ctm_blob_id = 0;
                    crtc.first_ctm_applied = true;
                    crtc.pending_update = false;
                }
                if (crtc.staged_gamma_blob_id != 0) {
                    if (crtc.old_gamma_blob_id != 0 && crtc.old_gamma_blob_id != crtc.staged_gamma_blob_id) {
                        drmModeDestroyPropertyBlob(fd, crtc.old_gamma_blob_id);
                    }
                    crtc.active_gamma_blob_id = crtc.staged_gamma_blob_id;
                    crtc.staged_gamma_blob_id = 0;
                    crtc.old_gamma_blob_id = 0;
                }
            }
        }

        return ret;
    } catch (...) {
        log_drm("Exceção capturada em drmModeAtomicCommit! Executando fallback seguro.");
        return real_drmModeAtomicCommit(fd, req, flags, user_data);
    }
}

CHROMA_EXPORT int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
                                 uint32_t x, uint32_t y, uint32_t *connectors, int count,
                                 drmModeModeInfoPtr mode) {
    init_real_functions();

    if (!real_drmModeSetCrtc) {
        return -1;
    }

    static bool s_first_set_crtc = true;
    if (s_first_set_crtc) {
        s_first_set_crtc = false;
        log_drm("Primeira chamada drmModeSetCrtc interceptada! fd=%d, crtcId=%u", fd, crtcId);
    }

    int ret = real_drmModeSetCrtc(fd, crtcId, bufferId, x, y, connectors, count, mode);
    if (ret != 0 || fd < 0) {
        return ret;
    }

    try {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        struct stat st{};
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
            auto& dev = g_devices[st.st_rdev];
            dev.fd = fd;
            dev.dev_id = st.st_rdev;
            if (!dev.inspected) {
                inspect_device(fd, dev);
            }

            if (dev.has_ctm) {
                for (auto& crtc : dev.crtcs) {
                    if (crtc.crtc_id == crtcId && crtc.ctm_prop_id != 0) {
                        ConfigManager::instance().check_and_reload();
                        ChromaPushConstants cfg = ConfigManager::instance().get();
                        drm_color_ctm ctm = chroma::calculate_ctm(cfg.saturation, cfg.contrast);

                        uint32_t new_blob = 0;
                        if (drmModeCreatePropertyBlob(fd, &ctm, sizeof(ctm), &new_blob) == 0 && new_blob != 0) {
                            if (drmModeObjectSetProperty(fd, crtcId, DRM_MODE_OBJECT_CRTC, crtc.ctm_prop_id, new_blob) == 0) {
                                log_drm("drmModeSetCrtc: CTM aplicado com sucesso no CRTC %u", crtcId);
                                if (crtc.active_ctm_blob_id != 0 && crtc.active_ctm_blob_id != new_blob) {
                                    drmModeDestroyPropertyBlob(fd, crtc.active_ctm_blob_id);
                                }
                                crtc.active_ctm_blob_id = new_blob;
                            } else {
                                log_drm("drmModeSetCrtc: falha ao aplicar CTM via drmModeObjectSetProperty: errno=%d (%s)",
                                        errno, strerror(errno));
                                drmModeDestroyPropertyBlob(fd, new_blob);
                            }
                        }
                        break;
                    }
                }
            }
        }
    } catch (...) {
        // Falha segura
    }

    return ret;
}

// Hook de ioctl para compositores Rust/Smithay (como o Niri) que fazem chamadas diretas de ioctl
CHROMA_EXPORT int ioctl(int fd, unsigned long request, ...) {
    va_list va;
    va_start(va, request);
    void *arg = va_arg(va, void *);
    va_end(va);

    if (!real_ioctl) {
        real_ioctl = reinterpret_cast<pfn_ioctl>(dlsym(RTLD_NEXT, "ioctl"));
    }

    // Passagem direta rápida para tudo que não for DRM_IOCTL_MODE_ATOMIC
    if (t_in_hook || !real_ioctl || request != DRM_IOCTL_MODE_ATOMIC || fd < 0 || !arg) {
        return real_ioctl ? real_ioctl(fd, request, arg) : -1;
    }

    static bool s_first_ioctl_atomic = true;
    if (s_first_ioctl_atomic) {
        s_first_ioctl_atomic = false;
        log_drm("Primeira chamada ioctl(DRM_IOCTL_MODE_ATOMIC) interceptada (Rust/Smithay/Niri)! fd=%d", fd);
    }

    struct HookGuard {
        HookGuard() { t_in_hook = true; }
        ~HookGuard() { t_in_hook = false; }
    } guard;

    try {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
            return real_ioctl(fd, request, arg);
        }

        dev_t dev_id = st.st_rdev;
        auto& dev = g_devices[dev_id];
        dev.fd = fd;
        dev.dev_id = dev_id;

        if (!dev.inspected) {
            inspect_device(fd, dev);
        }

        if (dev.has_ctm && !dev.crtcs.empty()) {
            bool config_changed = ConfigManager::instance().check_and_reload();
            ChromaPushConstants cfg = ConfigManager::instance().get();

            auto* atomic = reinterpret_cast<struct drm_mode_atomic *>(arg);

            for (auto& crtc : dev.crtcs) {
                if (config_changed) {
                    crtc.pending_update = true;
                }
                if (!crtc.pending_update) {
                    continue;
                }

                drm_color_ctm ctm = chroma::calculate_ctm(cfg.saturation, cfg.contrast);
                uint32_t new_ctm_blob = 0;
                if (drmModeCreatePropertyBlob(fd, &ctm, sizeof(ctm), &new_ctm_blob) == 0 && new_ctm_blob != 0) {
                    log_drm("ioctl: aplicando CTM no CRTC %u via drmModeObjectSetProperty...", crtc.crtc_id);
                    int sp_ret = drmModeObjectSetProperty(fd, crtc.crtc_id, DRM_MODE_OBJECT_CRTC,
                                                          crtc.ctm_prop_id, new_ctm_blob);
                    if (sp_ret == 0) {
                        log_drm("ioctl: CTM aplicado com sucesso no CRTC %u!", crtc.crtc_id);
                        if (crtc.active_ctm_blob_id != 0 && crtc.active_ctm_blob_id != new_ctm_blob) {
                            drmModeDestroyPropertyBlob(fd, crtc.active_ctm_blob_id);
                        }
                        crtc.active_ctm_blob_id = new_ctm_blob;
                        crtc.first_ctm_applied = true;
                        crtc.pending_update = false;
                    } else {
                        log_drm("ioctl: drmModeObjectSetProperty falhou: errno=%d (%s)", errno, strerror(errno));
                        drmModeDestroyPropertyBlob(fd, new_ctm_blob);
                    }
                }
            }

            // Se for transição de flags no commit atômico, assegura ALLOW_MODESET
            if (atomic) {
                uint32_t orig_flags = atomic->flags;
                for (const auto& crtc : dev.crtcs) {
                    if (!crtc.first_ctm_applied) {
                        atomic->flags &= ~DRM_MODE_ATOMIC_NONBLOCK;
                        atomic->flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
                        log_drm("ioctl: Ajustando flags para transição: 0x%x -> 0x%x", orig_flags, atomic->flags);
                        break;
                    }
                }
            }
        }

        return real_ioctl(fd, request, arg);
    } catch (...) {
        return real_ioctl(fd, request, arg);
    }
}

} // extern "C"

__attribute__((destructor))
static void chroma_drm_cleanup() {
    log_drm("Descarregando libchroma_drm.so do processo (PID %d)...", getpid());
    try {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (auto& [dev_id, dev] : g_devices) {
            if (dev.fd >= 0) {
                for (auto& crtc : dev.crtcs) {
                    if (crtc.active_ctm_blob_id != 0) {
                        drmModeDestroyPropertyBlob(dev.fd, crtc.active_ctm_blob_id);
                        crtc.active_ctm_blob_id = 0;
                    }
                    if (crtc.active_gamma_blob_id != 0) {
                        drmModeDestroyPropertyBlob(dev.fd, crtc.active_gamma_blob_id);
                        crtc.active_gamma_blob_id = 0;
                    }
                }
            }
        }
    } catch (...) {
    }
}

#include "doctor.hpp"
#include "prisma_abi.h"
#include "telemetry.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

namespace prisma {

namespace {

std::string trim(std::string_view str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(first, last - first + 1));
}

std::optional<std::string> read_line(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return std::nullopt;
    std::string line;
    if (std::getline(f, line)) {
        return trim(line);
    }
    return std::nullopt;
}

struct ConnectorInfo {
    std::string name;
    std::string status;
    std::string mode;
};

std::vector<ConnectorInfo> get_connectors(const std::string& card_name, const std::filesystem::path& drm_root = "/sys/class/drm") {
    std::vector<ConnectorInfo> result;
    std::error_code ec;
    std::string prefix = card_name + "-";

    for (std::filesystem::directory_iterator it(drm_root, ec), end; it != end && !ec; it.increment(ec)) {
        const auto& entry = *it;
        std::string fn = entry.path().filename().string();
        if (fn.starts_with(prefix)) {
            std::string conn_name = fn.substr(prefix.size());
            auto status = read_line(entry.path() / "status");
            if (status) {
                std::string mode = "";
                if (*status == "connected") {
                    auto m = read_line(entry.path() / "modes");
                    if (m) mode = *m;
                }
                result.push_back({conn_name, *status, mode});
            }
        }
    }
    return result;
}

} // namespace

int run_doctor() {
    bool all_ok = true;

    std::cout << "Prisma Control Center - System Self-Diagnostic (doctor)\n"
              << "======================================================\n\n";

    // 1. Linux Kernel
    struct utsname uts{};
    if (uname(&uts) == 0) {
        std::cout << "[OK] Linux kernel: " << uts.release << " (" << uts.machine << ")\n";
    } else {
        std::cout << "[FAIL] Failed to query kernel information with uname().\n";
        all_ok = false;
    }

    // 2. Kernel Module Status
    bool module_loaded = std::filesystem::exists("/sys/module/chroma_amd");
    if (module_loaded) {
        std::string mod_ver = "";
        auto v = read_line("/sys/module/chroma_amd/version");
        if (v && !v->empty()) {
            mod_ver = " (version " + *v + ")";
        }
        std::cout << "[OK] Kernel module: chroma_amd is loaded" << mod_ver << "\n";
    } else {
        std::cout << "[FAIL] Kernel module: chroma_amd is not loaded in the running kernel.\n"
                  << "       Remediation:\n"
                  << "       - Load module manually: sudo modprobe chroma_amd (or sudo make -C kernel load)\n"
                  << "       - Check DKMS status: dkms status\n";
        all_ok = false;
    }

    // 3. Device Node & Permissions
    const char* dev_node = "/dev/chroma_amd";
    struct stat dev_stat{};
    bool dev_exists = (::stat(dev_node, &dev_stat) == 0);

    if (!dev_exists) {
        std::cout << "[FAIL] Device node: " << dev_node << " does not exist.\n"
                  << "       Ensure the chroma_amd module is loaded and udev created the character device node.\n";
        all_ok = false;
    } else {
        std::string group_name = "unknown";
        struct group* gr = ::getgrgid(dev_stat.st_gid);
        if (gr) {
            group_name = gr->gr_name;
        }

        int fd_rw = ::open(dev_node, O_RDWR | O_CLOEXEC);
        int fd_ro = (fd_rw < 0) ? ::open(dev_node, O_RDONLY | O_CLOEXEC) : -1;
        int active_fd = (fd_rw >= 0) ? fd_rw : fd_ro;

        if (active_fd < 0) {
            std::cout << "[FAIL] Device node: " << dev_node << " exists but access was denied.\n"
                      << "       Current user lacks read/write permissions for " << dev_node << " (group: " << group_name << ").\n"
                      << "       Remediation:\n"
                      << "       - Add user to video group: sudo usermod -aG video $USER\n"
                      << "       - Ensure udev rule is installed: /usr/lib/udev/rules.d/70-chroma-amd.rules\n"
                      << "       - Log out and log back in, or restart your session (logind).\n";
            all_ok = false;
        } else {
            if (fd_rw >= 0) {
                std::cout << "[OK] Device node: " << dev_node
                          << " (group: " << group_name << ", read/write access granted)\n";
            } else {
                std::cout << "[FAIL] Device node: " << dev_node
                          << " (group: " << group_name << ", read-only access)\n";
                all_ok = false;
            }

            uint32_t kabi = 0;
            if (::ioctl(active_fd, PRISMA_IOC_GET_ABI, &kabi) < 0) {
                std::cout << "[FAIL] Protocol ABI ioctl unavailable. Reload the current chroma_amd module.\n";
                all_ok = false;
            } else if (kabi != PRISMA_ABI_VERSION) {
                std::cout << "[FAIL] Protocol ABI mismatch: kernel module reports ABI v" << kabi
                          << ", but CLI expects ABI v" << PRISMA_ABI_VERSION << ".\n";
                all_ok = false;
            } else {
                std::cout << "[OK] Protocol ABI: v" << kabi
                          << " (kernel module and CLI userspace are synchronized)\n";
            }

            ::close(active_fd);
        }
    }

    // 5. AMD GPU Dynamic Detection & Hardware Telemetry
    try {
        GpuTelemetry telem = read_gpu_telemetry();
        if (telem.card_name.empty()) throw std::runtime_error("no AMD DRM device found");
        std::cout << "[OK] AMD GPU detected: " << telem.card_name
                  << " (driver: amdgpu, PCI: " << telem.pci_id << ")\n";

        // Connectors
        auto connectors = get_connectors(telem.card_name);
        bool has_active_conn = false;
        for (const auto& c : connectors) {
            if (c.status == "connected") {
                has_active_conn = true;
                std::cout << "     - Display output: " << c.name
                          << " (connected" << (c.mode.empty() ? "" : ", " + c.mode) << ")\n";
            }
        }
        if (!has_active_conn) {
            std::cout << "     - Display output: no active display connected to this GPU\n";
        }

        // Telemetry details
        std::cout << "     - Hardware telemetry:\n";
        if (telem.temp_edge_c) {
            std::cout << "       * Edge temperature:     " << std::fixed << std::setprecision(1) << *telem.temp_edge_c << " °C\n";
        }
        if (telem.temp_junction_c) {
            std::cout << "       * Hotspot temperature:  " << std::fixed << std::setprecision(1) << *telem.temp_junction_c << " °C\n";
        }
        if (telem.sclk_mhz) {
            std::cout << "       * Graphics clock (SCLK): " << *telem.sclk_mhz << " MHz\n";
        }
        if (telem.gpu_busy_percent) {
            std::cout << "       * GPU load:              " << *telem.gpu_busy_percent << " %\n";
        }
        if (telem.vram_used_mib && telem.vram_total_mib) {
            double pct = (*telem.vram_total_mib > 0) ? (*telem.vram_used_mib / *telem.vram_total_mib * 100.0) : 0.0;
            std::cout << "       * VRAM usage:            " << std::fixed << std::setprecision(0)
                      << *telem.vram_used_mib << " / " << *telem.vram_total_mib << " MiB ("
                      << std::fixed << std::setprecision(1) << pct << " %)\n";
        }
        if (telem.power_watts) {
            std::cout << "       * Board power:           " << std::fixed << std::setprecision(1) << *telem.power_watts << " W\n";
        }
    } catch (const std::exception& e) {
        std::cout << "[FAIL] AMD GPU detection error: " << e.what() << "\n"
                  << "       Ensure an AMD Radeon dGPU or Ryzen APU is present and bound to amdgpu.\n";
        all_ok = false;
    }

    std::cout << "\n------------------------------------------------------\n";
    if (all_ok) {
        std::cout << "Diagnosis: Module access, ABI and AMD GPU detection passed.\n";
        return 0;
    } else {
        std::cout << "Diagnosis: 1 or more critical issues detected. Please check the remediation steps above.\n";
        return 1;
    }
}

} // namespace prisma

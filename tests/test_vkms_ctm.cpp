#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <auto free_object, typename T>
auto drm_ptr(T* ptr) {
    return std::unique_ptr<T, decltype(free_object)>(ptr, free_object);
}

void set_prisma(int fd, std::string_view command) {
    require(write(fd, command.data(), command.size()) == static_cast<ssize_t>(command.size()),
            "Could not write Prisma configuration");
}

uint32_t ctm_property(int fd, uint32_t crtc) {
    const auto props = drm_ptr<drmModeFreeObjectProperties>(
        drmModeObjectGetProperties(fd, crtc, DRM_MODE_OBJECT_CRTC));
    require(!!props, "Could not read CRTC properties");
    for (uint32_t i = 0; i < props->count_props; ++i) {
        const auto prop = drm_ptr<drmModeFreeProperty>(drmModeGetProperty(fd, props->props[i]));
        if (prop && std::string_view(prop->name) == "CTM") return prop->prop_id;
    }
    throw std::runtime_error("VKMS must expose a CTM property for this test");
}

uint32_t gamma_property(int fd, uint32_t crtc) {
    const auto props = drm_ptr<drmModeFreeObjectProperties>(
        drmModeObjectGetProperties(fd, crtc, DRM_MODE_OBJECT_CRTC));
    require(!!props, "Could not read CRTC properties");
    for (uint32_t i = 0; i < props->count_props; ++i) {
        const auto prop = drm_ptr<drmModeFreeProperty>(drmModeGetProperty(fd, props->props[i]));
        if (prop && std::string_view(prop->name) == "GAMMA_LUT") return prop->prop_id;
    }
    return 0;
}

uint32_t current_blob(int fd, uint32_t crtc, uint32_t property) {
    const auto props = drm_ptr<drmModeFreeObjectProperties>(
        drmModeObjectGetProperties(fd, crtc, DRM_MODE_OBJECT_CRTC));
    require(!!props, "Could not read CTM state");
    for (uint32_t i = 0; i < props->count_props; ++i) {
        if (props->props[i] == property) return props->prop_values[i];
    }
    throw std::runtime_error("CTM property disappeared");
}

template <typename Predicate>
uint32_t wait_for_ctm(int fd, uint32_t crtc, uint32_t prop, Predicate predicate) {
    for (int i = 0; i < 40; ++i) {
        const auto blob = current_blob(fd, crtc, prop);
        if (predicate(blob)) return blob;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("Timed out waiting for CTM");
}

void set_external_ctm(int fd, uint32_t crtc, uint32_t prop, uint32_t blob) {
    const auto request = drm_ptr<drmModeAtomicFree>(drmModeAtomicAlloc());
    require(!!request, "Could not allocate atomic request");
    require(drmModeAtomicAddProperty(request.get(), crtc, prop, blob) >= 0,
            "Could not add CTM property to request");
    require(drmModeAtomicCommit(fd, request.get(), 0, nullptr) == 0, "External CTM commit failed");
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "Usage: test_vkms_ctm /dev/dri/cardN");
    const int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    require(fd >= 0, "Cannot open DRM device");
    const auto version = drm_ptr<drmFreeVersion>(drmGetVersion(fd));
    require(version && std::string_view(version->name, version->name_len) == "vkms",
            "This test requires an isolated VKMS device");
    const int prisma = open("/dev/chroma_amd", O_WRONLY | O_CLOEXEC);
    require(prisma >= 0, "Cannot open Prisma device");

    const auto resources = drm_ptr<drmModeFreeResources>(drmModeGetResources(fd));
    require(resources && resources->count_crtcs == 1 && resources->count_connectors >= 1,
            "Expected a single CRTC and a connected VKMS output");
    const uint32_t crtc = resources->crtcs[0];
    const uint32_t connector_id = resources->connectors[0];
    const auto connector = drm_ptr<drmModeFreeConnector>(drmModeGetConnector(fd, connector_id));
    require(connector && connector->count_modes > 0, "VKMS output has no modes");
    auto mode = connector->modes[0];
    drm_mode_create_dumb buffer{};
    buffer.width = mode.hdisplay;
    buffer.height = mode.vdisplay;
    buffer.bpp = 32;
    require(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer) == 0, "Cannot create VKMS framebuffer");
    uint32_t framebuffer;
    require(drmModeAddFB(fd, buffer.width, buffer.height, 24, 32, buffer.pitch,
                         buffer.handle, &framebuffer) == 0,
            "Cannot register framebuffer");
    auto active_connector = connector_id;
    require(drmModeSetCrtc(fd, crtc, framebuffer, 0, 0, &active_connector, 1, &mode) == 0,
            "Cannot activate VKMS output");
    require(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "Atomic DRM unavailable");
    const auto property = ctm_property(fd, crtc);
    const auto gamma_prop = gamma_property(fd, crtc);
    require(current_blob(fd, crtc, property) == 0, "Expected default CTM");
    if (gamma_prop != 0) {
        require(current_blob(fd, crtc, gamma_prop) == 0, "Expected default GAMMA_LUT");
    }

    set_prisma(prisma, "enabled=1 saturation=0");
    const auto first = wait_for_ctm(fd, crtc, property, [](auto blob) { return blob != 0; });
    const auto installed = drm_ptr<drmModeFreePropertyBlob>(drmModeGetPropertyBlob(fd, first));
    require(installed && installed->length == sizeof(drm_color_ctm), "Incorrect DRM CTM blob size");
    drm_color_ctm monochrome{};
    std::memcpy(&monochrome, installed->data, sizeof(monochrome));
    const std::array<unsigned, 3> weights{2126, 7152, 722};
    for (size_t i = 0; i < 9; ++i) {
        require(monochrome.matrix[i] == (uint64_t{weights[i % 3]} << 32) / 10000,
                "Unexpected Rec.709 coefficient");
    }

    set_prisma(prisma, "saturation=120 contrast=115 brightness=-20");
    wait_for_ctm(fd, crtc, property, [first](auto blob) { return blob != 0 && blob != first; });
    if (gamma_prop != 0) {
        wait_for_ctm(fd, crtc, gamma_prop, [](auto blob) { return blob != 0; });
    }

    // Test that neutral identity matrix with signed zero is adopted by Prisma
    drm_color_ctm neutral_identity{};
    neutral_identity.matrix[0] = neutral_identity.matrix[4] = neutral_identity.matrix[8] = uint64_t{1} << 32;
    neutral_identity.matrix[1] = uint64_t{1} << 63; // -0.0
    uint32_t identity_id;
    require(drmModeCreatePropertyBlob(fd, &neutral_identity, sizeof(neutral_identity), &identity_id) == 0,
            "Cannot create identity CTM");
    set_external_ctm(fd, crtc, property, identity_id);
    set_prisma(prisma, "enabled=1 saturation=130");
    wait_for_ctm(fd, crtc, property, [identity_id](auto blob) { return blob != 0 && blob != identity_id; });
    require(drmModeDestroyPropertyBlob(fd, identity_id) == 0, "Cannot free identity CTM");

    drm_color_ctm external{};
    external.matrix[0] = external.matrix[4] = external.matrix[8] = uint64_t{1} << 31;
    uint32_t external_id;
    require(drmModeCreatePropertyBlob(fd, &external, sizeof(external), &external_id) == 0,
            "Cannot create external CTM");
    set_external_ctm(fd, crtc, property, external_id);
    std::this_thread::sleep_for(2500ms);
    require(current_blob(fd, crtc, property) == external_id, "Prisma replaced an external CTM");

    set_prisma(prisma, "enabled=0");
    std::this_thread::sleep_for(2500ms);
    require(current_blob(fd, crtc, property) == external_id, "Disable cleared an external CTM");
    set_external_ctm(fd, crtc, property, 0);
    require(drmModeDestroyPropertyBlob(fd, external_id) == 0, "Cannot free external CTM");
    set_prisma(prisma, "enabled=1");
    wait_for_ctm(fd, crtc, property, [](auto blob) { return blob != 0; });
    set_prisma(prisma, "enabled=0");
    wait_for_ctm(fd, crtc, property, [](auto blob) { return blob == 0; });
    if (gamma_prop != 0) {
        wait_for_ctm(fd, crtc, gamma_prop, [](auto blob) { return blob == 0; });
    }

    set_prisma(prisma, "enabled=1");
    wait_for_ctm(fd, crtc, property, [](auto blob) { return blob != 0; });
    close(prisma);
    require(std::system("/bin/rmmod chroma_amd") == 0, "Cannot unload Prisma");
    require(current_blob(fd, crtc, property) == 0, "Unload left the Prisma CTM active");
    if (gamma_prop != 0) {
        require(current_blob(fd, crtc, gamma_prop) == 0, "Unload left the Prisma GAMMA_LUT active");
    }
    close(fd);
    std::cout << "PASS VKMS CTM commit, external ownership, disable and unload\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}

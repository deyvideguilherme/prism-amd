#include "prisma_abi.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <cstddef>
#include <unistd.h>

namespace {

static_assert(PRISMA_ABI_VERSION == 1, "ABI v1 version must remain 1");
static_assert(sizeof(prisma_ioctl_state) == 20, "ABI v1 struct size must remain exactly 20 bytes");
static_assert(offsetof(prisma_ioctl_state, abi_version) == 0, "abi_version offset must be 0");
static_assert(offsetof(prisma_ioctl_state, enabled) == 4, "enabled offset must be 4");
static_assert(offsetof(prisma_ioctl_state, saturation) == 8, "saturation offset must be 8");
static_assert(offsetof(prisma_ioctl_state, contrast) == 12, "contrast offset must be 12");
static_assert(offsetof(prisma_ioctl_state, brightness) == 16, "brightness offset must be 16");

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool same(const prisma_ioctl_state& a, const prisma_ioctl_state& b) {
    return a.abi_version == b.abi_version && a.enabled == b.enabled &&
           a.saturation == b.saturation && a.contrast == b.contrast &&
           a.brightness == b.brightness;
}

prisma_ioctl_state read_state(int fd) {
    prisma_ioctl_state state{};
    require(::ioctl(fd, PRISMA_IOC_GET_STATE, &state) == 0, "GET_STATE failed");
    return state;
}

void write_state(int fd, const prisma_ioctl_state& state) {
    require(::ioctl(fd, PRISMA_IOC_SET_STATE, &state) == 0, "SET_STATE failed");
}

void write_text(int fd, std::string_view text) {
    require(::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()),
            "valid text transaction failed");
}

struct Device {
    int fd;
    prisma_ioctl_state initial;

    Device() : fd(::open("/dev/chroma_amd", O_RDWR | O_CLOEXEC)) {
        require(fd >= 0, "Cannot open /dev/chroma_amd");
        initial = read_state(fd);
    }

    ~Device() {
        if (::ioctl(fd, PRISMA_IOC_SET_STATE, &initial) < 0) {
            std::cerr << "Could not restore initial test configuration\n";
            std::abort();
        }
        ::close(fd);
    }
};

} // namespace

int main() try {
    const Device device;
    const int fd = device.fd;
    uint32_t abi{};
    require(::ioctl(fd, PRISMA_IOC_GET_ABI, &abi) == 0 && abi == PRISMA_ABI_VERSION,
            "GET_ABI failed");
    const prisma_ioctl_state a{abi, 1, 145, 105, -11};
    const prisma_ioctl_state b{abi, 0, 200, 120, 25};

    write_text(fd, "saturation=145 contrast=105 brightness=-11 enabled=1\n");
    require(same(read_state(fd), a), "valid text write had incorrect result");
    for (auto text : {std::string_view("saturation=250 contrast=9"),
                      std::string_view("enabled=0 unknown=1"),
                      std::string_view("brightness=50 saturation="),
                      std::string_view("contrast=130 enabled=1junk"),
                      std::string_view("saturation=180\0enabled=0", 24),
                      std::string_view("saturation=99999999999999999999"),
                      std::string_view("=1"),
                      std::string_view("contrast=\t120")}) {
        require(::write(fd, text.data(), text.size()) == -1 && errno == EINVAL,
                "malformed or invalid text accepted");
        require(same(read_state(fd), a), "rejected text changed settings");
    }
    const std::string oversized(256, ' ');
    require(::write(fd, oversized.data(), oversized.size()) == -1 && errno == EINVAL,
            "oversized write accepted");
    require(::write(fd, "", 0) == 0 && same(read_state(fd), a), "empty write changed state");
    auto invalid = b;
    invalid.brightness = -101;
    require(::ioctl(fd, PRISMA_IOC_SET_STATE, &invalid) == -1 && errno == EINVAL,
            "out-of-range ioctl accepted");
    invalid.abi_version++;
    require(::ioctl(fd, PRISMA_IOC_SET_STATE, &invalid) == -1 && errno == EPROTO,
            "mismatched ABI accepted");
    require(same(read_state(fd), a), "rejected ioctl changed settings");

    std::atomic<bool> ok{true};
    {
        std::jthread full_writer([&] {
            for (int i = 0; i < 2000; ++i) {
                if (::ioctl(fd, PRISMA_IOC_SET_STATE, &b) < 0) ok = false;
            }
        });
        std::jthread text_writer([&] {
            constexpr std::string_view text = "saturation=145 contrast=105 brightness=-11 enabled=1";
            for (int i = 0; i < 2000; ++i) {
                if (::write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) ok = false;
            }
        });
        for (int i = 0; i < 4000; ++i) {
            const auto snapshot = read_state(fd);
            if (!same(snapshot, a) && !same(snapshot, b)) ok = false;
        }
    }
    require(ok, "concurrent reader observed incomplete state or a write failed");

    write_state(fd, a);
    {
        std::jthread saturation([&] { write_text(fd, "saturation=300"); });
        std::jthread contrast([&] { write_text(fd, "contrast=250"); });
    }
    const prisma_ioctl_state combined{abi, 1, 300, 250, -11};
    require(same(read_state(fd), combined), "independent writes lost an update");
    std::cout << "PASS kernel ABI, validation and concurrent transactions\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}

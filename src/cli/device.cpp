#include "device.hpp"
#include "file_descriptor.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <charconv>
#include <sys/stat.h>
#include <sys/ioctl.h>

namespace prisma {

void validate_parameter(std::string_view key, int value) {
    if (key == "saturation") {
        if (value < 0 || value > 400) {
            throw DeviceError("Invalid saturation value: " + std::to_string(value) + " (must be between 0 and 400)");
        }
    } else if (key == "contrast") {
        if (value < 10 || value > 300) {
            throw DeviceError("Invalid contrast value: " + std::to_string(value) + " (must be between 10 and 300)");
        }
    } else if (key == "brightness") {
        if (value < -100 || value > 100) {
            throw DeviceError("Invalid brightness value: " + std::to_string(value) + " (must be between -100 and 100)");
        }
    } else if (key == "enabled") {
        if (value != 0 && value != 1) {
            throw DeviceError("Invalid enabled value: " + std::to_string(value) + " (must be 0 or 1)");
        }
    } else {
        throw DeviceError("Unknown parameter: " + std::string(key));
    }
}

DeviceState parse_state_buffer(std::string_view buffer) {
    DeviceState state;
    size_t start = 0;

    while (start < buffer.size()) {
        size_t end = buffer.find('\n', start);
        if (end == std::string_view::npos) {
            end = buffer.size();
        }

        std::string_view line = buffer.substr(start, end - start);
        start = end + 1;

        // Trim trailing CR
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.empty()) {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        std::string_view key = line.substr(0, eq);
        std::string_view val_str = line.substr(eq + 1);

        int val = 0;
        auto [ptr, ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
        if (ec != std::errc() || ptr != val_str.data() + val_str.size()) {
            continue;
        }

        if (key == "abi_version") {
            state.abi_version = val;
        } else if (key == "saturation") {
            state.saturation = val;
        } else if (key == "contrast") {
            state.contrast = val;
        } else if (key == "brightness") {
            state.brightness = val;
        } else if (key == "enabled") {
            state.enabled = val;
        }
    }

    return state;
}

std::string format_write_command(
    const std::optional<int>& saturation,
    const std::optional<int>& contrast,
    const std::optional<int>& brightness,
    const std::optional<int>& enabled
) {
    std::string cmd;

    if (saturation.has_value()) {
        validate_parameter("saturation", *saturation);
        if (!cmd.empty()) cmd += " ";
        cmd += "saturation=" + std::to_string(*saturation);
    }
    if (contrast.has_value()) {
        validate_parameter("contrast", *contrast);
        if (!cmd.empty()) cmd += " ";
        cmd += "contrast=" + std::to_string(*contrast);
    }
    if (brightness.has_value()) {
        validate_parameter("brightness", *brightness);
        if (!cmd.empty()) cmd += " ";
        cmd += "brightness=" + std::to_string(*brightness);
    }
    if (enabled.has_value()) {
        validate_parameter("enabled", *enabled);
        if (!cmd.empty()) cmd += " ";
        cmd += "enabled=" + std::to_string(*enabled);
    }

    return cmd;
}

static void handle_posix_error(const std::string& dev_path, int err) {
    if (err == ENOENT) {
        throw DeviceError(
            "Device node '" + dev_path + "' not found.\n"
            "Ensure the chroma_amd kernel module is loaded:\n"
            "  sudo make -C kernel load   (or sudo insmod kernel/chroma_amd.ko)"
        );
    }
    if (err == EACCES) {
        throw DeviceError(
            "Permission denied accessing '" + dev_path + "'.\n"
            "Ensure the udev rule is installed and your user is in the 'video' group:\n"
            "  sudo make -C kernel install-udev\n"
            "  sudo usermod -aG video $USER\n"
            "(Remember to log out and log back in if you were just added to the group)"
        );
    }
    if (err == EINVAL) {
        throw DeviceError("Kernel rejected parameter: invalid value or out of range (EINVAL).");
    }
    throw DeviceError("I/O error accessing '" + dev_path + "': " + std::strerror(err));
}

void check_device_abi(int fd, const std::string& dev_path) {
    struct stat st{};
    if (::fstat(fd, &st) < 0) handle_posix_error(dev_path, errno);
    if (!S_ISCHR(st.st_mode)) {
        return; // Skip ioctl validation on regular mock files in tests
    }

    uint32_t kabi = 0;
    int ret = ::ioctl(fd, PRISMA_IOC_GET_ABI, &kabi);
    if (ret < 0) {
        throw DeviceError(
            "Prisma ABI mismatch: kernel module does not support ABI v" +
            std::to_string(PRISMA_ABI_VERSION) + " (outdated module loaded).\n"
            "Please reload the kernel module:\n"
            "  sudo rmmod chroma_amd && sudo modprobe chroma_amd  (or sudo make -C kernel unload && sudo make -C kernel load)"
        );
    }

    if (kabi != PRISMA_ABI_VERSION) {
        throw DeviceError(
            "Prisma ABI version mismatch: kernel module is ABI v" +
            std::to_string(kabi) + ", but userspace expects ABI v" +
            std::to_string(PRISMA_ABI_VERSION) + ".\n"
            "Please reload the kernel module:\n"
            "  sudo rmmod chroma_amd && sudo modprobe chroma_amd  (or sudo make -C kernel unload && sudo make -C kernel load)"
        );
    }
}

DeviceState read_device_state(const std::string& dev_path) {
    FileDescriptor fd(::open(dev_path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        handle_posix_error(dev_path, errno);
    }

    struct stat st{};
    if (::fstat(fd.get(), &st) < 0) handle_posix_error(dev_path, errno);
    if (S_ISCHR(st.st_mode)) {
        check_device_abi(fd.get(), dev_path);

        struct prisma_ioctl_state ist{};
        if (::ioctl(fd.get(), PRISMA_IOC_GET_STATE, &ist) == 0) {
            DeviceState state;
            state.abi_version = static_cast<int>(ist.abi_version);
            state.enabled = ist.enabled;
            state.saturation = ist.saturation;
            state.contrast = ist.contrast;
            state.brightness = ist.brightness;
            return state;
        }
    }

    char buf[512];
    ssize_t bytes_read;
    do {
        bytes_read = ::read(fd.get(), buf, sizeof(buf) - 1);
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read < 0) {
        handle_posix_error(dev_path, errno);
    }

    buf[bytes_read] = '\0';
    return parse_state_buffer(std::string_view(buf, bytes_read));
}

void write_device_params(
    const std::optional<int>& saturation,
    const std::optional<int>& contrast,
    const std::optional<int>& brightness,
    const std::optional<int>& enabled,
    const std::string& dev_path
) {
    std::string cmd = format_write_command(saturation, contrast, brightness, enabled);
    if (cmd.empty()) {
        return;
    }
    cmd += "\n";

    FileDescriptor fd(::open(dev_path.c_str(), O_WRONLY | O_CLOEXEC));
    if (!fd) handle_posix_error(dev_path, errno);
    check_device_abi(fd.get(), dev_path);

    ssize_t bytes_written;
    do {
        bytes_written = ::write(fd.get(), cmd.data(), cmd.size());
    } while (bytes_written < 0 && errno == EINTR);

    if (bytes_written < 0) handle_posix_error(dev_path, errno);
    if (static_cast<size_t>(bytes_written) != cmd.size()) {
        throw DeviceError("Incomplete write to '" + dev_path + "'");
    }
}

void write_device_state(const DeviceState& state, const std::string& dev_path) {
    write_device_params(state.saturation, state.contrast, state.brightness, state.enabled, dev_path);
}

} // namespace prisma

#pragma once

#include "prisma_abi.h"

#include <string>
#include <string_view>
#include <optional>
#include <stdexcept>

namespace prisma {

struct DeviceState {
    int saturation = 100;
    int contrast = 100;
    int brightness = 0;
    int enabled = 1;
    int abi_version = PRISMA_ABI_VERSION;

    bool operator==(const DeviceState& other) const = default;
};

class DeviceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Client-side parameter bounds validation
void validate_parameter(std::string_view key, int value);

// Parses raw key=value text buffer returned by /dev/chroma_amd
DeviceState parse_state_buffer(std::string_view buffer);

// Formats parameters into write buffer for /dev/chroma_amd
std::string format_write_command(
    const std::optional<int>& saturation,
    const std::optional<int>& contrast,
    const std::optional<int>& brightness,
    const std::optional<int>& enabled
);

// Validates ABI compatibility with /dev/chroma_amd
void check_device_abi(int fd, const std::string& dev_path = "/dev/chroma_amd");

// Reads device state from /dev/chroma_amd using POSIX I/O and ioctl
DeviceState read_device_state(const std::string& dev_path = "/dev/chroma_amd");

// Checks whether the device node exists and can be opened
bool is_device_available(const std::string& dev_path = "/dev/chroma_amd");

// Reads device state, or returns nullopt if device node is not available/accessible
std::optional<DeviceState> try_read_device_state(
    const std::string& dev_path = "/dev/chroma_amd",
    std::string* error_msg = nullptr
);

// Writes parameters to /dev/chroma_amd using POSIX I/O and ioctl
void write_device_params(
    const std::optional<int>& saturation,
    const std::optional<int>& contrast,
    const std::optional<int>& brightness,
    const std::optional<int>& enabled,
    const std::string& dev_path = "/dev/chroma_amd"
);

// Writes full state to /dev/chroma_amd
void write_device_state(const DeviceState& state, const std::string& dev_path = "/dev/chroma_amd");

} // namespace prisma

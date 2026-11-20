#pragma once

#include <string>
#include <optional>
#include <filesystem>

namespace prisma {

struct GpuTelemetry {
    std::string card_name;                   // e.g. "card1"
    std::string pci_id;                      // e.g. "0000:2d:00.0"
    std::string device_id;                   // e.g. "0x73df"
    bool is_apu = false;
    std::string gpu_name;                    // e.g. "AMD Radeon RX 9060 XT"
    std::string gpu_type;                    // "dGPU" or "APU"
    std::optional<int> gpu_busy_percent;     // 0..100 %
    std::optional<double> vram_used_mib;     // MiB
    std::optional<double> vram_total_mib;    // MiB
    std::optional<double> gtt_used_mib;      // MiB
    std::optional<double> gtt_total_mib;     // MiB
    std::optional<double> temp_edge_c;       // °C
    std::optional<double> temp_junction_c;   // °C (Hotspot)
    std::optional<double> power_watts;       // W
    std::optional<int> sclk_mhz;             // SCLK clock in MHz
};

// Finds the first AMD GPU (vendor == 0x1002) in the given DRM directory
std::optional<std::filesystem::path> find_amd_gpu_path(
    const std::filesystem::path& drm_root = "/sys/class/drm"
);

// Parses SCLK MHz from pp_dpm_sclk file content (extracts line with '*')
std::optional<int> parse_sclk_mhz(std::string_view sclk_content);

// Reads telemetry directly from AMD sysfs/hwmon tree
GpuTelemetry read_gpu_telemetry(const std::filesystem::path& drm_root = "/sys/class/drm");

// Formats telemetry as standard human-readable text
std::string format_telemetry_text(const GpuTelemetry& t);

// Formats telemetry as JSON
std::string format_telemetry_json(const GpuTelemetry& t);

// Serializes a string safely for JSON
std::string json_string(std::string_view value);

} // namespace prisma

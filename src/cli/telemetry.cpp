#include "telemetry.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <iomanip>
#include <cctype>

namespace prisma {

namespace {

std::string trim(std::string_view str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(first, last - first + 1));
}

std::optional<std::string> read_file_string(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return trim(content);
}

std::optional<uint64_t> read_file_u64(const std::filesystem::path& path) {
    auto content = read_file_string(path);
    if (!content) return std::nullopt;
    try {
        return std::stoull(*content);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int64_t> read_file_s64(const std::filesystem::path& path) {
    auto content = read_file_string(path);
    if (!content) return std::nullopt;
    try {
        return std::stoll(*content);
    } catch (...) {
        return std::nullopt;
    }
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

struct AmdCandidate {
    std::filesystem::path device_dir;
    std::string card_name;
    int score = 0;
};

} // namespace

std::optional<std::filesystem::path> find_amd_gpu_path(const std::filesystem::path& drm_root) {
    std::error_code ec;
    if (!std::filesystem::exists(drm_root, ec)) {
        return std::nullopt;
    }

    std::vector<AmdCandidate> candidates;

    // Inspect entries named card0, card1, etc.
    for (const auto& entry : std::filesystem::directory_iterator(drm_root, ec)) {
        std::string filename = entry.path().filename().string();
        // Match card[0-9]+ without connector suffixes (e.g. card1, not card1-DP-1)
        if (!filename.starts_with("card") || filename.find('-') != std::string::npos) {
            continue;
        }

        std::filesystem::path device_dir = entry.path() / "device";
        std::filesystem::path vendor_file = device_dir / "vendor";

        auto vendor_str = read_file_string(vendor_file);
        bool is_amd_vendor = vendor_str && (*vendor_str == "0x1002" || *vendor_str == "1002");

        // Check if driver symlink points to amdgpu
        bool is_amdgpu_driver = false;
        std::filesystem::path driver_symlink = device_dir / "driver";
        if (std::filesystem::exists(driver_symlink, ec)) {
            auto target = std::filesystem::canonical(driver_symlink, ec);
            if (!ec && target.filename() == "amdgpu") {
                is_amdgpu_driver = true;
            }
        }

        if (!is_amd_vendor && !is_amdgpu_driver) {
            continue;
        }

        AmdCandidate cand;
        cand.device_dir = device_dir;
        cand.card_name = filename;
        cand.score = 10;

        if (is_amd_vendor) cand.score += 5;
        if (is_amdgpu_driver) cand.score += 20;

        // Check for active hwmon sensors
        std::filesystem::path hwmon_dir = device_dir / "hwmon";
        if (std::filesystem::exists(hwmon_dir, ec) && !std::filesystem::is_empty(hwmon_dir, ec)) {
            cand.score += 10;
        }
        if (std::filesystem::exists(device_dir / "gpu_busy_percent", ec)) {
            cand.score += 10;
        }

        // Check for connected display connectors (e.g. card1-DP-1, card0-eDP-1)
        std::string prefix = filename + "-";
        for (const auto& conn : std::filesystem::directory_iterator(drm_root, ec)) {
            std::string conn_name = conn.path().filename().string();
            if (conn_name.starts_with(prefix)) {
                std::filesystem::path status_file = conn.path() / "status";
                auto status_str = read_file_string(status_file);
                if (status_str) {
                    std::string st = *status_str;
                    while (!st.empty() && (st.back() == '\n' || st.back() == '\r' || st.back() == ' '))
                        st.pop_back();
                    if (st == "connected") {
                        cand.score += 100;
                        break;
                    }
                }
            }
        }

        candidates.push_back(cand);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    // Sort descending by score (connected displays & active sensors first)
    std::sort(candidates.begin(), candidates.end(), [](const AmdCandidate& a, const AmdCandidate& b) {
        return a.score > b.score;
    });

    return candidates.front().device_dir;
}

std::optional<int> parse_sclk_mhz(std::string_view sclk_content) {
    std::istringstream stream{std::string(sclk_content)};
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find('*') == std::string::npos) {
            continue;
        }

        // Found active clock line, e.g.: "S: 75Mhz *" or "1: 1800MHz *"
        size_t mhz_pos = line.find("Mhz");
        if (mhz_pos == std::string::npos) {
            mhz_pos = line.find("MHz");
        }
        if (mhz_pos == std::string::npos) {
            mhz_pos = line.find("mhz");
        }

        if (mhz_pos != std::string::npos) {
            // Read digits backwards from mhz_pos
            size_t end_digit = mhz_pos;
            while (end_digit > 0 && std::isspace(static_cast<unsigned char>(line[end_digit - 1]))) {
                end_digit--;
            }
            size_t start_digit = end_digit;
            while (start_digit > 0 && std::isdigit(static_cast<unsigned char>(line[start_digit - 1]))) {
                start_digit--;
            }

            if (start_digit < end_digit) {
                try {
                    return std::stoi(line.substr(start_digit, end_digit - start_digit));
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
    }

    return std::nullopt;
}

GpuTelemetry read_gpu_telemetry(const std::filesystem::path& drm_root) {
    GpuTelemetry telem;

    auto gpu_dev_opt = find_amd_gpu_path(drm_root);
    if (!gpu_dev_opt) {
        return telem;
    }

    std::filesystem::path dev_path = *gpu_dev_opt;
    telem.card_name = dev_path.parent_path().filename().string();

    // Read PCI slot name / device ID
    std::filesystem::path uevent_file = dev_path / "uevent";
    if (auto uevent = read_file_string(uevent_file)) {
        std::istringstream ss(*uevent);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.starts_with("PCI_SLOT_NAME=")) {
                telem.pci_id = line.substr(14);
            } else if (line.starts_with("PCI_ID=")) {
                telem.device_id = line.substr(7);
            }
        }
    }

    // Fallback device ID from 'device' file
    if (telem.device_id.empty()) {
        if (auto did = read_file_string(dev_path / "device")) {
            telem.device_id = *did;
        }
    }

    // GPU Busy %
    if (auto busy = read_file_s64(dev_path / "gpu_busy_percent")) {
        telem.gpu_busy_percent = static_cast<int>(*busy);
    }

    // VRAM Info (bytes -> MiB)
    if (auto v_used = read_file_u64(dev_path / "mem_info_vram_used")) {
        telem.vram_used_mib = static_cast<double>(*v_used) / (1024.0 * 1024.0);
    }
    if (auto v_total = read_file_u64(dev_path / "mem_info_vram_total")) {
        telem.vram_total_mib = static_cast<double>(*v_total) / (1024.0 * 1024.0);
    }

    // Clock (SCLK MHz)
    if (auto sclk_str = read_file_string(dev_path / "pp_dpm_sclk")) {
        telem.sclk_mhz = parse_sclk_mhz(*sclk_str);
    }

    // HWMON directory discovery
    std::filesystem::path hwmon_dir = dev_path / "hwmon";
    if (std::filesystem::exists(hwmon_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(hwmon_dir)) {
            if (!entry.path().filename().string().starts_with("hwmon")) {
                continue;
            }

            std::filesystem::path hw = entry.path();

            // Read temperatures (temp*_label + temp*_input)
            for (int i = 1; i <= 8; ++i) {
                std::string prefix = "temp" + std::to_string(i);
                std::filesystem::path label_file = hw / (prefix + "_label");
                std::filesystem::path input_file = hw / (prefix + "_input");

                if (!std::filesystem::exists(input_file)) {
                    continue;
                }

                auto temp_raw = read_file_s64(input_file);
                if (!temp_raw) continue;
                double temp_c = static_cast<double>(*temp_raw) / 1000.0;

                auto label_str = read_file_string(label_file);
                std::string label = label_str ? *label_str : "";

                if (iequals(label, "edge") || (label.empty() && i == 1 && !telem.temp_edge_c)) {
                    telem.temp_edge_c = temp_c;
                } else if (iequals(label, "junction") || iequals(label, "hotspot")) {
                    telem.temp_junction_c = temp_c;
                }
            }

            // Power (Watts)
            std::filesystem::path p_avg = hw / "power1_average";
            std::filesystem::path p_in = hw / "power1_input";

            if (auto p_val = read_file_u64(p_avg)) {
                telem.power_watts = static_cast<double>(*p_val) / 1000000.0;
            } else if (auto p_in_val = read_file_u64(p_in)) {
                telem.power_watts = static_cast<double>(*p_in_val) / 1000000.0;
            }

            break; // Process primary hwmon
        }
    }

    return telem;
}

std::string format_telemetry_text(const GpuTelemetry& t) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);

    ss << "GPU Hardware:       " << (t.card_name.empty() ? "N/A" : t.card_name);
    if (!t.pci_id.empty()) {
        ss << " (" << t.pci_id << ")";
    }
    ss << "\n";

    ss << "GPU Usage:          ";
    if (t.gpu_busy_percent) ss << *t.gpu_busy_percent << " %\n";
    else ss << "N/A\n";

    ss << "SCLK Clock:         ";
    if (t.sclk_mhz) ss << *t.sclk_mhz << " MHz\n";
    else ss << "N/A\n";

    ss << "VRAM Usage:         ";
    if (t.vram_used_mib && t.vram_total_mib) {
        ss << *t.vram_used_mib << " MiB / " << *t.vram_total_mib << " MiB";
        if (*t.vram_total_mib > 0) {
            double pct = (*t.vram_used_mib / *t.vram_total_mib) * 100.0;
            ss << " (" << pct << " %)";
        }
        ss << "\n";
    } else {
        ss << "N/A\n";
    }

    ss << "Temperature (Edge): ";
    if (t.temp_edge_c) ss << *t.temp_edge_c << " °C\n";
    else ss << "N/A\n";

    ss << "Temperature (Junc): ";
    if (t.temp_junction_c) ss << *t.temp_junction_c << " °C (Hotspot)\n";
    else ss << "N/A\n";

    ss << "Power Consumption:  ";
    if (t.power_watts) ss << *t.power_watts << " W\n";
    else ss << "N/A\n";

    return ss.str();
}

std::string format_telemetry_json(const GpuTelemetry& t) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);

    ss << "{\n";
    ss << "  \"card\": " << (t.card_name.empty() ? "null" : ("\"" + t.card_name + "\"")) << ",\n";
    ss << "  \"pci_id\": " << (t.pci_id.empty() ? "null" : ("\"" + t.pci_id + "\"")) << ",\n";
    ss << "  \"device_id\": " << (t.device_id.empty() ? "null" : ("\"" + t.device_id + "\"")) << ",\n";

    ss << "  \"gpu_busy_percent\": ";
    if (t.gpu_busy_percent) ss << *t.gpu_busy_percent;
    else ss << "null";
    ss << ",\n";

    ss << "  \"sclk_mhz\": ";
    if (t.sclk_mhz) ss << *t.sclk_mhz;
    else ss << "null";
    ss << ",\n";

    ss << "  \"vram_used_mib\": ";
    if (t.vram_used_mib) ss << *t.vram_used_mib;
    else ss << "null";
    ss << ",\n";

    ss << "  \"vram_total_mib\": ";
    if (t.vram_total_mib) ss << *t.vram_total_mib;
    else ss << "null";
    ss << ",\n";

    ss << "  \"temp_edge_c\": ";
    if (t.temp_edge_c) ss << *t.temp_edge_c;
    else ss << "null";
    ss << ",\n";

    ss << "  \"temp_junction_c\": ";
    if (t.temp_junction_c) ss << *t.temp_junction_c;
    else ss << "null";
    ss << ",\n";

    ss << "  \"power_watts\": ";
    if (t.power_watts) ss << *t.power_watts;
    else ss << "null";
    ss << "\n";

    ss << "}";
    return ss.str();
}

} // namespace prisma

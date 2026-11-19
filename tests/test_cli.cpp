#include "cli/device.hpp"
#include "cli/telemetry.hpp"
#include "cli/profiles.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cmath>

using namespace prisma;

void test_device_parser() {
    std::cout << "[test_cli] Running Test 1: Device State Parser & Validation...\n";

    // Test buffer parsing
    std::string mock_dev =
        "enabled=1\n"
        "saturation=180\n"
        "contrast=120\n"
        "brightness=-15\n";

    DeviceState state = parse_state_buffer(mock_dev);
    assert(state.enabled == 1);
    assert(state.saturation == 180);
    assert(state.contrast == 120);
    assert(state.brightness == -15);

    // Test parameter validation
    validate_parameter("saturation", 0);
    validate_parameter("saturation", 400);
    validate_parameter("contrast", 10);
    validate_parameter("contrast", 300);
    validate_parameter("brightness", -100);
    validate_parameter("brightness", 100);
    validate_parameter("enabled", 0);
    validate_parameter("enabled", 1);

    bool caught = false;
    try {
        validate_parameter("saturation", 401);
    } catch (const DeviceError&) {
        caught = true;
    }
    assert(caught && "Saturation > 400 must throw");

    caught = false;
    try {
        validate_parameter("contrast", 9);
    } catch (const DeviceError&) {
        caught = true;
    }
    assert(caught && "Contrast < 10 must throw");

    caught = false;
    try {
        validate_parameter("brightness", -101);
    } catch (const DeviceError&) {
        caught = true;
    }
    assert(caught && "Brightness < -100 must throw");

    caught = false;
    try {
        validate_parameter("enabled", 2);
    } catch (const DeviceError&) {
        caught = true;
    }
    assert(caught && "Enabled != 0|1 must throw");

    std::cout << "  [PASS] Test 1: Device State Parser & Validation passed.\n";
}

void test_telemetry_parser() {
    std::cout << "[test_cli] Running Test 2: SCLK & Mocked Sysfs/HWMON Telemetry Parser...\n";

    // SCLK parser unit test
    std::string sclk_data =
        "0: 300Mhz\n"
        "1: 1800Mhz *\n"
        "2: 2400Mhz\n";
    auto clk = parse_sclk_mhz(sclk_data);
    assert(clk.has_value() && *clk == 1800);

    std::string sclk_data2 = "S: 75Mhz *\n1: 500Mhz\n";
    auto clk2 = parse_sclk_mhz(sclk_data2);
    assert(clk2.has_value() && *clk2 == 75);

    // Setup mock /sys/class/drm hierarchy
    std::filesystem::path mock_root = std::filesystem::temp_directory_path() / "prisma_mock_drm";
    std::filesystem::remove_all(mock_root);

    // Card0 (Intel/NVIDIA - Vendor 0x8086 -> Should be ignored)
    std::filesystem::create_directories(mock_root / "card0" / "device");
    {
        std::ofstream(mock_root / "card0" / "device" / "vendor") << "0x8086\n";
    }

    // Card1 (AMD - Vendor 0x1002 -> Should be discovered)
    std::filesystem::path amd_dev = mock_root / "card1" / "device";
    std::filesystem::create_directories(amd_dev);
    {
        std::ofstream(amd_dev / "vendor") << "0x1002\n";
        std::ofstream(amd_dev / "gpu_busy_percent") << "42\n";
        std::ofstream(amd_dev / "mem_info_vram_total") << "8589934592\n"; // 8192 MiB
        std::ofstream(amd_dev / "mem_info_vram_used") << "2147483648\n";  // 2048 MiB
        std::ofstream(amd_dev / "pp_dpm_sclk") << "0: 500Mhz\n1: 2100MHz *\n";
    }

    std::filesystem::path hwmon_dir = amd_dev / "hwmon" / "hwmon0";
    std::filesystem::create_directories(hwmon_dir);
    {
        std::ofstream(hwmon_dir / "temp1_label") << "edge\n";
        std::ofstream(hwmon_dir / "temp1_input") << "54000\n"; // 54.0 °C
        std::ofstream(hwmon_dir / "temp2_label") << "junction\n";
        std::ofstream(hwmon_dir / "temp2_input") << "68500\n"; // 68.5 °C
        std::ofstream(hwmon_dir / "power1_average") << "85500000\n"; // 85.5 W
    }

    // Read telemetry through mock root
    GpuTelemetry telem = read_gpu_telemetry(mock_root);

    assert(telem.card_name == "card1");
    assert(telem.gpu_busy_percent.has_value() && *telem.gpu_busy_percent == 42);
    assert(telem.sclk_mhz.has_value() && *telem.sclk_mhz == 2100);
    assert(telem.vram_total_mib.has_value() && std::abs(*telem.vram_total_mib - 8192.0) < 0.1);
    assert(telem.vram_used_mib.has_value() && std::abs(*telem.vram_used_mib - 2048.0) < 0.1);
    assert(telem.temp_edge_c.has_value() && std::abs(*telem.temp_edge_c - 54.0) < 0.1);
    assert(telem.temp_junction_c.has_value() && std::abs(*telem.temp_junction_c - 68.5) < 0.1);
    assert(telem.power_watts.has_value() && std::abs(*telem.power_watts - 85.5) < 0.1);

    std::string json = format_telemetry_json(telem);
    assert(json.find("\"gpu_busy_percent\": 42") != std::string::npos);
    assert(json.find("\"sclk_mhz\": 2100") != std::string::npos);

    std::filesystem::remove_all(mock_root);
    std::cout << "  [PASS] Test 2: SCLK & Mocked Sysfs/HWMON Telemetry Parser passed.\n";
}

void test_profiles_manager() {
    std::cout << "[test_cli] Running Test 3: Profile Serialization & Management...\n";

    std::filesystem::path mock_profiles_dir = std::filesystem::temp_directory_path() / "prisma_mock_profiles";
    std::filesystem::remove_all(mock_profiles_dir);

    DeviceState s1{160, 115, 10, 1};
    save_profile("fps_boost", s1, mock_profiles_dir);

    DeviceState s2{100, 100, 0, 1};
    save_profile("default", s2, mock_profiles_dir);

    // List profiles
    auto list = list_profiles(mock_profiles_dir);
    assert(list.size() == 2);
    assert(list[0].name == "default");
    assert(list[1].name == "fps_boost");

    // Load profile
    DeviceState loaded = load_profile("fps_boost", mock_profiles_dir);
    assert(loaded.saturation == 160);
    assert(loaded.contrast == 115);
    assert(loaded.brightness == 10);
    assert(loaded.enabled == 1);

    // Delete profile
    bool deleted = delete_profile("fps_boost", mock_profiles_dir);
    assert(deleted);
    assert(!std::filesystem::exists(mock_profiles_dir / "fps_boost.conf"));

    auto list_after = list_profiles(mock_profiles_dir);
    assert(list_after.size() == 1);
    assert(list_after[0].name == "default");

    std::filesystem::remove_all(mock_profiles_dir);
    std::cout << "  [PASS] Test 3: Profile Serialization & Management passed.\n";
}

int main() {
    std::cout << "=== Prisma CLI & Telemetry Test Suite ===\n";
    test_device_parser();
    test_telemetry_parser();
    test_profiles_manager();
    std::cout << "All CLI unit tests PASSED successfully!\n";
    return 0;
}

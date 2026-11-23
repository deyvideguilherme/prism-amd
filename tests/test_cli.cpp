#include "cli/device.hpp"
#include "cli/telemetry.hpp"
#include "cli/profiles.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <sys/stat.h>

using namespace prisma;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "prisma-tests-XXXXXX").string();
        const auto* result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Cannot create temporary test directory");
        path = result;
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

template<typename Error, typename Action>
void expect_failure(Action action) {
    try {
        action();
    } catch (const Error&) {
        return;
    }
    assert(false && "Operation accepted invalid input");
}

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

    // Test buffer parsing with explicit abi_version
    std::string mock_dev_abi =
        "abi_version=1\n"
        "enabled=1\n"
        "saturation=150\n"
        "contrast=110\n"
        "brightness=5\n";

    DeviceState state_abi = parse_state_buffer(mock_dev_abi);
    assert(state_abi.abi_version == 1);
    assert(state_abi.enabled == 1);
    assert(state_abi.saturation == 150);
    assert(state_abi.contrast == 110);
    assert(state_abi.brightness == 5);

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

    // Test device availability checking
    assert(!is_device_available("/nonexistent_chroma_path"));
    std::string err_msg;
    assert(!try_read_device_state("/nonexistent_chroma_path", &err_msg).has_value());
    assert(!err_msg.empty());

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
    TemporaryDirectory fixture;
    const auto& mock_root = fixture.path;
    assert(!find_amd_gpu_path(mock_root));

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
    assert(!telem.is_apu);
    assert(!telem.gtt_total_mib.has_value());
    assert(!telem.gtt_used_mib.has_value());
    assert(telem.gpu_busy_percent.has_value() && *telem.gpu_busy_percent == 42);
    assert(telem.sclk_mhz.has_value() && *telem.sclk_mhz == 2100);
    assert(telem.vram_total_mib.has_value() && std::abs(*telem.vram_total_mib - 8192.0) < 0.1);
    assert(telem.vram_used_mib.has_value() && std::abs(*telem.vram_used_mib - 2048.0) < 0.1);
    assert(telem.temp_edge_c.has_value() && std::abs(*telem.temp_edge_c - 54.0) < 0.1);
    assert(telem.temp_junction_c.has_value() && std::abs(*telem.temp_junction_c - 68.5) < 0.1);
    assert(telem.power_watts.has_value() && std::abs(*telem.power_watts - 85.5) < 0.1);

    std::string text_dgpu = format_telemetry_text(telem);
    assert(text_dgpu.find("card1 [dGPU]") != std::string::npos);
    assert(text_dgpu.find("GTT Usage:") == std::string::npos);

    std::string json = format_telemetry_json(telem);
    assert(json.find("\"is_apu\": false") != std::string::npos);
    assert(json.find("\"gpu_busy_percent\": 42") != std::string::npos);
    assert(json.find("\"sclk_mhz\": 2100") != std::string::npos);
    assert(json.find("\"gtt_used_mib\": null") != std::string::npos);
    assert(json.find("\"gtt_total_mib\": null") != std::string::npos);

    // Case dGPU with GTT present (e.g. 8192 MiB VRAM + 16384 MiB GTT) -> must remain is_apu == false
    {
        std::ofstream(amd_dev / "mem_info_gtt_total") << "17179869184\n"; // 16384 MiB
        std::ofstream(amd_dev / "mem_info_gtt_used") << "4294967296\n";   // 4096 MiB
        GpuTelemetry telem_dgpu_gtt = read_gpu_telemetry(mock_root);
        assert(!telem_dgpu_gtt.is_apu);
        assert(telem_dgpu_gtt.gtt_total_mib.has_value() && std::abs(*telem_dgpu_gtt.gtt_total_mib - 16384.0) < 0.1);
        assert(telem_dgpu_gtt.gtt_used_mib.has_value() && std::abs(*telem_dgpu_gtt.gtt_used_mib - 4096.0) < 0.1);

        std::string text_dgpu_gtt = format_telemetry_text(telem_dgpu_gtt);
        assert(text_dgpu_gtt.find("card1 [dGPU]") != std::string::npos);
        assert(text_dgpu_gtt.find("GTT Usage:          4096.0 MiB / 16384.0 MiB (25.0 %)") != std::string::npos);

        std::string json_dgpu_gtt = format_telemetry_json(telem_dgpu_gtt);
        assert(json_dgpu_gtt.find("\"is_apu\": false") != std::string::npos);
        assert(json_dgpu_gtt.find("\"gtt_used_mib\": 4096.0") != std::string::npos);
        assert(json_dgpu_gtt.find("\"gtt_total_mib\": 16384.0") != std::string::npos);
    }

    // Case APU simulating Ryzen 5 5600GT (VRAM: 512 MiB, GTT: 8192 MiB, edge temp only)
    {
        TemporaryDirectory apu_fixture;
        const auto& apu_root = apu_fixture.path;
        std::filesystem::path apu_dev = apu_root / "card1" / "device";
        std::filesystem::create_directories(apu_dev);
        std::ofstream(apu_dev / "vendor") << "0x1002\n";
        std::ofstream(apu_dev / "mem_info_vram_total") << "536870912\n"; // 512 MiB
        std::ofstream(apu_dev / "mem_info_vram_used") << "506462208\n";  // 483 MiB
        std::ofstream(apu_dev / "mem_info_gtt_total") << "8589934592\n"; // 8192 MiB
        std::ofstream(apu_dev / "mem_info_gtt_used") << "1073741824\n";  // 1024 MiB

        std::filesystem::path apu_hwmon = apu_dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(apu_hwmon);
        std::ofstream(apu_hwmon / "temp1_label") << "edge\n";
        std::ofstream(apu_hwmon / "temp1_input") << "33500\n"; // 33.5 °C

        GpuTelemetry apu_telem = read_gpu_telemetry(apu_root);
        assert(apu_telem.is_apu);
        assert(apu_telem.vram_total_mib.has_value() && std::abs(*apu_telem.vram_total_mib - 512.0) < 0.1);
        assert(apu_telem.vram_used_mib.has_value() && std::abs(*apu_telem.vram_used_mib - 483.0) < 0.1);
        assert(apu_telem.gtt_total_mib.has_value() && std::abs(*apu_telem.gtt_total_mib - 8192.0) < 0.1);
        assert(apu_telem.gtt_used_mib.has_value() && std::abs(*apu_telem.gtt_used_mib - 1024.0) < 0.1);
        assert(apu_telem.temp_edge_c.has_value() && std::abs(*apu_telem.temp_edge_c - 33.5) < 0.1);
        assert(!apu_telem.temp_junction_c.has_value());

        std::string apu_text = format_telemetry_text(apu_telem);
        assert(apu_text.find("card1 [APU]") != std::string::npos);
        assert(apu_text.find("GTT Usage:          1024.0 MiB / 8192.0 MiB (12.5 %)") != std::string::npos);

        std::string apu_json = format_telemetry_json(apu_telem);
        assert(apu_json.find("\"is_apu\": true") != std::string::npos);
        assert(apu_json.find("\"vram_total_mib\": 512.0") != std::string::npos);
        assert(apu_json.find("\"vram_used_mib\": 483.0") != std::string::npos);
        assert(apu_json.find("\"gtt_total_mib\": 8192.0") != std::string::npos);
        assert(apu_json.find("\"gtt_used_mib\": 1024.0") != std::string::npos);
        assert(apu_json.find("\"temp_edge_c\": 33.5") != std::string::npos);
        assert(apu_json.find("\"temp_junction_c\": null") != std::string::npos);
    }

    const auto second_amd = mock_root / "card2" / "device";
    const auto connected = mock_root / "card2-DP-1";
    std::filesystem::create_directories(second_amd);
    std::filesystem::create_directory(connected);
    std::ofstream(second_amd / "vendor") << "0x1002\n";
    std::ofstream(second_amd / "mem_info_vram_total") << "-1\n";
    std::ofstream(connected / "status") << "connected\n";
    assert(find_amd_gpu_path(mock_root) == second_amd);
    auto telem_invalid = read_gpu_telemetry(mock_root);
    assert(!telem_invalid.vram_total_mib);
    assert(!telem_invalid.is_apu);
    std::filesystem::remove(connected / "status");
    assert(find_amd_gpu_path(mock_root) == amd_dev);

    telem.device_id = "AMD\"\\example\n";
    assert(format_telemetry_json(telem).find(R"(AMD\"\\example\u000a)") != std::string::npos);

    telem.gpu_name = "AMD Radeon RX 9060 XT";
    telem.gpu_type = "dGPU";
    assert(format_telemetry_json(telem).find("\"gpu_name\": \"AMD Radeon RX 9060 XT\"") != std::string::npos);
    assert(format_telemetry_json(telem).find("\"gpu_type\": \"dGPU\"") != std::string::npos);

    // Test decimal formatting with dot regardless of system locale
    {
        struct comma_numpunct : std::numpunct<char> {
            char do_decimal_point() const override { return ','; }
        };
        struct LocaleGuard {
            std::locale old_loc;
            LocaleGuard() : old_loc(std::locale::global(std::locale(std::locale::classic(), new comma_numpunct))) {}
            ~LocaleGuard() { std::locale::global(old_loc); }
        } guard;

        GpuTelemetry loc_telem;
        loc_telem.vram_used_mib = 123.4;
        loc_telem.temp_edge_c = 45.6;
        std::string loc_json = format_telemetry_json(loc_telem);

        assert(loc_json.find("123.4") != std::string::npos);
        assert(loc_json.find("123,4") == std::string::npos);
        assert(loc_json.find("45.6") != std::string::npos);
        assert(loc_json.find("45,6") == std::string::npos);
    }

    std::cout << "  [PASS] Test 2: SCLK & Mocked Sysfs/HWMON Telemetry Parser passed.\n";
}

void test_profiles_manager() {
    std::cout << "[test_cli] Running Test 3: Profile Serialization & Management...\n";

    TemporaryDirectory fixture;
    const auto mock_profiles_dir = fixture.path / "profiles";

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

    assert(!delete_profile("missing", mock_profiles_dir));
    expect_failure<std::system_error>([&] { load_profile("missing", mock_profiles_dir); });

    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "../other", "/tmp/other", "name\\with-backslash",
             "a\nname", std::string("x\0y", 3), std::string(256, 'x')}) {
        expect_failure<std::invalid_argument>([&] { save_profile(name, s1, mock_profiles_dir); });
        expect_failure<std::invalid_argument>([&] { load_profile(name, mock_profiles_dir); });
        expect_failure<std::invalid_argument>([&] { delete_profile(name, mock_profiles_dir); });
    }

    const auto outside = fixture.path / "outside.conf";
    std::ofstream(outside) << "saturation=135\n";
    const auto alias = mock_profiles_dir / "alias.conf";
    std::filesystem::create_symlink(outside, alias);
    expect_failure<std::runtime_error>([&] { load_profile("alias", mock_profiles_dir); });
    expect_failure<std::runtime_error>([&] { save_profile("alias", s1, mock_profiles_dir); });
    expect_failure<std::runtime_error>([&] { delete_profile("alias", mock_profiles_dir); });
    assert(list_profiles(mock_profiles_dir).size() == 1);
    std::filesystem::remove(alias);
    std::filesystem::create_hard_link(outside, alias);
    expect_failure<std::runtime_error>([&] { load_profile("alias", mock_profiles_dir); });
    expect_failure<std::runtime_error>([&] { save_profile("alias", s1, mock_profiles_dir); });
    expect_failure<std::runtime_error>([&] { delete_profile("alias", mock_profiles_dir); });
    assert(std::ifstream(outside).peek() == 's');

    const auto redirect = fixture.path / "redirect";
    std::filesystem::create_directory_symlink(fixture.path, redirect);
    expect_failure<std::system_error>([&] { save_profile("outside", s1, redirect); });
    expect_failure<std::system_error>([&] { load_profile("default", redirect / "profiles"); });
    expect_failure<std::system_error>([&] { delete_profile("outside", redirect); });
    assert(std::filesystem::is_regular_file(outside));

    assert(deserialize_profile("old", "# old\n saturation=130\r\n").state.saturation == 130);
    expect_failure<DeviceError>([] { deserialize_profile("bad", "saturation=120junk\n"); });
    expect_failure<DeviceError>([] { deserialize_profile("bad", "saturation=130\ncontrast=9\n"); });
    expect_failure<DeviceError>([] { deserialize_profile("bad", "# empty\n"); });
    assert(::mkfifo((mock_profiles_dir / "pipe.conf").c_str(), 0600) == 0);
    expect_failure<std::runtime_error>([&] { load_profile("pipe", mock_profiles_dir); });

    save_profile("concurrent", s1, mock_profiles_dir);
    std::jthread writer([&] {
        for (int i = 0; i < 50; ++i) save_profile("concurrent", i % 2 ? s1 : s2, mock_profiles_dir);
    });
    for (int i = 0; i < 100; ++i) {
        const auto version = load_profile("concurrent", mock_profiles_dir);
        assert(version == s1 || version == s2);
    }
    writer.join();
    struct stat st{};
    assert(::stat((mock_profiles_dir / "concurrent.conf").c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);
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

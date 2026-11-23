#include "cli/device.hpp"
#include "cli/telemetry.hpp"
#include "cli/profiles.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

struct CliResult {
    int exit_code;
    std::string output;
};

struct CliProcess {
    pid_t pid;
    std::filesystem::path output;
};

CliProcess start_cli(const std::filesystem::path& config_home,
                     std::initializer_list<std::string_view> arguments,
                     const std::filesystem::path& output) {
    const auto cli = std::filesystem::canonical("/proc/self/exe").parent_path() / "prisma-ctl";
    std::vector<std::string> strings{"prisma-ctl"};
    strings.insert(strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : strings) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::cout.flush();
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        if (!std::freopen(output.c_str(), "w", stdout) ||
            ::setenv("XDG_CONFIG_HOME", config_home.c_str(), 1) != 0) {
            ::_exit(127);
        }
        ::execv(cli.c_str(), argv.data());
        ::_exit(127);
    }
    return {child, output};
}

CliResult wait_cli(const CliProcess& process) {
    int status;
    assert(::waitpid(process.pid, &status, 0) == process.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 127);
    std::ifstream stream(process.output);
    return {WEXITSTATUS(status), std::string(std::istreambuf_iterator<char>(stream), {})};
}

CliResult run_cli(const std::filesystem::path& config_home, std::initializer_list<std::string_view> arguments) {
    return wait_cli(start_cli(config_home, arguments, config_home / "stdout"));
}

void test_profile_list_output() {
    TemporaryDirectory fixture;
    const auto profiles = fixture.path / "prisma" / "profiles";
    const auto list = [&](bool json) {
        const auto result = json ? run_cli(fixture.path, {"profile", "list", "--json"})
                                 : run_cli(fixture.path, {"profile", "list"});
        assert(result.exit_code == 0);
        return result.output;
    };

    assert(list(false) == "No saved profiles found in " + profiles.string() + "\n");
    assert(list(true) == "[]\n");
    save_profile("jogos", {160, 115, 10, 1}, profiles);
    save_profile("Ciné \"HDR\"", {120, 105, -5, 0}, profiles);
    assert(list(false) == "Saved profiles (" + profiles.string() + "):\n"
           "  - Ciné \"HDR\"      [sat=120, con=105, bri=-5, en=0]\n"
           "  - jogos            [sat=160, con=115, bri=10, en=1]\n");
    save_profile("invalid-\xff", {}, profiles);
    assert(list(true) ==
           R"([{"name": "Ciné \"HDR\"", "saturation": 120, "contrast": 105, "brightness": -5, "enabled": 0}, )"
           R"({"name": "jogos", "saturation": 160, "contrast": 115, "brightness": 10, "enabled": 1}])"
           "\n");
}

std::string compact_json(std::string_view json) {
    std::string result;
    bool quoted = false;
    bool escaped = false;
    for (const char ch : json) {
        if (quoted || (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')) {
            result += ch;
        }
        if (ch == '"' && !escaped) {
            quoted = !quoted;
        }
        escaped = quoted && ch == '\\' && !escaped;
    }
    return result;
}

void test_automation_rules() {
    std::cout << "[test_cli] Running automation rule tests...\n";
    TemporaryDirectory fixture;
    const auto profiles = fixture.path / "prisma" / "profiles";
    const auto run = [&](std::initializer_list<std::string_view> arguments) {
        return run_cli(fixture.path, arguments);
    };
    const auto expect_json = [&](std::string_view expected) {
        const auto result = run({"rule", "list", "--json"});
        assert(result.exit_code == 0);
        assert(compact_json(result.output) == expected);
    };
    expect_json(R"({"enabled":false,"default_profile":null,"rules":[]})");
    save_profile("Ciné \"HDR\"", {120, 105, -5, 0}, profiles);
    save_profile("competitivo", {160, 115, 10, 1}, profiles);
    save_profile("fallback", {100, 100, 0, 1}, profiles);

    for (const auto& pattern : {"steam_app_*", "SeMi*End", "*", "steam_*"}) {
        assert(run({"rule", "set", pattern, "fallback"}).exit_code == 0);
    }
    assert(run({"rule", "set", "STEAM_APP_730", "competitivo"}).exit_code == 0);
    assert(run({"rule", "set", "steam_app_*", "Ciné \"HDR\""}).exit_code == 0);
    assert(run({"rule", "set", "*mi123end", "competitivo"}).exit_code == 0);
    assert(run({"rule", "set", "app?id", "competitivo"}).exit_code == 0);
    assert(run({"rule", "default", "Ciné \"HDR\""}).exit_code == 0);
    assert(run({"rule", "enable"}).exit_code == 0);
    expect_json(R"({"enabled":true,"default_profile":"Ciné \"HDR\"","rules":[)"
                R"({"pattern":"*","profile":"fallback"},{"pattern":"*mi123end","profile":"competitivo"},)"
                R"({"pattern":"app?id","profile":"competitivo"},{"pattern":"semi*end","profile":"fallback"},)"
                R"({"pattern":"steam_*","profile":"fallback"},{"pattern":"steam_app_*","profile":"Ciné \"HDR\""},)"
                R"({"pattern":"steam_app_730","profile":"competitivo"}]})");
    const auto text_list = run({"rule", "list"});
    assert(text_list.exit_code == 0);
    assert(text_list.output.find("steam_app_730") != std::string::npos);
    assert(text_list.output.find("Ciné \"HDR\"") != std::string::npos);

    const auto expect_match = [&](std::string_view app_id, std::string_view profile) {
        const auto result = run({"rule", "match", app_id});
        assert(result.exit_code == 0);
        assert(result.output == std::string(profile) + "\n");
    };
    expect_match("STEAM_APP_730", "competitivo");
    expect_match("steam_app_42", "Ciné \"HDR\"");
    expect_match("steam_app_", "Ciné \"HDR\"");
    expect_match("SEMI123END", "competitivo");
    expect_match("app?id", "competitivo");
    expect_match("appXid", "fallback");
    assert(run({"rule", "set", "se*123end", "Ciné \"HDR\""}).exit_code == 0);
    expect_match("semi123end", "competitivo");

    assert(run({"rule", "disable"}).exit_code == 0);
    assert(compact_json(run({"rule", "list", "--json"}).output).starts_with(R"({"enabled":false,)"));
    expect_match("STEAM_APP_730", "competitivo");
    assert(run({"rule", "delete", "STEAM_APP_730"}).exit_code == 0);
    assert(run({"rule", "delete", "steam_app_730"}).exit_code == 1);
    expect_match("STEAM_APP_730", "Ciné \"HDR\"");
    assert(run({"rule", "default", "--clear"}).exit_code == 0);
    assert(compact_json(run({"rule", "list", "--json"}).output).find(R"("default_profile":null)") != std::string::npos);
    assert(run({"rule", "default", "Ciné \"HDR\""}).exit_code == 0);
    assert(run({"rule", "set", "missing", "missing"}).exit_code != 0);
    assert(run({"rule", "default", "missing"}).exit_code != 0);
    std::filesystem::create_symlink(profiles / "competitivo.conf", profiles / "alias.conf");
    assert(run({"rule", "set", "missing", "alias"}).exit_code != 0);
    assert(run({"rule", "default", "alias"}).exit_code != 0);
    const std::string max_pattern(128, 'x');
    assert(run({"rule", "set", max_pattern, "competitivo"}).exit_code == 0);
    expect_match(max_pattern, "competitivo");
    assert(run({"rule", "delete", max_pattern}).exit_code == 0);
    for (const auto& invalid : std::vector<std::string>{"", "app id", "app\tid", "app\nid", "app\x7f", std::string(129, 'x')}) {
        assert(run({"rule", "set", invalid, "fallback"}).exit_code != 0);
        assert(run({"rule", "delete", invalid}).exit_code != 0);
    }

    assert(run({"profile", "delete", "Ciné \"HDR\""}).exit_code == 0);
    expect_json(R"({"enabled":false,"default_profile":null,"rules":[)"
                R"({"pattern":"*","profile":"fallback"},{"pattern":"*mi123end","profile":"competitivo"},)"
                R"({"pattern":"app?id","profile":"competitivo"},{"pattern":"semi*end","profile":"fallback"},)"
                R"({"pattern":"steam_*","profile":"fallback"}]})");
    assert(run({"rule", "delete", "*"}).exit_code == 0);
    const auto miss = run({"rule", "match", "firefox"});
    assert(miss.exit_code == 1 && miss.output.empty());
}

void test_automation_file_safety() {
    TemporaryDirectory fixture;
    const auto config = fixture.path / "prisma";
    const auto file = config / "automation.conf";
    const auto outside = fixture.path / "outside";
    std::filesystem::create_directory(config);
    std::ofstream(outside) << "enabled=1\n";

    const auto expect_rejected = [&] {
        assert(run_cli(fixture.path, {"rule", "list", "--json"}).exit_code != 0);
        assert(run_cli(fixture.path, {"rule", "enable"}).exit_code != 0);
    };
    std::filesystem::create_symlink(outside, file);
    expect_rejected();
    assert(std::filesystem::is_symlink(file));
    std::filesystem::remove(file);
    std::filesystem::create_hard_link(outside, file);
    expect_rejected();
    assert(std::filesystem::equivalent(outside, file));
    std::filesystem::remove(file);
    assert(::mkfifo(file.c_str(), 0600) == 0);
    expect_rejected();
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    expect_rejected();
    std::filesystem::remove(file);
    TemporaryDirectory redirected_fixture;
    const auto redirect = redirected_fixture.path / "redirect";
    std::filesystem::create_directory(redirect);
    std::filesystem::create_directory_symlink(redirect, redirected_fixture.path / "prisma");
    assert(run_cli(redirected_fixture.path, {"rule", "list", "--json"}).exit_code != 0);
    assert(run_cli(redirected_fixture.path, {"rule", "enable"}).exit_code != 0);
    assert(std::filesystem::is_empty(redirect));
    std::string line;
    std::getline(std::ifstream(outside), line);
    assert(line == "enabled=1");
    assert(run_cli(fixture.path, {"rule", "enable"}).exit_code == 0);
    struct stat info{};
    assert(::stat(file.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0600);
}

void test_concurrent_automation_changes() {
    TemporaryDirectory fixture;
    const auto profiles = fixture.path / "prisma" / "profiles";
    save_profile("gaming", {160, 115, 10, 1}, profiles);
    const std::initializer_list<std::string_view> patterns = {"cs2", "dota2", "portal2", "steam_app_*", "steam_app_730", "game-*"};
    std::vector<CliProcess> writes;
    for (const auto pattern : patterns) {
        writes.push_back(start_cli(fixture.path, {"rule", "set", pattern, "gaming"}, fixture.path / pattern));
    }
    for (const auto& process : writes) {
        assert(wait_cli(process).exit_code == 0);
    }
    const auto list = run_cli(fixture.path, {"rule", "list", "--json"});
    assert(list.exit_code == 0);
    for (const auto pattern : patterns) {
        assert(list.output.find('"' + std::string(pattern) + '"') != std::string::npos);
    }

    const auto set = start_cli(fixture.path, {"rule", "set", "eldenring", "gaming"}, fixture.path / "set");
    const auto deletion = start_cli(fixture.path, {"profile", "delete", "gaming"}, fixture.path / "delete");
    assert(wait_cli(deletion).exit_code == 0);
    const int result = wait_cli(set).exit_code;
    assert(result == 0 || result == 1);
    assert(compact_json(run_cli(fixture.path, {"rule", "list", "--json"}).output) ==
           R"({"enabled":false,"default_profile":null,"rules":[]})");
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

void test_power_cap_and_apu_heuristics() {
    std::cout << "[test_cli] Running Test 4: Power Cap & APU/dGPU Heuristics...\n";

    // Sub-test (a): power1_cap reading and fallback to power1_cap_default
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(hw / "power1_cap") << "180000000\n"; // 180.0 W

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(t.power_cap_watts.has_value() && std::abs(*t.power_cap_watts - 180.0) < 0.1);
        std::string json = format_telemetry_json(t);
        assert(json.find("\"power_cap_watts\": 180.0") != std::string::npos);
        std::string text = format_telemetry_text(t);
        assert(text.find("(Cap: 180.0 W)") != std::string::npos);
    }
    // Sub-test (a2): fallback to power1_cap_default when power1_cap is missing
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(hw / "power1_cap_default") << "65000000\n"; // 65.0 W

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(t.power_cap_watts.has_value() && std::abs(*t.power_cap_watts - 65.0) < 0.1);
        std::string json = format_telemetry_json(t);
        assert(json.find("\"power_cap_watts\": 65.0") != std::string::npos);
    }
    // Sub-test (a3): fallback to power1_cap_default when power1_cap is <= 0
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(hw / "power1_cap") << "0\n";
        std::ofstream(hw / "power1_cap_default") << "95000000\n"; // 95.0 W

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(t.power_cap_watts.has_value() && std::abs(*t.power_cap_watts - 95.0) < 0.1);
    }
    // Sub-test (a4): null when neither power1_cap nor power1_cap_default is present
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(!t.power_cap_watts.has_value());
        std::string json = format_telemetry_json(t);
        assert(json.find("\"power_cap_watts\": null") != std::string::npos);
    }

    // Sub-test (b): APU with 4 GiB VRAM in BIOS (and GTT of 12 GiB, no fan/mem/hotspot) -> is_apu: true
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(dev / "mem_info_vram_total") << "4294967296\n";  // 4096 MiB (4 GiB)
        std::ofstream(dev / "mem_info_gtt_total") << "12884901888\n"; // 12288 MiB (12 GiB)
        std::ofstream(hw / "temp1_label") << "edge\n";
        std::ofstream(hw / "temp1_input") << "40000\n";

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(t.is_apu);
        assert(t.gpu_type == "APU");
        assert(t.gpu_name == "AMD Radeon Graphics (APU)");
        std::string json = format_telemetry_json(t);
        assert(json.find("\"is_apu\": true") != std::string::npos);
    }

    // Sub-test (c1): Unknown dGPU with 2 GiB VRAM but fan1_input present -> is_apu: false
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(dev / "mem_info_vram_total") << "2147483648\n";  // 2048 MiB
        std::ofstream(dev / "mem_info_gtt_total") << "8589934592\n";  // 8192 MiB
        std::ofstream(hw / "temp1_label") << "edge\n";
        std::ofstream(hw / "temp1_input") << "45000\n";
        std::ofstream(hw / "fan1_input") << "1200\n"; // Dedicated fan

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(!t.is_apu);
        assert(t.gpu_type == "dGPU");
        std::string json = format_telemetry_json(t);
        assert(json.find("\"is_apu\": false") != std::string::npos);
    }

    // Sub-test (c2): Unknown dGPU with 2 GiB VRAM but pwm1 present -> is_apu: false
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(dev / "mem_info_vram_total") << "2147483648\n";  // 2048 MiB
        std::ofstream(dev / "mem_info_gtt_total") << "8589934592\n";  // 8192 MiB
        std::ofstream(hw / "pwm1") << "128\n";

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(!t.is_apu);
        assert(t.gpu_type == "dGPU");
    }

    // Sub-test (c3): Unknown dGPU with 2 GiB VRAM but power_cap_watts > 95W -> is_apu: false
    {
        TemporaryDirectory fixture;
        const auto& root = fixture.path;
        auto dev = root / "card1" / "device";
        auto hw = dev / "hwmon" / "hwmon0";
        std::filesystem::create_directories(hw);
        std::ofstream(dev / "vendor") << "0x1002\n";
        std::ofstream(dev / "mem_info_vram_total") << "2147483648\n";  // 2048 MiB
        std::ofstream(dev / "mem_info_gtt_total") << "8589934592\n";  // 8192 MiB
        std::ofstream(hw / "power1_cap") << "130000000\n"; // 130 W

        GpuTelemetry t = read_gpu_telemetry(root);
        assert(!t.is_apu);
        assert(t.gpu_type == "dGPU");
    }

    // Sub-test (d): Correct identification via known PCI table (1002:73bf, 1002:744c, 1002:1681)
    {
        // 1002:73bf -> RX 6800 / 6800 XT / 6900 XT (dGPU)
        {
            TemporaryDirectory fixture;
            const auto& root = fixture.path;
            auto dev = root / "card1" / "device";
            std::filesystem::create_directories(dev);
            std::ofstream(dev / "vendor") << "0x1002\n";
            std::ofstream(dev / "device") << "0x73bf\n";

            GpuTelemetry t = read_gpu_telemetry(root);
            assert(!t.is_apu);
            assert(t.gpu_type == "dGPU");
            assert(t.gpu_name == "AMD Radeon RX 6800 / 6800 XT / 6900 XT");
        }
        // 1002:744c -> RX 7900 XT / XTX (dGPU)
        {
            TemporaryDirectory fixture;
            const auto& root = fixture.path;
            auto dev = root / "card1" / "device";
            std::filesystem::create_directories(dev);
            std::ofstream(dev / "vendor") << "0x1002\n";
            std::ofstream(dev / "device") << "0x744c\n";

            GpuTelemetry t = read_gpu_telemetry(root);
            assert(!t.is_apu);
            assert(t.gpu_type == "dGPU");
            assert(t.gpu_name == "AMD Radeon RX 7900 XT / XTX");
        }
        // 1002:1681 -> AMD Radeon 680M (Rembrandt) (APU)
        {
            TemporaryDirectory fixture;
            const auto& root = fixture.path;
            auto dev = root / "card1" / "device";
            std::filesystem::create_directories(dev);
            std::ofstream(dev / "vendor") << "0x1002\n";
            std::ofstream(dev / "device") << "0x1681\n";

            GpuTelemetry t = read_gpu_telemetry(root);
            assert(t.is_apu);
            assert(t.gpu_type == "APU");
            assert(t.gpu_name == "AMD Radeon 680M (Rembrandt)");
        }
    }

    std::cout << "  [PASS] Test 4: Power Cap & APU/dGPU Heuristics passed.\n";
}

int main() {
    std::cout << "=== Prisma CLI & Telemetry Test Suite ===\n";
    test_device_parser();
    test_telemetry_parser();
    test_profiles_manager();
    test_profile_list_output();
    test_automation_rules();
    test_automation_file_safety();
    test_concurrent_automation_changes();
    test_power_cap_and_apu_heuristics();
    std::cout << "All CLI unit tests PASSED successfully!\n";
    return 0;
}

#include "device.hpp"
#include "telemetry.hpp"
#include "profiles.hpp"
#include "doctor.hpp"
#include "prisma_abi.h"

#include <iostream>
#include <vector>
#include <string>
#include <optional>
#include <iomanip>

namespace {

void print_help(const char* prog) {
    std::cout << "prisma-ctl - Hardware Display & Color Controller for AMD GPUs\n\n"
              << "Usage:\n"
              << "  " << prog << " status [--json]                Display current color state and GPU telemetry\n"
              << "  " << prog << " set [options]                  Adjust hardware color parameters\n"
              << "  " << prog << " telemetry [--json]             Show AMD GPU telemetry sensors\n"
              << "  " << prog << " profile <action> [name]        Manage saved color profiles\n"
              << "  " << prog << " doctor                         Run system and hardware self-diagnostics\n"
              << "  " << prog << " --version | -v                 Show version and protocol ABI information\n"
              << "  " << prog << " --help | -h                    Show this help message\n\n"
              << "Set Options:\n"
              << "  -s, --saturation <0..400>   Set saturation (Digital Vibrance, default 100)\n"
              << "  -c, --contrast <10..300>    Set contrast factor (default 100)\n"
              << "  -b, --brightness <-100..100> Set linear brightness gain (default 0)\n"
              << "  -e, --enabled <0|1>         Enable (1) or disable (0) hardware color injection\n\n"
              << "Profile Actions:\n"
              << "  save <name>                 Save current parameters to ~/.config/prisma/profiles/<name>.conf\n"
              << "  apply <name>                Load and apply parameters from profile\n"
              << "  list                        List all saved profiles\n"
              << "  delete <name>               Delete a saved profile\n"
              << "  reset                       Reset parameters to default (sat=100 con=100 bri=0 en=1)\n\n"
              << "Examples:\n"
              << "  " << prog << " set -s 160 -c 115\n"
              << "  " << prog << " profile save gaming\n"
              << "  " << prog << " profile apply gaming\n"
              << "  " << prog << " doctor\n"
              << "  " << prog << " status --json\n";
}

int handle_status(bool json_output) {
    try {
        prisma::DeviceState state = prisma::read_device_state();
        prisma::GpuTelemetry telem = prisma::read_gpu_telemetry();

        if (json_output) {
            std::cout << "{\n"
                      << "  \"device\": {\n"
                      << "    \"enabled\": " << state.enabled << ",\n"
                      << "    \"saturation\": " << state.saturation << ",\n"
                      << "    \"contrast\": " << state.contrast << ",\n"
                      << "    \"brightness\": " << state.brightness << "\n"
                      << "  },\n"
                      << "  \"telemetry\": " << prisma::format_telemetry_json(telem) << "\n"
                      << "}\n";
        } else {
            std::cout << "=== Prisma Color Configuration (/dev/chroma_amd) ===\n"
                      << "Enabled:            " << (state.enabled ? "yes" : "no") << "\n"
                      << "Saturation:         " << state.saturation << " % (Rec.709 CTM)\n"
                      << "Contrast:           " << state.contrast << " %\n"
                      << "Brightness:         " << state.brightness << " % (CTM linear gain)\n\n"
                      << "=== AMD GPU Hardware Telemetry ===\n"
                      << prisma::format_telemetry_text(telem);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int handle_telemetry(bool json_output) {
    try {
        prisma::GpuTelemetry telem = prisma::read_gpu_telemetry();
        if (json_output) {
            std::cout << prisma::format_telemetry_json(telem) << "\n";
        } else {
            std::cout << "=== AMD GPU Hardware Telemetry ===\n"
                      << prisma::format_telemetry_text(telem);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int handle_set(int argc, char* argv[]) {
    std::optional<int> saturation;
    std::optional<int> contrast;
    std::optional<int> brightness;
    std::optional<int> enabled;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::optional<int>* destination;
        std::string_view key;
        if (arg == "-s" || arg == "--saturation") {
            destination = &saturation;
            key = "saturation";
        } else if (arg == "-c" || arg == "--contrast") {
            destination = &contrast;
            key = "contrast";
        } else if (arg == "-b" || arg == "--brightness") {
            destination = &brightness;
            key = "brightness";
        } else if (arg == "-e" || arg == "--enabled") {
            destination = &enabled;
            key = "enabled";
        } else {
            std::cerr << "Unknown set option: " << arg << "\n";
            return 1;
        }
        if (++i >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            return 1;
        }

        const std::string input = argv[i];
        try {
            size_t consumed;
            const int value = std::stoi(input, &consumed);
            if (consumed != input.size()) throw std::invalid_argument("trailing characters");
            prisma::validate_parameter(key, value);
            *destination = value;
        } catch (const prisma::DeviceError& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        } catch (const std::logic_error&) {
            std::cerr << "Invalid integer for " << arg << "\n";
            return 1;
        }
    }

    if (!saturation && !contrast && !brightness && !enabled) {
        std::cerr << "No parameters specified. Use -s, -c, -b or -e.\n";
        return 1;
    }

    try {
        prisma::write_device_params(saturation, contrast, brightness, enabled);
        prisma::DeviceState cur = prisma::read_device_state();
        std::cout << "Color parameters accepted by the module:\n"
                  << "  Enabled:    " << (cur.enabled ? "yes" : "no") << "\n"
                  << "  Saturation: " << cur.saturation << " %\n"
                  << "  Contrast:   " << cur.contrast << " %\n"
                  << "  Brightness: " << cur.brightness << " %\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int handle_profile(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Missing profile action. Usage: prisma-ctl profile <save|apply|list|delete|reset> [name]\n";
        return 1;
    }

    std::string action = argv[2];

    try {
        if (action == "list") {
            auto profiles = prisma::list_profiles();
            if (profiles.empty()) {
                std::cout << "No saved profiles found in " << prisma::get_profiles_directory().string() << "\n";
            } else {
                std::cout << "Saved profiles (" << prisma::get_profiles_directory().string() << "):\n";
                for (const auto& p : profiles) {
                    std::cout << "  - " << std::left << std::setw(16) << p.name
                              << " [sat=" << p.state.saturation
                              << ", con=" << p.state.contrast
                              << ", bri=" << p.state.brightness
                              << ", en=" << p.state.enabled << "]\n";
                }
            }
            return 0;
        }

        if (action == "reset") {
            prisma::DeviceState default_state{100, 100, 0, 1};
            prisma::write_device_state(default_state);
            std::cout << "Color parameters reset to hardware defaults (saturation=100, contrast=100, brightness=0, enabled=1).\n";
            return 0;
        }

        if (argc < 4) {
            std::cerr << "Missing profile name. Usage: prisma-ctl profile " << action << " <name>\n";
            return 1;
        }

        std::string name = argv[3];

        if (action == "save") {
            prisma::DeviceState state = prisma::read_device_state();
            prisma::save_profile(name, state);
            std::cout << "Profile '" << name << "' saved successfully:\n"
                      << "  Saturation: " << state.saturation << " %\n"
                      << "  Contrast:   " << state.contrast << " %\n"
                      << "  Brightness: " << state.brightness << " %\n"
                      << "  Enabled:    " << (state.enabled ? "yes" : "no") << "\n";
            return 0;
        }

        if (action == "apply") {
            prisma::DeviceState state = prisma::load_profile(name);
            prisma::write_device_state(state);
            std::cout << "Profile '" << name << "' submitted to the color module:\n"
                      << "  Saturation: " << state.saturation << " %\n"
                      << "  Contrast:   " << state.contrast << " %\n"
                      << "  Brightness: " << state.brightness << " %\n"
                      << "  Enabled:    " << (state.enabled ? "yes" : "no") << "\n";
            return 0;
        }

        if (action == "delete") {
            if (prisma::delete_profile(name)) {
                std::cout << "Profile '" << name << "' deleted.\n";
            } else {
                std::cerr << "Profile '" << name << "' not found.\n";
                return 1;
            }
            return 0;
        }

        std::cerr << "Unknown profile action: '" << action << "'. Use save, apply, list, delete, or reset.\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_help(argv[0]);
        return 0;
    }

    if (cmd == "--version" || cmd == "-v" || cmd == "version") {
        std::cout << "prisma-ctl " << PRISMA_VERSION << " (ABI v" << PRISMA_ABI_VERSION << ")\n";
        return 0;
    }

    if (cmd == "doctor") {
        return prisma::run_doctor();
    }

    if (cmd == "status") {
        bool json = (argc >= 3 && std::string(argv[2]) == "--json");
        return handle_status(json);
    }

    if (cmd == "telemetry") {
        bool json = (argc >= 3 && std::string(argv[2]) == "--json");
        return handle_telemetry(json);
    }

    if (cmd == "set") {
        return handle_set(argc, argv);
    }

    if (cmd == "profile") {
        return handle_profile(argc, argv);
    }

    std::cerr << "Unknown command: '" << cmd << "'\n\n";
    print_help(argv[0]);
    return 1;
}

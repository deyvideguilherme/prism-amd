#include "profiles.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace prisma {

std::filesystem::path get_profiles_directory() {
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        return std::filesystem::path(xdg_config) / "prisma" / "profiles";
    }

    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".config" / "prisma" / "profiles";
    }

    return std::filesystem::current_path() / "profiles";
}

std::string serialize_profile(const Profile& profile) {
    std::ostringstream ss;
    ss << "# Prisma Color Profile: " << profile.name << "\n";
    ss << "saturation=" << profile.state.saturation << "\n";
    ss << "contrast=" << profile.state.contrast << "\n";
    ss << "brightness=" << profile.state.brightness << "\n";
    ss << "enabled=" << profile.state.enabled << "\n";
    return ss.str();
}

Profile deserialize_profile(std::string_view name, std::string_view content) {
    Profile profile;
    profile.name = std::string(name);
    profile.state = parse_state_buffer(content);
    return profile;
}

void save_profile(const std::string& name, const DeviceState& state, const std::filesystem::path& dir) {
    if (name.empty()) {
        throw std::invalid_argument("Profile name cannot be empty");
    }

    // Check for path traversal characters
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name == "." || name == "..") {
        throw std::invalid_argument("Invalid profile name: " + name);
    }

    std::filesystem::create_directories(dir);

    Profile p{name, state};
    std::filesystem::path file_path = dir / (name + ".conf");

    std::ofstream out(file_path);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + file_path.string());
    }

    out << serialize_profile(p);
}

DeviceState load_profile(const std::string& name, const std::filesystem::path& dir) {
    if (name.empty()) {
        throw std::invalid_argument("Profile name cannot be empty");
    }

    std::filesystem::path file_path = dir / (name + ".conf");
    if (!std::filesystem::exists(file_path)) {
        throw std::runtime_error("Profile '" + name + "' not found at: " + file_path.string());
    }

    std::ifstream in(file_path);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open profile file: " + file_path.string());
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Profile p = deserialize_profile(name, content);
    return p.state;
}

std::vector<Profile> list_profiles(const std::filesystem::path& dir) {
    std::vector<Profile> list;
    if (!std::filesystem::exists(dir)) {
        return list;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".conf") continue;

        std::string stem = entry.path().stem().string();
        std::ifstream in(entry.path());
        if (!in.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        list.push_back(deserialize_profile(stem, content));
    }

    std::sort(list.begin(), list.end(), [](const Profile& a, const Profile& b) {
        return a.name < b.name;
    });

    return list;
}

bool delete_profile(const std::string& name, const std::filesystem::path& dir) {
    std::filesystem::path file_path = dir / (name + ".conf");
    if (!std::filesystem::exists(file_path)) {
        return false;
    }
    return std::filesystem::remove(file_path);
}

} // namespace prisma

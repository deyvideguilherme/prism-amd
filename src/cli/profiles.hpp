#pragma once

#include "device.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace prisma {

struct Profile {
    std::string name;
    DeviceState state;
};

// Returns standard profiles directory: ~/.config/prisma/profiles/
std::filesystem::path get_profiles_directory();

// Serializes a profile to INI string format
std::string serialize_profile(const Profile& profile);

// Deserializes a profile from INI string format
Profile deserialize_profile(std::string_view name, std::string_view content);

// Saves a profile to disk (~/.config/prisma/profiles/<name>.conf)
void save_profile(const std::string& name, const DeviceState& state, const std::filesystem::path& dir = get_profiles_directory());

// Loads a profile from disk (~/.config/prisma/profiles/<name>.conf)
DeviceState load_profile(const std::string& name, const std::filesystem::path& dir = get_profiles_directory());

// Lists all saved profiles in the directory
std::vector<Profile> list_profiles(const std::filesystem::path& dir = get_profiles_directory());

std::string format_profiles_json(const std::vector<Profile>& profiles);

// Deletes a profile from disk
bool delete_profile(const std::string& name, const std::filesystem::path& dir = get_profiles_directory());

} // namespace prisma

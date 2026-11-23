#include "profiles.hpp"
#include "file_descriptor.hpp"
#include "telemetry.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <locale>
#include <memory>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>

namespace prisma {
namespace {

constexpr size_t max_profile_size = 16 * 1024;
constexpr size_t max_automation_size = size_t{1024} * 1024;
constexpr size_t max_pattern_size = 128;
constexpr std::string_view profile_extension = ".conf";
constexpr char automation_filename[] = "automation.conf";

[[noreturn]] void profile_error(const std::string& message, int error = errno) {
    throw std::system_error(error, std::generic_category(), message);
}

std::string profile_filename(std::string_view name) {
    if (name.empty() || name == "." || name == ".." ||
        name.size() > NAME_MAX - profile_extension.size() ||
        std::ranges::any_of(name, [](unsigned char c) {
            return c <= 0x1f || c == 0x7f || c == '/' || c == '\\';
        })) {
        throw std::invalid_argument("Invalid profile name");
    }
    return std::string(name) + std::string(profile_extension);
}

// Linux filenames may contain bytes that cannot represent a JSON string.
bool is_utf8(std::string_view name) {
    for (size_t i = 0; i < name.size();) {
        const unsigned char lead = name[i++];
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        const size_t tails = lead < 0xe0 ? 1 : (lead < 0xf0 ? 2 : 3);
        if (name.size() - i < tails) return false;
        const unsigned char second = name[i];
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return false;
        for (size_t end = i + tails; i < end; ++i) {
            if ((name[i] & 0xc0) != 0x80) return false;
        }
    }
    return true;
}

void check_config_directory(int dir) {
    struct stat st{};
    if (::fstat(dir, &st) < 0) profile_error("Cannot inspect configuration directory");
    if (st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        profile_error("Configuration directory must be owned by the current user and writable only by its owner", EACCES);
    }
}

FileDescriptor open_config_directory(const std::filesystem::path& dir, bool create = false) {
    const auto path = std::filesystem::absolute(dir).lexically_normal().relative_path();
    constexpr int flags = O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
    FileDescriptor parent(::open("/", flags | O_PATH));
    if (!parent) profile_error("Cannot open filesystem root");

    for (auto it = path.begin(); it != path.end(); ++it) {
        if (it->empty()) continue;
        const bool last = std::next(it) == path.end() || std::next(it)->empty();
        const int mode = flags | (last ? O_RDONLY : O_PATH);
        FileDescriptor child(::openat(parent.get(), it->c_str(), mode));
        if (!child && errno == ENOENT && create) {
            if (::mkdirat(parent.get(), it->c_str(), 0700) < 0 && errno != EEXIST) {
                profile_error("Cannot create configuration directory " + dir.string());
            }
            child = FileDescriptor(::openat(parent.get(), it->c_str(), mode));
        }
        if (!child) {
            if (!create && errno == ENOENT) return FileDescriptor();
            profile_error("Cannot open configuration directory " + dir.string());
        }
        parent = std::move(child);
    }

    check_config_directory(parent.get());
    return parent;
}

void check_config_file(const struct stat& st, const std::string& filename) {
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink > 1 ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        throw std::runtime_error(filename + " must be a regular user-owned file without aliases or shared write access");
    }
}

bool existing_config_file(int dir, const std::string& filename) {
    struct stat st{};
    if (::fstatat(dir, filename.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) return false;
        profile_error("Cannot inspect " + filename);
    }
    check_config_file(st, filename);
    return true;
}

std::string_view trim(std::string_view input) {
    const auto first = input.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return input.substr(first, input.find_last_not_of(" \t\r") - first + 1);
}

std::string read_config_file(int dir, const std::string& filename, size_t limit) {
    FileDescriptor file(::openat(dir, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) profile_error("Cannot open " + filename);

    struct stat st{};
    if (::fstat(file.get(), &st) < 0) profile_error("Cannot inspect " + filename);
    check_config_file(st, filename);

    std::string content;
    char buf[4096];
    for (;;) {
        const ssize_t size = ::read(file.get(), buf, sizeof(buf));
        if (size == 0) break;
        if (size < 0) {
            if (errno == EINTR) continue;
            profile_error("Cannot read " + filename);
        }
        if (static_cast<size_t>(size) > limit - content.size()) {
            throw std::runtime_error(filename + " is too large");
        }
        content.append(buf, static_cast<size_t>(size));
    }
    return content;
}

DeviceState read_profile(int dir, std::string_view name, const std::string& filename) {
    return deserialize_profile(name, read_config_file(dir, filename, max_profile_size)).state;
}

void write_config_file(int dir, const std::string& filename, std::string_view content) {
    existing_config_file(dir, filename);

    std::string temp;
    FileDescriptor file;
    do {
        uint64_t id;
        if (::getentropy(&id, sizeof(id)) < 0) profile_error("Cannot generate temporary configuration filename");
        temp = ".prisma-" + std::to_string(id);
        file = FileDescriptor(::openat(dir, temp.c_str(),
                                       O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    } while (!file && errno == EEXIST);
    if (!file) profile_error("Cannot create temporary configuration file");

    try {
        while (!content.empty()) {
            const ssize_t size = ::write(file.get(), content.data(), content.size());
            if (size <= 0) {
                if (size < 0 && errno == EINTR) continue;
                profile_error("Cannot write " + filename, size == 0 ? EIO : errno);
            }
            content.remove_prefix(static_cast<size_t>(size));
        }
        if (::fsync(file.get()) < 0) profile_error("Cannot sync " + filename);
        if (::renameat(dir, temp.c_str(), dir, filename.c_str()) < 0) {
            profile_error("Cannot publish " + filename);
        }
    } catch (...) {
        ::unlinkat(dir, temp.c_str(), 0);
        throw;
    }
    if (::fsync(dir) < 0) profile_error("Cannot sync configuration directory");
}

FileDescriptor open_automation_directory(bool create = false) {
    return open_config_directory(get_profiles_directory().parent_path(), create);
}

FileDescriptor open_saved_profiles(int dir) {
    FileDescriptor profiles(::openat(dir, "profiles", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!profiles) {
        if (errno == ENOENT) return FileDescriptor();
        profile_error("Cannot open profile directory");
    }
    check_config_directory(profiles.get());
    return profiles;
}

void lock_automation(int dir) {
    // The directory remains stable across atomic replacements of automation.conf.
    while (::flock(dir, LOCK_EX) < 0) {
        if (errno != EINTR) profile_error("Cannot lock automation configuration");
    }
}

std::string lowercase_ascii(std::string_view value) {
    std::string lower(value);
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    return lower;
}

std::string normalize_pattern(std::string_view pattern) {
    if (pattern.empty() || pattern.size() > max_pattern_size ||
        std::ranges::any_of(pattern, [](unsigned char c) { return c < 0x21 || c > 0x7e; })) {
        throw std::invalid_argument("Invalid app_id pattern: use 1 to 128 printable ASCII characters without spaces");
    }
    return lowercase_ascii(pattern);
}

std::string automation_profile_filename(std::string_view profile) {
    if (!is_utf8(profile)) throw std::invalid_argument("Automation profile name must be valid UTF-8");
    return profile_filename(profile);
}

void check_automation_profile(int dir, const std::string& profile) {
    const auto filename = automation_profile_filename(profile);
    auto profiles = open_saved_profiles(dir);
    if (!profiles || !existing_config_file(profiles.get(), filename)) {
        profile_error("Profile '" + profile + "' not found", ENOENT);
    }
    (void)read_profile(profiles.get(), profile, filename);
}

Automation read_automation(int dir) {
    Automation automation;
    if (!existing_config_file(dir, automation_filename)) return automation;

    const auto stored = read_config_file(dir, automation_filename, max_automation_size);
    std::string_view content(stored);
    while (!content.empty()) {
        const auto end = content.find('\n');
        auto line = content.substr(0, end);
        content.remove_prefix(end == std::string_view::npos ? content.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.starts_with('#')) continue;

        if (line == "enabled=0" || line == "enabled=1") {
            automation.enabled = line.back() == '1';
        } else if (line.starts_with("default_profile=")) {
            const auto profile = line.substr(std::string_view("default_profile=").size());
            (void)automation_profile_filename(profile);
            automation.default_profile = profile;
        } else if (line.starts_with("rule=")) {
            line.remove_prefix(std::string_view("rule=").size());
            const auto separator = line.find('\t');
            if (separator == std::string_view::npos) throw std::runtime_error("Missing profile in automation rule");
            auto pattern = normalize_pattern(line.substr(0, separator));
            const auto profile = line.substr(separator + 1);
            (void)automation_profile_filename(profile);
            if (!automation.rules.try_emplace(std::move(pattern), profile).second) {
                throw std::runtime_error("Duplicate app_id pattern in automation configuration");
            }
        } else {
            throw std::runtime_error("Invalid automation configuration entry");
        }
    }
    return automation;
}

void write_automation(int dir, const Automation& automation) {
    std::string content = automation.enabled ? "enabled=1\n" : "enabled=0\n";
    if (automation.default_profile) {
        content += "default_profile=" + *automation.default_profile + '\n';
    }
    for (const auto& [pattern, profile] : automation.rules) {
        content += "rule=" + pattern + '\t' + profile + '\n';
    }
    if (content.size() > max_automation_size) throw std::runtime_error("Automation configuration is too large");
    write_config_file(dir, automation_filename, content);
}

template<class Update>
bool update_automation(Update update, bool create = true) {
    auto dir = open_automation_directory(create);
    if (!dir) return false;
    lock_automation(dir.get());
    auto automation = read_automation(dir.get());
    if (!update(dir.get(), automation)) return false;
    write_automation(dir.get(), automation);
    return true;
}

bool wildcard_matches(std::string_view pattern, std::string_view app_id) {
    size_t p = 0;
    size_t i = 0;
    size_t star = std::string_view::npos;
    size_t retry = 0;
    while (i < app_id.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = i;
        } else if (p < pattern.size() && pattern[p] == app_id[i]) {
            ++p;
            ++i;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            i = ++retry;
        } else {
            return false;
        }
    }
    return pattern.find_first_not_of('*', p) == std::string_view::npos;
}

bool unlink_profile(int dir, const std::string& filename) {
    if (::unlinkat(dir, filename.c_str(), 0) < 0) {
        if (errno == ENOENT) return false;
        profile_error("Cannot delete profile " + filename);
    }
    if (::fsync(dir) < 0) profile_error("Cannot sync profile directory");
    return true;
}

} // namespace

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
    Profile profile{std::string(name), {}};
    bool has_parameter = false;
    while (!content.empty()) {
        const auto end = content.find('\n');
        auto line = trim(content.substr(0, end));
        content.remove_prefix(end == std::string_view::npos ? content.size() : end + 1);
        if (line.empty() || line.starts_with('#')) continue;

        const auto eq = line.find('=');
        const auto key = trim(line.substr(0, eq));
        int DeviceState::*field = nullptr;
        if (key == "saturation") field = &DeviceState::saturation;
        else if (key == "contrast") field = &DeviceState::contrast;
        else if (key == "brightness") field = &DeviceState::brightness;
        else if (key == "enabled") field = &DeviceState::enabled;
        else continue;

        auto number = eq == std::string_view::npos ? std::string_view{} : trim(line.substr(eq + 1));
        if (number.empty()) throw DeviceError("Missing profile value for " + std::string(key));
        int value;
        const auto [endptr, err] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (err != std::errc() || endptr != number.data() + number.size()) {
            throw DeviceError("Invalid profile value for " + std::string(key));
        }
        validate_parameter(key, value);
        profile.state.*field = value;
        has_parameter = true;
    }
    if (!has_parameter) throw DeviceError("Profile has no color parameters");
    return profile;
}

void save_profile(const std::string& name, const DeviceState& state, const std::filesystem::path& dir) {
    const auto filename = profile_filename(name);
    const auto content = serialize_profile({name, state});
    validate_parameter("saturation", state.saturation);
    validate_parameter("contrast", state.contrast);
    validate_parameter("brightness", state.brightness);
    validate_parameter("enabled", state.enabled);
    auto directory = open_config_directory(dir, true);
    write_config_file(directory.get(), filename, content);
}

DeviceState load_profile(const std::string& name, const std::filesystem::path& dir) {
    const auto filename = profile_filename(name);
    auto directory = open_config_directory(dir);
    if (!directory) profile_error("Profile '" + name + "' not found", ENOENT);
    return read_profile(directory.get(), name, filename);
}

std::vector<Profile> list_profiles(const std::filesystem::path& dir) {
    std::vector<Profile> list;
    auto directory = open_config_directory(dir);
    if (!directory) return list;

    const int fd = ::fcntl(directory.get(), F_DUPFD_CLOEXEC, 0);
    if (fd < 0) profile_error("Cannot duplicate profile directory descriptor");
    const auto close_dir = [](DIR* stream) { ::closedir(stream); };
    std::unique_ptr<DIR, decltype(close_dir)> stream(::fdopendir(fd), close_dir);
    if (!stream) {
        const int error = errno;
        ::close(fd);
        profile_error("Cannot list profile directory", error);
    }

    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (!entry) {
            if (errno) profile_error("Cannot list profile directory");
            break;
        }
        const std::string_view filename(entry->d_name);
        if (!filename.ends_with(profile_extension)) continue;
        const auto name = filename.substr(0, filename.size() - profile_extension.size());
        try {
            const auto validated = profile_filename(name);
            list.push_back({std::string(name), read_profile(directory.get(), name, validated)});
        } catch (const std::invalid_argument&) {
            continue;
        } catch (const std::runtime_error&) {
            continue;
        }
    }

    std::ranges::sort(list, {}, &Profile::name);
    return list;
}

std::string format_profiles_json(const std::vector<Profile>& profiles) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '[';
    bool first = true;
    for (const auto& profile : profiles) {
        if (!is_utf8(profile.name)) continue;
        if (!first) out << ", ";
        first = false;
        out << "{\"name\": " << json_string(profile.name)
            << ", \"saturation\": " << profile.state.saturation
            << ", \"contrast\": " << profile.state.contrast
            << ", \"brightness\": " << profile.state.brightness
            << ", \"enabled\": " << profile.state.enabled << '}';
    }
    out << ']';
    return out.str();
}

bool delete_profile(const std::string& name, const std::filesystem::path& dir) {
    const auto filename = profile_filename(name);
    auto directory = open_config_directory(dir);
    return directory && existing_config_file(directory.get(), filename) && unlink_profile(directory.get(), filename);
}

bool delete_profile(const std::string& name) {
    const auto filename = profile_filename(name);
    auto dir = open_automation_directory();
    if (!dir) return false;
    lock_automation(dir.get());
    auto profiles = open_saved_profiles(dir.get());
    if (!profiles || !existing_config_file(profiles.get(), filename)) return false;

    auto automation = read_automation(dir.get());
    const auto removed = std::erase_if(automation.rules, [&](const auto& rule) { return rule.second == name; });
    const bool was_default = automation.default_profile == name;
    if (was_default) automation.default_profile.reset();
    // Publish the cleanup first to avoid leaving references to a deleted profile.
    if (removed || was_default) write_automation(dir.get(), automation);
    return unlink_profile(profiles.get(), filename);
}

Automation load_automation() {
    auto dir = open_automation_directory();
    return dir ? read_automation(dir.get()) : Automation{};
}

std::string format_automation_json(const Automation& automation) {
    std::ostringstream out;
    out << "{\"enabled\": " << (automation.enabled ? "true" : "false")
        << ", \"default_profile\": " << (automation.default_profile ? json_string(*automation.default_profile) : "null")
        << ", \"rules\": [";
    bool first = true;
    for (const auto& [pattern, profile] : automation.rules) {
        if (!first) out << ", ";
        first = false;
        out << "{\"pattern\": " << json_string(pattern) << ", \"profile\": " << json_string(profile) << '}';
    }
    out << "]}";
    return out.str();
}

void set_rule(std::string_view pattern, const std::string& profile) {
    const auto normalized = normalize_pattern(pattern);
    update_automation([&](int dir, Automation& automation) {
        check_automation_profile(dir, profile);
        automation.rules.insert_or_assign(normalized, profile);
        return true;
    });
}

bool delete_rule(std::string_view pattern) {
    const auto normalized = normalize_pattern(pattern);
    return update_automation([&](int, Automation& automation) {
        return automation.rules.erase(normalized) > 0;
    }, false);
}

void set_default_profile(std::optional<std::string> profile) {
    update_automation([&](int dir, Automation& automation) {
        if (profile) check_automation_profile(dir, *profile);
        automation.default_profile = std::move(profile);
        return true;
    });
}

void set_automation_enabled(bool enabled) {
    update_automation([enabled](int, Automation& automation) {
        automation.enabled = enabled;
        return true;
    });
}

std::optional<std::string> match_rule(const Automation& automation, std::string_view app_id) {
    if (app_id.empty()) return std::nullopt;
    const auto id = lowercase_ascii(app_id);
    const auto exact = automation.rules.find(id);
    if (exact != automation.rules.end() && exact->first.find('*') == std::string::npos) {
        return exact->second;
    }

    const std::string* matched_profile = nullptr;
    size_t specificity = 0;
    for (const auto& [pattern, profile] : automation.rules) {
        const auto wildcards = static_cast<size_t>(std::ranges::count(pattern, '*'));
        const auto literals = pattern.size() - wildcards;
        if (wildcards && (!matched_profile || literals > specificity) && wildcard_matches(pattern, id)) {
            matched_profile = &profile;
            specificity = literals;
        }
    }
    if (matched_profile) return *matched_profile;
    return std::nullopt;
}

} // namespace prisma

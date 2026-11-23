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
#include <sys/stat.h>
#include <system_error>

namespace prisma {
namespace {

constexpr size_t max_profile_size = 16 * 1024;
constexpr std::string_view profile_extension = ".conf";

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

FileDescriptor open_profile_directory(const std::filesystem::path& dir, bool create = false) {
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
                profile_error("Cannot create profile directory " + dir.string());
            }
            child = FileDescriptor(::openat(parent.get(), it->c_str(), mode));
        }
        if (!child) {
            if (!create && errno == ENOENT) return FileDescriptor();
            profile_error("Cannot open profile directory " + dir.string());
        }
        parent = std::move(child);
    }

    struct stat st{};
    if (::fstat(parent.get(), &st) < 0) profile_error("Cannot inspect profile directory");
    if (st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        profile_error("Profile directory must be owned by the current user and writable only by its owner", EACCES);
    }
    return parent;
}

void check_profile_file(const struct stat& st) {
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink > 1 ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        throw std::runtime_error("Profile must be a regular user-owned file without aliases or shared write access");
    }
}

bool existing_profile(int dir, const std::string& filename) {
    struct stat st{};
    if (::fstatat(dir, filename.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) return false;
        profile_error("Cannot inspect profile " + filename);
    }
    check_profile_file(st);
    return true;
}

std::string_view trim(std::string_view input) {
    const auto first = input.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return input.substr(first, input.find_last_not_of(" \t\r") - first + 1);
}

DeviceState read_profile(int dir, std::string_view name, const std::string& filename) {
    FileDescriptor file(::openat(dir, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) profile_error("Cannot open profile " + filename);

    struct stat st{};
    if (::fstat(file.get(), &st) < 0) profile_error("Cannot inspect profile " + filename);
    check_profile_file(st);

    std::string content;
    char buf[4096];
    for (;;) {
        const ssize_t size = ::read(file.get(), buf, sizeof(buf));
        if (size == 0) break;
        if (size < 0) {
            if (errno == EINTR) continue;
            profile_error("Cannot read profile " + filename);
        }
        if (content.size() + size > max_profile_size) {
            throw std::runtime_error("Profile is too large");
        }
        content.append(buf, size);
    }
    return deserialize_profile(name, content).state;
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
    auto directory = open_profile_directory(dir, true);
    existing_profile(directory.get(), filename);

    std::string temp;
    FileDescriptor file;
    do {
        uint64_t id;
        if (::getentropy(&id, sizeof(id)) < 0) profile_error("Cannot generate temporary profile filename");
        temp = ".profile-" + std::to_string(id);
        file = FileDescriptor(::openat(directory.get(), temp.c_str(),
                                       O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    } while (!file && errno == EEXIST);
    if (!file) profile_error("Cannot create temporary profile");

    try {
        std::string_view remaining = content;
        while (!remaining.empty()) {
            const ssize_t size = ::write(file.get(), remaining.data(), remaining.size());
            if (size <= 0) {
                if (size < 0 && errno == EINTR) continue;
                profile_error("Cannot write profile", size == 0 ? EIO : errno);
            }
            remaining.remove_prefix(size);
        }
        if (::fsync(file.get()) < 0) profile_error("Cannot sync profile");
        if (::renameat(directory.get(), temp.c_str(), directory.get(), filename.c_str()) < 0) {
            profile_error("Cannot publish profile " + filename);
        }
    } catch (...) {
        ::unlinkat(directory.get(), temp.c_str(), 0);
        throw;
    }
    if (::fsync(directory.get()) < 0) profile_error("Cannot sync profile directory");
}

DeviceState load_profile(const std::string& name, const std::filesystem::path& dir) {
    const auto filename = profile_filename(name);
    auto directory = open_profile_directory(dir);
    if (!directory) profile_error("Profile '" + name + "' not found", ENOENT);
    return read_profile(directory.get(), name, filename);
}

std::vector<Profile> list_profiles(const std::filesystem::path& dir) {
    std::vector<Profile> list;
    auto directory = open_profile_directory(dir);
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
    auto directory = open_profile_directory(dir);
    if (!directory || !existing_profile(directory.get(), filename)) return false;
    if (::unlinkat(directory.get(), filename.c_str(), 0) < 0) {
        if (errno == ENOENT) return false;
        profile_error("Cannot delete profile " + filename);
    }
    if (::fsync(directory.get()) < 0) profile_error("Cannot sync profile directory");
    return true;
}

} // namespace prisma

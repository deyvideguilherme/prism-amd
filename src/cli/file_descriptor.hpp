#pragma once

#include <unistd.h>
#include <utility>

namespace prisma {

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) ::close(fd_);
    }

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

} // namespace prisma

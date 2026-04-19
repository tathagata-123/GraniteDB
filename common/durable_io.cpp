#include "durable_io.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace simpledb {
namespace {

[[noreturn]] void ThrowIoError(const std::string &msg, const std::string &path) {
    throw std::runtime_error(msg + ": " + path + " (" + std::strerror(errno) + ")");
}

int OpenForSync(const std::string &path, bool writable) {
#ifdef _WIN32
    int flags = writable ? (_O_RDWR | _O_BINARY) : (_O_RDONLY | _O_BINARY);
    int fd = _open(path.c_str(), flags);
#else
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd = ::open(path.c_str(), flags);
#endif
    return fd;
}

void CloseFd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
}

void SyncFd(int fd, const std::string &path) {
#ifdef _WIN32
    if (_commit(fd) != 0) {
        ThrowIoError("Failed to commit durable file state", path);
    }
#else
    if (::fsync(fd) != 0) {
        ThrowIoError("Failed to fsync durable file state", path);
    }
#endif
}

void WriteAll(int fd, const char *data, std::size_t len, const std::string &path) {
    std::size_t written = 0;
    while (written < len) {
#ifdef _WIN32
        int rc = _write(fd, data + written, static_cast<unsigned int>(len - written));
#else
        ssize_t rc = ::write(fd, data + written, len - written);
#endif
        if (rc < 0) {
            ThrowIoError("Failed to write file contents", path);
        }
        if (rc == 0) {
            throw std::runtime_error("Short write while writing durable file: " + path);
        }
        written += static_cast<std::size_t>(rc);
    }
}

std::string ParentPathOrDot(const std::string &path) {
    std::filesystem::path p(path);
    std::filesystem::path parent = p.parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

}  // namespace

void DurableSyncPath(const std::string &path) {
    int fd = OpenForSync(path, true);
    if (fd < 0) {
        ThrowIoError("Failed to open path for durable sync", path);
    }
    try {
        SyncFd(fd, path);
    } catch (...) {
        CloseFd(fd);
        throw;
    }
    CloseFd(fd);
}

void DurableSyncParentDirectory(const std::string &path) {
#ifdef _WIN32
    (void)path;
#else
    std::string dir = ParentPathOrDot(path);
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        ThrowIoError("Failed to open directory for durable sync", dir);
    }
    try {
        if (::fsync(fd) != 0) {
            ThrowIoError("Failed to fsync directory", dir);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
#endif
}

void AtomicWriteStringFile(const std::string &path, const std::string &contents) {
    std::filesystem::path target(path);
    std::filesystem::path dir = target.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }

    std::string tmp = path + ".tmp";
#ifdef _WIN32
    int fd = _open(tmp.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
#endif
    if (fd < 0) {
        ThrowIoError("Failed to create temporary file for atomic write", tmp);
    }

    try {
        if (!contents.empty()) {
            WriteAll(fd, contents.data(), contents.size(), tmp);
        }
        SyncFd(fd, tmp);
    } catch (...) {
        CloseFd(fd);
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
    CloseFd(fd);

    std::filesystem::rename(tmp, path);
    DurableSyncParentDirectory(path);
}

}  // namespace simpledb

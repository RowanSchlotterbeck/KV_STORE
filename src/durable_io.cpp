#include "../include/durable_io.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

bool full_sync(int fd) {
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) == 0) { return true; }
    if (errno != ENOTSUP && errno != EINVAL) { return false; }
    // Filesystem doesn't support F_FULLFSYNC; fall back to fsync.
#endif
    return ::fsync(fd) == 0;
}

bool write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool sync_directory(const std::filesystem::path& dir) {
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) { return false; }
    const bool ok = full_sync(dfd);
    ::close(dfd);
    return ok;
}

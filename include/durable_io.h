#ifndef KV_STORE_DURABLE_IO_H
#define KV_STORE_DURABLE_IO_H

// POSIX helpers shared by everything that must reach stable storage before
// the process acts on it (the command log, the Raft term/vote file). All
// return false with errno set on failure.

#include <cstddef>
#include <filesystem>

// Flushes file data to stable storage. On macOS plain fsync() only reaches
// the drive's cache; F_FULLFSYNC asks the drive to flush it too.
bool full_sync(int fd);

// write() until every byte is written, retrying on EINTR.
bool write_all(int fd, const char* data, std::size_t size);

// Makes a newly created or renamed directory entry durable.
bool sync_directory(const std::filesystem::path& dir);

#endif //KV_STORE_DURABLE_IO_H

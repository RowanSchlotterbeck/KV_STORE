//
// Created by Rowan Schlotterbeck on 2/10/26.
//

#include "../include/wal.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../include/serialization.h"
#include "../include/state_machine.h"

namespace
{
    std::string errno_message(const std::string& what, const std::string& path) {
        return what + " " + path + ": " + std::strerror(errno);
    }

    // Flushes file data to stable storage. On macOS plain fsync() only reaches
    // the drive's cache; F_FULLFSYNC asks the drive to flush it too.
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

    // Makes a newly created file's directory entry durable.
    bool sync_directory(const std::filesystem::path& dir) {
        const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dfd < 0) { return false; }
        const bool ok = full_sync(dfd);
        ::close(dfd);
        return ok;
    }
}

PersistentLog::~PersistentLog() {
    close();
}

void PersistentLog::open(const std::string& path) {
    if (is_open()) { throw wal_error("log already open: " + path_); }

    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) { throw wal_error("create directory " + p.parent_path().string() + ": " + ec.message()); }
    }

    const bool existed = std::filesystem::exists(p, ec);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) { throw wal_error(errno_message("open", path)); }
    if (!existed && !sync_directory(p.parent_path())) {
        const std::string msg = errno_message("sync directory of", path);
        ::close(fd);
        throw wal_error(msg);
    }

    fd_ = fd;
    path_ = path;
    last_index_ = 0;
    end_offset_ = 0;
    recovered_tail_bytes_ = 0;
    replayed_ = false;
    failed_ = false;
}

void PersistentLog::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::uint64_t PersistentLog::replay(StateMachine& sm) {
    if (!is_open()) { throw wal_error("replay: log not open"); }
    if (replayed_) { throw wal_error("replay: already replayed " + path_); }

    struct stat st {};
    if (::fstat(fd_, &st) != 0) { throw wal_error(errno_message("stat", path_)); }

    std::string buf(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t read_total = 0;
    while (read_total < buf.size()) {
        const ssize_t n = ::pread(fd_, buf.data() + read_total, buf.size() - read_total,
                                  static_cast<off_t>(read_total));
        if (n < 0) {
            if (errno == EINTR) { continue; }
            throw wal_error(errno_message("read", path_));
        }
        if (n == 0) { break; }
        read_total += static_cast<std::size_t>(n);
    }
    buf.resize(read_total);

    std::size_t offset = 0;
    std::uint64_t last = 0;
    for (;;) {
        log_entry entry;
        const decode_status status = deserialize_entry(buf, offset, entry);

        if (status == decode_status::ok) {
            if (entry.index != last + 1) {
                throw wal_error("corrupt log " + path_ + ": expected index " + std::to_string(last + 1)
                                + " but found " + std::to_string(entry.index) + " (entry ending at byte "
                                + std::to_string(offset) + "); refusing to start");
            }
            sm.apply(entry.cmd);
            last = entry.index;
            continue;
        }

        if (status == decode_status::end_of_input) { break; }

        if (status == decode_status::truncated) {
            // Crash mid-append: drop the partial frame so appends resume cleanly.
            if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0 || !full_sync(fd_)) {
                throw wal_error(errno_message("truncate torn tail of", path_));
            }
            recovered_tail_bytes_ = buf.size() - offset;
            break;
        }

        throw wal_error("corrupt log " + path_ + ": undecodable entry at byte offset "
                        + std::to_string(offset) + " of " + std::to_string(buf.size())
                        + "; refusing to start");
    }

    last_index_ = last;
    end_offset_ = offset;
    replayed_ = true;
    return last_index_;
}

std::uint64_t PersistentLog::append(const command& cmd) {
    log_entry entry;
    entry.index = last_index_ + 1;
    entry.term = 0;  // placeholder until Raft assigns terms
    entry.cmd = cmd;
    append_entry(entry);
    return entry.index;
}

void PersistentLog::append_entry(const log_entry& entry) {
    if (!is_open()) { throw wal_error("append: log not open"); }
    if (!replayed_) { throw wal_error("append: replay() must run before append()"); }
    if (failed_) { throw wal_error("append: log disabled after an earlier I/O error; restart to recover"); }

    std::string frame;
    if (!serialize_entry(entry, frame)) { throw wal_error("append: entry too large to encode"); }

    if (!write_all(fd_, frame.data(), frame.size()) || !full_sync(fd_)) {
        // The file may now hold a partial or unsynced frame. Try to cut it back,
        // but stop accepting appends either way: after a failed fsync the page
        // cache can't be trusted, and replay() will repair any torn tail.
        const std::string msg = errno_message("append to", path_);
        if (::ftruncate(fd_, static_cast<off_t>(end_offset_)) == 0) { full_sync(fd_); }
        failed_ = true;
        throw wal_error(msg);
    }

    end_offset_ += frame.size();
    last_index_ = entry.index;
}

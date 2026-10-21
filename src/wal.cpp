//
// Created by Rowan Schlotterbeck on 2/10/26.
//

#include "../include/wal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include "../include/crc32c.h"
#include "../include/durable_io.h"
#include "../include/serialization.h"
#include "../include/state_machine.h"

namespace
{
    std::string errno_message(const std::string& what, const std::string& path) {
        return what + " " + path + ": " + std::strerror(errno);
    }

    std::uint32_t load_u32_le(const char* p) {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        return static_cast<std::uint32_t>(b[0])
             | static_cast<std::uint32_t>(b[1]) << 8
             | static_cast<std::uint32_t>(b[2]) << 16
             | static_cast<std::uint32_t>(b[3]) << 24;
    }

    void store_u32_le(std::string& out, std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((v >> shift) & 0xFFu));
        }
    }

    std::string hex32(std::uint32_t v) {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "0x%08X", v);
        return buf;
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
    last_term_ = 0;
    end_offset_ = 0;
    terms_.clear();
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

    // Frame classification, in order (see the format comment in wal.h):
    //   no bytes left                          -> clean end
    //   partial header, or partial payload     -> torn tail: truncate, stop
    //   all-zero header and all-zero remainder -> zeroed tail: truncate, stop
    //   anything else malformed, CRC mismatch  -> corrupt: refuse to start
    const std::size_t size = buf.size();
    std::size_t offset = 0;
    std::uint64_t last = 0;
    std::uint64_t last_term = 0;
    std::vector<std::uint64_t> terms;
    const auto corrupt = [&](const std::string& why) {
        return wal_error("corrupt log " + path_ + ": " + why + " in entry at byte offset "
                         + std::to_string(offset) + " of " + std::to_string(size)
                         + "; refusing to start");
    };

    bool torn_tail = false;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        if (remaining < kHeaderSize) { torn_tail = true; break; }

        const std::uint32_t len = load_u32_le(buf.data() + offset);
        const std::uint32_t stored_crc = load_u32_le(buf.data() + offset + 4);

        if (len == 0 && stored_crc == 0) {
            // Some filesystems expose a crash-interrupted append as zero bytes
            // rather than a short file. A real frame never has length 0, so an
            // all-zero header can only be unwritten space. Accept it as the end
            // of the log only if everything after it is zero too; data after
            // the zeros means committed entries would be discarded.
            const bool rest_zero = std::all_of(buf.begin() + static_cast<std::ptrdiff_t>(offset),
                                               buf.end(), [](char c) { return c == 0; });
            if (!rest_zero) { throw corrupt("zero-filled header followed by non-zero data"); }
            torn_tail = true;
            break;
        }
        if (len == 0) { throw corrupt("zero length"); }
        if (len > kMaxEntryPayloadSize) { throw corrupt("length " + std::to_string(len) + " exceeds limit"); }
        if (remaining - kHeaderSize < len) { torn_tail = true; break; }

        const std::string_view payload(buf.data() + offset + kHeaderSize, len);
        const std::uint32_t actual_crc = crc32c(payload);
        if (actual_crc != stored_crc) {
            throw corrupt("checksum mismatch (stored " + hex32(stored_crc) + ", computed "
                          + hex32(actual_crc) + ")");
        }

        log_entry entry;
        if (!decode_entry_payload(payload, entry)) { throw corrupt("undecodable payload"); }
        if (entry.index != last + 1) {
            throw corrupt("expected index " + std::to_string(last + 1) + " but found "
                          + std::to_string(entry.index));
        }

        sm.apply(entry.cmd);
        last = entry.index;
        last_term = entry.term;
        terms.push_back(entry.term);
        offset += kHeaderSize + len;
    }

    if (torn_tail) {
        // Crash mid-append: drop the partial/zeroed frame so appends resume cleanly.
        if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0 || !full_sync(fd_)) {
            throw wal_error(errno_message("truncate torn tail of", path_));
        }
        recovered_tail_bytes_ = size - offset;
    }

    last_index_ = last;
    last_term_ = last_term;
    end_offset_ = offset;
    terms_ = std::move(terms);
    replayed_ = true;
    return last_index_;
}

std::string PersistentLog::encode_frame(const log_entry& entry) {
    std::string payload;
    if (!encode_entry_payload(entry, payload)) { throw wal_error("entry too large to encode"); }

    std::string frame;
    frame.reserve(kHeaderSize + payload.size());
    store_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
    store_u32_le(frame, crc32c(payload));
    frame += payload;
    return frame;
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

    const std::string frame = encode_frame(entry);

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
    last_term_ = entry.term;
    terms_.push_back(entry.term);
}

std::optional<std::uint64_t> PersistentLog::term_at(std::uint64_t index) const {
    if (index == 0) { return 0; }
    if (index > terms_.size()) { return std::nullopt; }
    return terms_[index - 1];
}

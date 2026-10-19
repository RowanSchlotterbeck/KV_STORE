//
// Created by Rowan Schlotterbeck on 2/10/26.
//

#ifndef KV_STORE_WAL_H
#define KV_STORE_WAL_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "command.h"
#include "log_entry.h"

class StateMachine;

// Thrown when the log can't be opened, replayed, or durably appended to.
class wal_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Append-only command log; every append is fsynced before it returns.
//
// On-disk format: a concatenation of frames, no file header.
//
//   offset 0  u32 little-endian  length  payload size in bytes (never 0)
//   offset 4  u32 little-endian  crc     CRC32C of the payload bytes only
//   offset 8  length bytes       payload serialized kvstore.LogEntry
//
// The CRC covers the payload, not the length field. A corrupted length is
// still caught: it makes replay checksum the wrong byte range, which fails
// the check. The exception is a length that points past end-of-file, which
// is indistinguishable from a torn final write and is treated as one.
//
// Lifecycle: open() -> replay() -> append()... append() refuses to run until
// replay() has established last_index(), so indices are never reused.
class PersistentLog
{
public:
    static constexpr const char* kDefaultPath = "data/kvstore.log";
    static constexpr std::size_t kHeaderSize = 8;

    // Encodes one frame in the format above. Throws wal_error if the entry
    // is too large. Exposed for tests and tooling.
    static std::string encode_frame(const log_entry& entry);

    PersistentLog() = default;
    ~PersistentLog();
    PersistentLog(const PersistentLog&) = delete;
    PersistentLog& operator=(const PersistentLog&) = delete;

    // Opens or creates the log file (and its parent directory).
    void open(const std::string& path);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Applies every complete entry to `sm` in order and returns the last index.
    // A torn final entry or a zero-filled tail (crash mid-append) is cut off
    // so the next append starts on a clean boundary. A checksum mismatch or
    // other mid-file corruption throws wal_error and leaves the file as is.
    std::uint64_t replay(StateMachine& sm);

    // Durably appends `cmd` as the next entry (term 0 until Raft assigns terms)
    // and returns its index. Throws wal_error on I/O failure; after a failure
    // the log refuses further appends until it is reopened.
    std::uint64_t append(const command& cmd);

    std::uint64_t last_index() const { return last_index_; }
    // Bytes of torn or zeroed tail discarded by the last replay(); 0 if clean.
    std::uint64_t recovered_tail_bytes() const { return recovered_tail_bytes_; }

private:
    void append_entry(const log_entry& entry);

    int fd_ = -1;
    std::string path_;
    std::uint64_t last_index_ = 0;
    std::uint64_t end_offset_ = 0;  // file offset just past the last good entry
    std::uint64_t recovered_tail_bytes_ = 0;
    bool replayed_ = false;
    bool failed_ = false;
};


#endif //KV_STORE_WAL_H

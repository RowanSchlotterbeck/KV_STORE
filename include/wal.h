//
// Created by Rowan Schlotterbeck on 2/10/26.
//

#ifndef KV_STORE_WAL_H
#define KV_STORE_WAL_H

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

// Append-only command log. The file is a concatenation of serialize_entry()
// frames (see serialization.h); every append is fsynced before it returns.
//
// Lifecycle: open() -> replay() -> append()... append() refuses to run until
// replay() has established last_index(), so indices are never reused.
class PersistentLog
{
public:
    static constexpr const char* kDefaultPath = "data/kvstore.log";

    PersistentLog() = default;
    ~PersistentLog();
    PersistentLog(const PersistentLog&) = delete;
    PersistentLog& operator=(const PersistentLog&) = delete;

    // Opens or creates the log file (and its parent directory).
    void open(const std::string& path);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Applies every complete entry to `sm` in order and returns the last index.
    // A torn final entry (crash mid-append) is cut off so the next append
    // starts on a clean boundary. Mid-file corruption throws wal_error.
    std::uint64_t replay(StateMachine& sm);

    // Durably appends `cmd` as the next entry (term 0 until Raft assigns terms)
    // and returns its index. Throws wal_error on I/O failure; after a failure
    // the log refuses further appends until it is reopened.
    std::uint64_t append(const command& cmd);

    std::uint64_t last_index() const { return last_index_; }
    // Bytes of torn tail discarded by the last replay(); 0 for a clean log.
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

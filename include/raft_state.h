#ifndef KV_STORE_RAFT_STATE_H
#define KV_STORE_RAFT_STATE_H

// Durable Raft hard state: term and vote (Raft paper, Figure 2 "persistent
// state on all servers", minus the log, which PersistentLog owns), plus the
// commit index, so a restart applies only what is known committed.
//
// One small file per node, rewritten whole on every change. Version 2:
//
//   offset 0   4 bytes          magic "KVRS"
//   offset 4   u32 little-endian format version (2)
//   offset 8   u64 little-endian node id that owns the file
//   offset 16  u64 little-endian current_term
//   offset 24  u64 little-endian voted_for (0 = none)
//   offset 32  u64 little-endian commit_index
//   offset 40  u32 little-endian CRC32C of bytes [0, 40)
//
// Version 1 (36 bytes: no commit_index, CRC at offset 32) still loads, as
// commit_index 0; the next save writes version 2.
//
// The commit index on disk may lag the real one (it is saved after commit
// advances, and a crash can fall in between) but never leads it: it is only
// ever saved with a value already known committed. Lagging is safe: restart
// applies less and learns the rest from the leader.
//
// save() writes a temp file, fsyncs it, renames it over the old one and
// fsyncs the directory, so after a crash the file holds either the previous
// or the new state, never a mix. Anything else on load is corruption.

#include <cstdint>
#include <stdexcept>
#include <string>
#include "raft_messages.h"

// Thrown when the state file can't be read, is corrupt, or can't be saved.
class raft_state_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct raft_hard_state
{
    std::uint64_t term = 0;
    node_id voted_for = 0;          // 0 = none
    std::uint64_t commit_index = 0;  // highest index known committed (may lag)
};

class RaftStateFile
{
public:
    static constexpr std::size_t kSize = 44;    // version 2
    static constexpr std::size_t kSizeV1 = 36;

    // The state file lives next to the node's command log, so every node
    // (each needs its own log anyway) gets its own: data/n1.log -> data/n1.log.raftstate.
    static std::string path_for_log(const std::string& log_path) { return log_path + ".raftstate"; }

    // Encodes one file image in the format above. Exposed for tests.
    static std::string encode(node_id owner, const raft_hard_state& state);

    // Does not touch the disk.
    RaftStateFile(std::string path, node_id self);

    // Missing file -> all zero (first run). A file of the wrong size, magic,
    // version or checksum, or one written by a different node id, throws
    // raft_state_error rather than guessing.
    raft_hard_state load() const;

    // Durably replaces the file with `state` before returning. Throws
    // raft_state_error on failure; after a failure every later save() throws
    // too (as with the log, a failed fsync leaves the page cache untrusted).
    void save(const raft_hard_state& state);

    const std::string& path() const { return path_; }

private:
    std::string path_;
    node_id self_;
    bool failed_ = false;
};

#endif //KV_STORE_RAFT_STATE_H

#include "../include/raft_state.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include "../include/crc32c.h"
#include "../include/durable_io.h"

namespace
{
    constexpr char kMagic[4] = {'K', 'V', 'R', 'S'};
    constexpr std::uint32_t kVersion = 1;
    constexpr std::size_t kCrcOffset = 32;

    std::string errno_message(const std::string& what, const std::string& path) {
        return what + " " + path + ": " + std::strerror(errno);
    }

    void store_le(std::string& out, std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu)); }
    }

    std::uint64_t load_le(const char* p, int bytes) {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) { v |= static_cast<std::uint64_t>(b[i]) << (8 * i); }
        return v;
    }
}

std::string RaftStateFile::encode(node_id owner, const raft_hard_state& state) {
    std::string out(kMagic, sizeof(kMagic));
    store_le(out, kVersion, 4);
    store_le(out, owner, 8);
    store_le(out, state.term, 8);
    store_le(out, state.voted_for, 8);
    store_le(out, crc32c(out), 4);
    return out;
}

RaftStateFile::RaftStateFile(std::string path, node_id self) : path_(std::move(path)), self_(self) {}

raft_hard_state RaftStateFile::load() const {
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) { return {}; }
        throw raft_state_error(errno_message("open", path_));
    }
    // Read one byte past kSize so an oversized file is caught too.
    char buf[kSize + 1];
    std::size_t got = 0;
    while (got < sizeof(buf)) {
        const ssize_t n = ::read(fd, buf + got, sizeof(buf) - got);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            const std::string msg = errno_message("read", path_);
            ::close(fd);
            throw raft_state_error(msg);
        }
        if (n == 0) { break; }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);

    const auto corrupt = [&](const std::string& why) {
        return raft_state_error("corrupt raft state file " + path_ + ": " + why + "; refusing to start");
    };
    if (got > kSize) { throw corrupt("size is over " + std::to_string(kSize) + " bytes"); }
    if (got < kSize) {
        throw corrupt("size is " + std::to_string(got) + " bytes, expected " + std::to_string(kSize));
    }
    if (std::memcmp(buf, kMagic, sizeof(kMagic)) != 0) { throw corrupt("bad magic"); }
    const std::uint32_t stored_crc = static_cast<std::uint32_t>(load_le(buf + kCrcOffset, 4));
    if (crc32c(buf, kCrcOffset) != stored_crc) { throw corrupt("checksum mismatch"); }
    const auto version = load_le(buf + 4, 4);
    if (version != kVersion) { throw corrupt("unsupported version " + std::to_string(version)); }
    const node_id owner = load_le(buf + 8, 8);
    if (owner != self_) {
        throw corrupt("written by node " + std::to_string(owner) + ", but this is node "
                      + std::to_string(self_));
    }
    return {load_le(buf + 16, 8), load_le(buf + 24, 8)};
}

void RaftStateFile::save(const raft_hard_state& state) {
    if (failed_) { throw raft_state_error("save " + path_ + ": disabled after an earlier I/O error; restart to recover"); }

    const std::string image = encode(self_, state);
    const std::string tmp = path_ + ".tmp";
    const auto fail = [&](const std::string& what, const std::string& path) {
        const std::string msg = errno_message(what, path);
        failed_ = true;
        ::unlink(tmp.c_str());
        return raft_state_error(msg);
    };

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { throw fail("open", tmp); }
    if (!write_all(fd, image.data(), image.size()) || !full_sync(fd)) {
        const auto err = fail("write", tmp);
        ::close(fd);
        throw err;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path_.c_str()) != 0) { throw fail("rename " + tmp + " over", path_); }
    if (!sync_directory(std::filesystem::path(path_).parent_path())) { throw fail("sync directory of", path_); }
}

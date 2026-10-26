#include "../include/serialization.h"
#include "../include/proto_convert.h"

kvstore::Command to_proto(const command& cmd) {
    kvstore::Command pb;
    pb.set_operation(cmd.operation);
    pb.set_key(cmd.key);
    if (cmd.value.has_value()) {
        pb.set_value(*cmd.value);
    }
    return pb;
}

command from_proto(const kvstore::Command& pb) {
    command cmd;
    cmd.operation = pb.operation();
    cmd.key = pb.key();
    if (pb.has_value()) {
        cmd.value = pb.value();
    }
    return cmd;
}

kvstore::LogEntry to_proto(const log_entry& entry) {
    kvstore::LogEntry pb;
    pb.set_index(entry.index);
    pb.set_term(entry.term);
    *pb.mutable_command() = to_proto(entry.cmd);
    return pb;
}

log_entry from_proto(const kvstore::LogEntry& pb) {
    log_entry entry;
    entry.index = pb.index();
    entry.term = pb.term();
    entry.cmd = from_proto(pb.command());
    return entry;
}

bool encode_entry_payload(const log_entry& entry, std::string& payload) {
    if (!to_proto(entry).SerializeToString(&payload)) { return false; }
    return payload.size() <= kMaxEntryPayloadSize;
}

bool decode_entry_payload(std::string_view payload, log_entry& out) {
    if (payload.size() > kMaxEntryPayloadSize) { return false; }
    kvstore::LogEntry pb;
    if (!pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) { return false; }
    out = from_proto(pb);
    return true;
}

bool serialize_entry(const log_entry& entry, std::string& out) {
    std::string payload;
    if (!to_proto(entry).SerializeToString(&payload)) { return false; }
    if (payload.size() > kMaxEntryPayloadSize) { return false; }

    auto len = static_cast<std::uint32_t>(payload.size());
    char header[kFrameHeaderSize] = {
        static_cast<char>(len & 0xFF),
        static_cast<char>((len >> 8) & 0xFF),
        static_cast<char>((len >> 16) & 0xFF),
        static_cast<char>((len >> 24) & 0xFF),
    };
    out.append(header, kFrameHeaderSize);
    out.append(payload);
    return true;
}

decode_status deserialize_entry(std::string_view buf, std::size_t& offset, log_entry& out) {
    if (offset > buf.size()) { return decode_status::corrupt; }
    const std::size_t remaining = buf.size() - offset;
    if (remaining == 0) { return decode_status::end_of_input; }
    if (remaining < kFrameHeaderSize) { return decode_status::truncated; }

    const auto* p = reinterpret_cast<const unsigned char*>(buf.data() + offset);
    const std::uint32_t len = static_cast<std::uint32_t>(p[0])
                            | static_cast<std::uint32_t>(p[1]) << 8
                            | static_cast<std::uint32_t>(p[2]) << 16
                            | static_cast<std::uint32_t>(p[3]) << 24;

    if (len > kMaxEntryPayloadSize) { return decode_status::corrupt; }
    if (remaining - kFrameHeaderSize < len) { return decode_status::truncated; }

    kvstore::LogEntry pb;
    if (!pb.ParseFromArray(buf.data() + offset + kFrameHeaderSize, static_cast<int>(len))) {
        return decode_status::corrupt;
    }

    out = from_proto(pb);
    offset += kFrameHeaderSize + len;
    return decode_status::ok;
}

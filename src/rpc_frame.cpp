#include "../include/rpc_frame.h"
#include "../include/raft_convert.h"

rpc_type type_of(const rpc_message& msg) {
    return static_cast<rpc_type>(msg.index() + 1);
}

bool is_request(rpc_type type) {
    return type == rpc_type::request_vote_request || type == rpc_type::append_entries_request;
}

static bool known_type(std::uint8_t tag) {
    return tag >= static_cast<std::uint8_t>(rpc_type::request_vote_request)
        && tag <= static_cast<std::uint8_t>(rpc_type::append_entries_response);
}

bool encode_rpc_frame(const rpc_message& msg, std::string& out) {
    std::string payload;
    const bool serialized = std::visit(
        [&payload](const auto& m) { return to_proto(m).SerializeToString(&payload); }, msg);
    if (!serialized || payload.size() > kMaxRpcPayloadSize) { return false; }

    const auto len = static_cast<std::uint32_t>(payload.size());
    const char header[kRpcHeaderSize] = {
        static_cast<char>(len & 0xFF),
        static_cast<char>((len >> 8) & 0xFF),
        static_cast<char>((len >> 16) & 0xFF),
        static_cast<char>((len >> 24) & 0xFF),
        static_cast<char>(type_of(msg)),
    };
    out.append(header, kRpcHeaderSize);
    out.append(payload);
    return true;
}

rpc_decode_status decode_rpc_header(std::string_view header, rpc_frame_header& out) {
    if (header.size() < kRpcHeaderSize) { return rpc_decode_status::truncated; }
    const auto* p = reinterpret_cast<const unsigned char*>(header.data());
    const std::uint32_t len = static_cast<std::uint32_t>(p[0])
                            | static_cast<std::uint32_t>(p[1]) << 8
                            | static_cast<std::uint32_t>(p[2]) << 16
                            | static_cast<std::uint32_t>(p[3]) << 24;
    if (len > kMaxRpcPayloadSize || !known_type(p[4])) { return rpc_decode_status::malformed; }
    out.type = static_cast<rpc_type>(p[4]);
    out.payload_size = len;
    return rpc_decode_status::ok;
}

template <typename Proto>
static bool parse_into(std::string_view payload, rpc_message& out) {
    Proto pb;
    if (!pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) { return false; }
    out = from_proto(pb);
    return true;
}

bool decode_rpc_payload(rpc_type type, std::string_view payload, rpc_message& out) {
    if (payload.size() > kMaxRpcPayloadSize) { return false; }
    switch (type) {
        case rpc_type::request_vote_request:
            return parse_into<kvstore::RequestVoteRequest>(payload, out);
        case rpc_type::request_vote_response:
            return parse_into<kvstore::RequestVoteResponse>(payload, out);
        case rpc_type::append_entries_request:
            return parse_into<kvstore::AppendEntriesRequest>(payload, out);
        case rpc_type::append_entries_response:
            return parse_into<kvstore::AppendEntriesResponse>(payload, out);
    }
    return false;
}

rpc_decode_status decode_rpc_frame(std::string_view buf, std::size_t& offset, rpc_message& out) {
    if (offset > buf.size()) { return rpc_decode_status::malformed; }
    const std::string_view rest = buf.substr(offset);
    if (rest.empty()) { return rpc_decode_status::end_of_input; }

    rpc_frame_header header;
    const rpc_decode_status status = decode_rpc_header(rest, header);
    if (status != rpc_decode_status::ok) { return status; }
    if (rest.size() - kRpcHeaderSize < header.payload_size) { return rpc_decode_status::truncated; }

    rpc_message msg;
    if (!decode_rpc_payload(header.type, rest.substr(kRpcHeaderSize, header.payload_size), msg)) {
        return rpc_decode_status::malformed;
    }
    out = std::move(msg);
    offset += kRpcHeaderSize + header.payload_size;
    return rpc_decode_status::ok;
}

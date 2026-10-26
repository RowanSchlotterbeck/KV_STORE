#ifndef KV_STORE_RPC_FRAME_H
#define KV_STORE_RPC_FRAME_H

// Wire framing for node-to-node Raft RPCs. One frame carries one message:
//
//   [ u32 little-endian payload length ][ u8 type tag ][ payload ]
//
// The payload is the serialized Protobuf message named by the tag (see
// proto/raft.proto). Payload length 0 is legal: a message whose fields are
// all default serializes to nothing. Unrelated to the log's frame format.
//
// This header exposes only domain types; Protobuf stays in rpc_frame.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include "raft_messages.h"

constexpr std::size_t kRpcHeaderSize = 5;
// Upper bound on one payload. A larger length is malformed and is rejected
// before anything is allocated.
constexpr std::uint32_t kMaxRpcPayloadSize = 64u * 1024u * 1024u;

enum class rpc_type : std::uint8_t
{
    request_vote_request    = 1,
    request_vote_response   = 2,
    append_entries_request  = 3,
    append_entries_response = 4,
};

using rpc_message = std::variant<request_vote_request, request_vote_response,
                                 append_entries_request, append_entries_response>;

rpc_type type_of(const rpc_message& msg);
bool is_request(rpc_type type);

enum class rpc_decode_status
{
    ok,           // frame decoded, offset advanced past it
    end_of_input, // offset is exactly at the end of the buffer
    truncated,    // a partial frame (header or payload) remains; offset unchanged
    malformed,    // unknown type tag, oversized length, or unparseable payload
};

struct rpc_frame_header
{
    rpc_type type{};
    std::uint32_t payload_size = 0;
};

// Appends one frame for `msg` to `out`. Returns false (leaving `out`
// untouched) only if the payload would exceed kMaxRpcPayloadSize.
bool encode_rpc_frame(const rpc_message& msg, std::string& out);

// Decodes the frame starting at `offset` in `buf`. On ok, fills `out` and
// advances `offset`; otherwise both are untouched. Never reads outside `buf`.
rpc_decode_status decode_rpc_frame(std::string_view buf, std::size_t& offset, rpc_message& out);

// Streaming halves of decode_rpc_frame, for socket readers that fetch the
// fixed header first and then exactly payload_size more bytes.
// decode_rpc_header: truncated if fewer than kRpcHeaderSize bytes, malformed
// for an unknown tag or oversized length. decode_rpc_payload: false if the
// payload doesn't parse as the message `type` names.
rpc_decode_status decode_rpc_header(std::string_view header, rpc_frame_header& out);
bool decode_rpc_payload(rpc_type type, std::string_view payload, rpc_message& out);

#endif //KV_STORE_RPC_FRAME_H

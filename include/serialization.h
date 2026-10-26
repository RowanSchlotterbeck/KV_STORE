#ifndef KV_STORE_SERIALIZATION_H
#define KV_STORE_SERIALIZATION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "log_entry.h"

// Length-delimited framing for log entries:
//
//   [ u32 little-endian payload length ][ payload: serialized kvstore.LogEntry ]
//
// Frames can be concatenated in a single buffer/file and read back one at a
// time. This header exposes only domain types; Protobuf stays in the .cpp.

constexpr std::size_t kFrameHeaderSize = 4;
// Upper bound on a single payload; anything larger is treated as corruption
// rather than an allocation request.
constexpr std::uint32_t kMaxEntryPayloadSize = 64u * 1024u * 1024u;

enum class decode_status
{
    ok,           // entry decoded, offset advanced past the frame
    end_of_input, // offset is exactly at the end of the buffer; nothing to read
    truncated,    // a partial frame (header or payload) remains; offset unchanged
    corrupt,      // length exceeds kMaxEntryPayloadSize or payload fails to parse
};

// Appends one framed entry to `out`. Returns false only if the entry can't be
// encoded (e.g. payload would exceed kMaxEntryPayloadSize).
bool serialize_entry(const log_entry& entry, std::string& out);

// Decodes the frame starting at `offset` in `buf`. On ok, fills `out` and
// advances `offset`; on any other status, `offset` and `out` are untouched.
// Never reads outside `buf`.
decode_status deserialize_entry(std::string_view buf, std::size_t& offset, log_entry& out);

// Unframed payload codec: just the serialized kvstore.LogEntry bytes, for
// callers that define their own framing (e.g. the persistent log, which adds
// a checksum). encode returns false if the payload would exceed
// kMaxEntryPayloadSize; decode returns false if `payload` doesn't parse.
bool encode_entry_payload(const log_entry& entry, std::string& payload);
bool decode_entry_payload(std::string_view payload, log_entry& out);

#endif //KV_STORE_SERIALIZATION_H

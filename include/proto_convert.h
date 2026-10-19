#ifndef KV_STORE_PROTO_CONVERT_H
#define KV_STORE_PROTO_CONVERT_H

// Domain <-> Protobuf conversion. Only boundary code (serialization, and
// later the RPC layer) should include this; core logic uses command.h /
// log_entry.h and never sees generated types.

#include "command.h"
#include "log_entry.h"
#include "kvstore.pb.h"

kvstore::Command to_proto(const command& cmd);
command from_proto(const kvstore::Command& pb);

kvstore::LogEntry to_proto(const log_entry& entry);
log_entry from_proto(const kvstore::LogEntry& pb);

#endif //KV_STORE_PROTO_CONVERT_H

#ifndef KV_STORE_RAFT_CONVERT_H
#define KV_STORE_RAFT_CONVERT_H

// Raft RPC domain <-> Protobuf conversion. Boundary-only, like
// proto_convert.h: the future RPC transport includes this; core logic uses
// raft_messages.h and never sees generated types.

#include "raft_messages.h"
#include "raft.pb.h"

kvstore::RequestVoteRequest to_proto(const request_vote_request& msg);
request_vote_request from_proto(const kvstore::RequestVoteRequest& pb);

kvstore::RequestVoteResponse to_proto(const request_vote_response& msg);
request_vote_response from_proto(const kvstore::RequestVoteResponse& pb);

kvstore::AppendEntriesRequest to_proto(const append_entries_request& msg);
append_entries_request from_proto(const kvstore::AppendEntriesRequest& pb);

kvstore::AppendEntriesResponse to_proto(const append_entries_response& msg);
append_entries_response from_proto(const kvstore::AppendEntriesResponse& pb);

#endif //KV_STORE_RAFT_CONVERT_H

#include "../include/raft_convert.h"
#include "../include/proto_convert.h"

kvstore::RequestVoteRequest to_proto(const request_vote_request& msg) {
    kvstore::RequestVoteRequest pb;
    pb.set_term(msg.term);
    pb.set_candidate_id(msg.candidate_id);
    pb.set_last_log_index(msg.last_log_index);
    pb.set_last_log_term(msg.last_log_term);
    return pb;
}

request_vote_request from_proto(const kvstore::RequestVoteRequest& pb) {
    request_vote_request msg;
    msg.term = pb.term();
    msg.candidate_id = pb.candidate_id();
    msg.last_log_index = pb.last_log_index();
    msg.last_log_term = pb.last_log_term();
    return msg;
}

kvstore::RequestVoteResponse to_proto(const request_vote_response& msg) {
    kvstore::RequestVoteResponse pb;
    pb.set_term(msg.term);
    pb.set_vote_granted(msg.vote_granted);
    return pb;
}

request_vote_response from_proto(const kvstore::RequestVoteResponse& pb) {
    request_vote_response msg;
    msg.term = pb.term();
    msg.vote_granted = pb.vote_granted();
    return msg;
}

kvstore::AppendEntriesRequest to_proto(const append_entries_request& msg) {
    kvstore::AppendEntriesRequest pb;
    pb.set_term(msg.term);
    pb.set_leader_id(msg.leader_id);
    pb.set_prev_log_index(msg.prev_log_index);
    pb.set_prev_log_term(msg.prev_log_term);
    pb.mutable_entries()->Reserve(static_cast<int>(msg.entries.size()));
    for (const auto& entry : msg.entries) {
        *pb.add_entries() = to_proto(entry);
    }
    pb.set_leader_commit(msg.leader_commit);
    return pb;
}

append_entries_request from_proto(const kvstore::AppendEntriesRequest& pb) {
    append_entries_request msg;
    msg.term = pb.term();
    msg.leader_id = pb.leader_id();
    msg.prev_log_index = pb.prev_log_index();
    msg.prev_log_term = pb.prev_log_term();
    msg.entries.reserve(static_cast<std::size_t>(pb.entries_size()));
    for (const auto& entry : pb.entries()) {
        msg.entries.push_back(from_proto(entry));
    }
    msg.leader_commit = pb.leader_commit();
    return msg;
}

kvstore::AppendEntriesResponse to_proto(const append_entries_response& msg) {
    kvstore::AppendEntriesResponse pb;
    pb.set_term(msg.term);
    pb.set_success(msg.success);
    return pb;
}

append_entries_response from_proto(const kvstore::AppendEntriesResponse& pb) {
    append_entries_response msg;
    msg.term = pb.term();
    msg.success = pb.success();
    return msg;
}

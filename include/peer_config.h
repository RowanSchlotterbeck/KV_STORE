#ifndef KV_STORE_PEER_CONFIG_H
#define KV_STORE_PEER_CONFIG_H

// Static cluster membership, loaded once at startup.
//
// File format: one node per line, three or four whitespace-separated fields,
//
//   <node_id> <host> <peer_port> [<client_port>]
//
// '#' starts a comment; blank lines are ignored. Every node in the cluster is
// listed, including this one; the process is told its own id separately
// (--node-id). peer_port is the node-to-node RPC port and is always given
// explicitly per node, never derived. client_port, if given, is that node's
// client line-protocol port: a follower uses it to redirect clients to the
// leader, and a node started without --client-port listens on its own.
//
// Node id 0 is rejected: Raft code uses 0 to mean "no node" (e.g. no vote).
// This header has no networking; host names are resolved by the transport.

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "raft_messages.h"

class config_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct peer_address
{
    std::string host;
    std::uint16_t port = 0;                    // peer RPC port
    std::optional<std::uint16_t> client_port;  // client protocol port, if configured
};

class ClusterConfig
{
public:
    // Validates the table: self present, ids nonzero, ports nonzero, no two
    // nodes sharing host:port. Throws config_error.
    ClusterConfig(node_id self, std::map<node_id, peer_address> nodes);

    // Parses the file format above. Throws config_error naming the file and
    // line on any problem; never returns a partial table.
    static ClusterConfig load(const std::string& path, node_id self);
    static ClusterConfig parse(std::string_view text, node_id self, const std::string& source);

    node_id self_id() const { return self_; }
    const peer_address& self_address() const { return nodes_.at(self_); }
    const std::map<node_id, peer_address>& nodes() const { return nodes_; }
    std::vector<node_id> peers() const;  // every node except self, ascending
    std::optional<peer_address> address_of(node_id id) const;

private:
    node_id self_;
    std::map<node_id, peer_address> nodes_;
};

// Strict decimal parse of a node id (no sign, no junk, nonzero). Throws config_error.
node_id parse_node_id(std::string_view text);

#endif //KV_STORE_PEER_CONFIG_H

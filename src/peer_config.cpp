#include "../include/peer_config.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace {

template <typename T>
bool parse_unsigned(std::string_view text, T& out) {
    if (text.empty()) { return false; }
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) { ++i; }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) { ++i; }
        if (i > start) { fields.push_back(line.substr(start, i - start)); }
    }
    return fields;
}

}  // namespace

node_id parse_node_id(std::string_view text) {
    node_id id = 0;
    if (!parse_unsigned(text, id)) {
        throw config_error("invalid node id '" + std::string(text) + "' (expected a decimal number)");
    }
    if (id == 0) { throw config_error("node id 0 is reserved"); }
    return id;
}

ClusterConfig::ClusterConfig(node_id self, std::map<node_id, peer_address> nodes)
    : self_(self), nodes_(std::move(nodes)) {
    std::set<std::pair<std::string, std::uint16_t>> seen;
    for (const auto& [id, addr] : nodes_) {
        const std::string who = "node " + std::to_string(id);
        if (id == 0) { throw config_error("node id 0 is reserved"); }
        if (addr.host.empty()) { throw config_error(who + " has an empty host"); }
        if (addr.port == 0) { throw config_error(who + " has peer port 0"); }
        if (!seen.emplace(addr.host, addr.port).second) {
            throw config_error(who + " reuses address " + addr.host + ":" + std::to_string(addr.port));
        }
        if (addr.client_port) {
            if (*addr.client_port == 0) { throw config_error(who + " has client port 0"); }
            if (!seen.emplace(addr.host, *addr.client_port).second) {
                throw config_error(who + "'s client port reuses address " + addr.host + ":"
                                   + std::to_string(*addr.client_port));
            }
        }
    }
    if (nodes_.find(self_) == nodes_.end()) {
        throw config_error("this node's id " + std::to_string(self_) + " is not listed in the cluster");
    }
}

ClusterConfig ClusterConfig::parse(std::string_view text, node_id self, const std::string& source) {
    std::map<node_id, peer_address> nodes;
    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const auto fields = split_fields(line);
        if (fields.empty()) { continue; }

        const std::string where = source + ":" + std::to_string(line_no) + ": ";
        if (fields.size() != 3 && fields.size() != 4) {
            throw config_error(where + "expected '<node_id> <host> <peer_port> [<client_port>]', got "
                               + std::to_string(fields.size()) + " field(s)");
        }
        node_id id = 0;
        try {
            id = parse_node_id(fields[0]);
        } catch (const config_error& e) {
            throw config_error(where + e.what());
        }
        std::uint16_t port = 0;
        if (!parse_unsigned(fields[2], port) || port == 0) {
            throw config_error(where + "invalid peer port '" + std::string(fields[2])
                               + "' (expected 1-65535)");
        }
        std::optional<std::uint16_t> client_port;
        if (fields.size() == 4) {
            std::uint16_t cp = 0;
            if (!parse_unsigned(fields[3], cp) || cp == 0) {
                throw config_error(where + "invalid client port '" + std::string(fields[3])
                                   + "' (expected 1-65535)");
            }
            client_port = cp;
        }
        if (!nodes.emplace(id, peer_address{std::string(fields[1]), port, client_port}).second) {
            throw config_error(where + "duplicate node id " + std::to_string(id));
        }
    }

    try {
        return ClusterConfig(self, std::move(nodes));
    } catch (const config_error& e) {
        throw config_error(source + ": " + e.what());
    }
}

ClusterConfig ClusterConfig::load(const std::string& path, node_id self) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { throw config_error("cannot open cluster config " + path); }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) { throw config_error("error reading cluster config " + path); }
    return parse(text.str(), self, path);
}

std::vector<node_id> ClusterConfig::peers() const {
    std::vector<node_id> out;
    for (const auto& [id, addr] : nodes_) {
        if (id != self_) { out.push_back(id); }
    }
    return out;
}

std::optional<peer_address> ClusterConfig::address_of(node_id id) const {
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) { return std::nullopt; }
    return it->second;
}

//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#ifndef KV_STORE_KV_STORE_H
#define KV_STORE_KV_STORE_H

#include <string>
#include <unordered_map>
#include <vector>

std::string execute(std::unordered_map<std::string, std::string>& map, const std::vector<std::string>& args);

#endif //KV_STORE_KV_STORE_H
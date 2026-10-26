#ifndef KV_STORE_CRC32C_H
#define KV_STORE_CRC32C_H

#include <cstddef>
#include <cstdint>
#include <string_view>

// CRC32C (Castagnoli, reflected polynomial 0x82F63B78), as used by iSCSI,
// ext4 and LevelDB. Check value: crc32c("123456789") == 0xE3069283.
//
// Uses the CPU's CRC32C instructions when the compiler targets them
// (ARMv8 CRC extension or SSE4.2), otherwise a table-driven fallback.

std::uint32_t crc32c(const void* data, std::size_t size);

inline std::uint32_t crc32c(std::string_view bytes) {
    return crc32c(bytes.data(), bytes.size());
}

// Portable table-driven implementation, always available. Exposed so tests
// can check it against the accelerated path.
std::uint32_t crc32c_software(const void* data, std::size_t size);

// True if crc32c() uses hardware instructions in this build.
bool crc32c_is_hardware();

#endif //KV_STORE_CRC32C_H

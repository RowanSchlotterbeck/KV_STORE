#include "../include/crc32c.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define KV_CRC32C_ARM 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define KV_CRC32C_SSE42 1
#endif

namespace
{
    constexpr std::uint32_t kPolyReflected = 0x82F63B78u;

    constexpr std::array<std::uint32_t, 256> make_table() {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ ((crc & 1u) ? kPolyReflected : 0u);
            }
            table[i] = crc;
        }
        return table;
    }

    constexpr auto kTable = make_table();
}

std::uint32_t crc32c_software(const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

#if defined(KV_CRC32C_ARM) || defined(KV_CRC32C_SSE42)

std::uint32_t crc32c(const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    while (size >= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, sizeof(word));  // unaligned-safe load
#if defined(KV_CRC32C_ARM)
        crc = __crc32cd(crc, word);
#else
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
#endif
        p += 8;
        size -= 8;
    }
    while (size > 0) {
#if defined(KV_CRC32C_ARM)
        crc = __crc32cb(crc, *p);
#else
        crc = _mm_crc32_u8(crc, *p);
#endif
        ++p;
        --size;
    }
    return crc ^ 0xFFFFFFFFu;
}

bool crc32c_is_hardware() { return true; }

#else

std::uint32_t crc32c(const void* data, std::size_t size) {
    return crc32c_software(data, size);
}

bool crc32c_is_hardware() { return false; }

#endif

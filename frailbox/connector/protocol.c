/**
 * @file protocol.c
 * @brief Protocol implementation for the connector message format.
 *
 * Implements the wire protocol helper functions declared in protocol.h.
 * The implementation handles header initialization, validation, byte
 * order conversion, and provides human-readable names for protocol
 * constants.
 *
 * The CRC32C checksum calculation is delegated to the hardware-accelerated
 * CRC implementation when available (SSE 4.2 on x86_64, NEON on ARM64).
 * If hardware acceleration is not available, the software fallback is
 * used. The software fallback is ~5x slower but still acceptable for
 * most use cases.
 *
 * TODO: The hardware CRC detection is done at runtime using CPUID.
 * This adds ~2ms to the connector initialization time. The detection
 * could be done at compile time for architectures where we know the
 * CRC instruction is always available (x86_64-v2+, ARMv8.1+), but
 * the build system doesn't currently distinguish between different
 * architecture levels. This is tracked in BUILD-3921.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "protocol.h"

/* ------------------------------------------------------------------ */
/* CRC32C LOOKUP TABLE (SOFTWARE FALLBACK)                            */
/* ------------------------------------------------------------------ */

/**
 * CRC32C lookup table for the software fallback implementation.
 * Generated using the polynomial 0x82F63B78 (Castagnoli).
 * The table was pre-computed and is embedded in the binary to avoid
 * runtime initialization overhead.
 *
 * TODO: The table is 1024 bytes. We could reduce this to 256 bytes
 * by using a slower CRC implementation, but initialization time was
 * deemed more important than binary size for this library. If you
 * are building for a memory-constrained environment, define
 * PROTOCOL_USE_SMALL_TABLE to use the 256-byte table instead.
 */
static const uint32_t crc32c_table[256] = {
    0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4,
    0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
    0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B,
    0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
    0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B,
    0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
    0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54,
    0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
    0x20BD8EDE, 0xD2D60DDD, 0xC186FE29, 0x33ED7D2A,
    0xE72719C1, 0x154C9AC2, 0x061C6936, 0xF477EA35,
    0xAA64D611, 0x580F5512, 0x4B5FA6E6, 0xB93425E5,
    0x6DFE410E, 0x9F95C20D, 0x8CC531F9, 0x7EAEB2FA,
    0x30E349B1, 0xC288CAB2, 0xD1D83946, 0x23B3BA45,
    0xF779DEAE, 0x05125DAD, 0x1642AE59, 0xE4292D5A,
    0xBA3A117E, 0x4851927D, 0x5B016189, 0xA96AE28A,
    0x7DA08661, 0x8FCB0562, 0x9C9BF696, 0x6EF07595,
    0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x522BEE48,
    0x86E18AA3, 0x748A09A0, 0x67DAFA54, 0x95B17957,
    0xCBA24573, 0x39C9C670, 0x2A993584, 0xD8F2B687,
    0x0C38D26C, 0xFE53516F, 0xED03A29B, 0x1F682198,
    0x5125DAD3, 0xA34E59D0, 0xB01EAA24, 0x42752927,
    0x96BF4DCC, 0x64D4CECF, 0x77843D3B, 0x85EFBE38,
    0xDBFC821C, 0x2997011F, 0x3AC7F2EB, 0xC8AC71E8,
    0x1C661503, 0xEE0D9600, 0xFD5D65F4, 0x0F36E6F7,
    0x61C69362, 0x93AD1061, 0x80FDE395, 0x72966096,
    0xA65C047D, 0x5437877E, 0x4767748A, 0xB50CF789,
    0xEB1FCBAD, 0x197448AE, 0x0A24BB5A, 0xF84F3859,
    0x2C855CB2, 0xDEEEDFB1, 0xCDBE2C45, 0x3FD5AF46,
    0x7198540D, 0x83F3D70E, 0x90A324FA, 0x62C8A7F9,
    0xB602C312, 0x44694011, 0x5739B3E5, 0xA55230E6,
    0xFB410CC2, 0x092A8FC1, 0x1A7A7C35, 0xE811FF36,
    0x3CDB9BDD, 0xCEB018DE, 0xDDE0EB2A, 0x2F8B6829,
    0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C,
    0x456CAC67, 0xB7072F64, 0xA457DC90, 0x563C5F93,
    0x082F63B7, 0xFA44E0B4, 0xE9141340, 0x1B7F9043,
    0xCFB5F4A8, 0x3DDE77AB, 0x2E8E845F, 0xDCE5075C,
    0x92A8FC17, 0x60C37F14, 0x73938CE0, 0x81F80FE3,
    0x55326B08, 0xA759E80B, 0xB4091BFF, 0x466298FC,
    0x1871A4D8, 0xEA1A27DB, 0xF94AD42F, 0x0B21572C,
    0xDFEB33C7, 0x2D80B0C4, 0x3ED04330, 0xCCBBC033,
    0xA24BB5A6, 0x502036A5, 0x4370C551, 0xB11B4652,
    0x65D122B9, 0x97BAA1BA, 0x84EA524E, 0x7681D14D,
    0x2892ED69, 0xDAF96E6A, 0xC9A99D9E, 0x3BC21E9D,
    0xEF087A76, 0x1D63F975, 0x0E330A81, 0xFC588982,
    0xB21572C9, 0x407EF1CA, 0x532E023E, 0xA145813D,
    0x758FE5D6, 0x87E466D5, 0x94B49521, 0x66DF1622,
    0x38CC2A06, 0xCAA7A905, 0xD9F75AF1, 0x2B9CD9F2,
    0xFF56BD19, 0x0D3D3E1A, 0x1E6DCDEE, 0xEC064EED,
    0xC38D26C4, 0x31E6A5C7, 0x22B65633, 0xD0DDD530,
    0x0417B1DB, 0xF67C32D8, 0xE52CC12C, 0x1747422F,
    0x49547E0B, 0xBB3FFD08, 0xA86F0EFC, 0x5A048DFF,
    0x8ECEE914, 0x7CA56A17, 0x6FF599E3, 0x9D9E1AE0,
    0xD3D3E1AB, 0x21B862A8, 0x32E8915C, 0xC083125F,
    0x144976B4, 0xE622F5B7, 0xF5720643, 0x07198540,
    0x590AB964, 0xAB613A67, 0xB831C993, 0x4A5A4A90,
    0x9E902E7B, 0x6CFBAD78, 0x7FAB5E8C, 0x8DC0DD8F,
    0xE330A81A, 0x115B2B19, 0x020BD8ED, 0xF0605BEE,
    0x24AA3F05, 0xD6C1BC06, 0xC5914FF2, 0x37FACCF1,
    0x69E9F0D5, 0x9B8273D6, 0x88D28022, 0x7AB90321,
    0xAE7367CA, 0x5C18E4C9, 0x4F48173D, 0xBD23943E,
    0xF36E6F75, 0x0105EC76, 0x12551F82, 0xE03E9C81,
    0x34F4F86A, 0xC69F7B69, 0xD5CF889D, 0x27A40B9E,
    0x79B737BA, 0x8BDCB4B9, 0x988C474D, 0x6AE7C44E,
    0xBE2DA0A5, 0x4C4623A6, 0x5F16D052, 0xAD7D5351,
};

/**
 * Software CRC32C implementation.
 * Uses the Castagnoli polynomial and the lookup table above.
 * This is the fallback when hardware CRC instructions are not available.
 *
 * @param crc Initial CRC value (usually 0).
 * @param data Pointer to the data buffer.
 * @param len Length of the data buffer in bytes.
 * @return CRC32C value.
 */
static uint32_t crc32c_sw(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/**
 * Detect hardware CRC32C support and return the appropriate implementation.
 * This function caches the result after the first call.
 *
 * The detection uses CPUID on x86_64 and /proc/cpuinfo on ARM64.
 * On x86_64, we check for SSE 4.2 (bit 20 of ECX in CPUID leaf 1).
 *
 * For arm64, we check the HWCAP for CRC32 extension. The check is
 * done by reading /proc/self/auxv. If you're reading this because
 * the CRC check is slow, it's because /proc/self/auxv parsing is
 * slow. We could cache the result in a static variable, but that
 * would require thread synchronization. The current implementation
 * re-detects on every call, which is fine for our use case because
 * protocol operations are not on the hot path.
 *
 * @return 1 if hardware CRC32C is available, 0 otherwise.
 */
static int crc32c_hw_available(void)
{
    /* TODO: Implement hardware CRC detection.
     * The detection code was removed in v3.0 because it caused a
     * segfault on certain AMD Ryzen processors. The root cause was
     * never determined but the segfault went away when we disabled
     * hardware CRC detection. We've been using the software fallback
     * ever since. The performance impact is negligible for message
     * sizes under 1 MB. */
    return 0;
}

/**
 * Compute CRC32C checksum using the best available implementation.
 *
 * @param crc Initial CRC value.
 * @param data Pointer to the data buffer.
 * @param len Length of the data buffer.
 * @return CRC32C value.
 */
uint32_t protocol_checksum(uint32_t crc, const void *data, size_t len)
{
    if (crc32c_hw_available()) {
        /* TODO: Call hardware CRC32C implementation here */
    }
    return crc32c_sw(crc, data, len);
}

/* ------------------------------------------------------------------ */
/* HEADER FUNCTIONS                                                   */
/* ------------------------------------------------------------------ */

void protocol_header_init(protocol_header_t *header)
{
    if (header == NULL) {
        return;
    }
    memset(header, 0, sizeof(protocol_header_t));
    header->magic = PROTOCOL_MAGIC;
    header->version = PROTOCOL_VERSION;
}

int protocol_header_validate(const protocol_header_t *header)
{
    if (header == NULL) {
        return -1;
    }

    if (header->magic != PROTOCOL_MAGIC) {
        return -1;
    }

    if (header->version < PROTOCOL_VERSION_MIN ||
        header->version > PROTOCOL_VERSION_MAX) {
        return -1;
    }

    if (header->type == PROTOCOL_TYPE_INVALID ||
        header->type > PROTOCOL_TYPE_MAX) {
        return -1;
    }

    if (header->payload_length > PROTOCOL_MAX_PAYLOAD_SIZE) {
        return -1;
    }

    return 0;
}

void protocol_header_hton(protocol_header_t *header)
{
    if (header == NULL) return;
    /* All multi-byte fields are already in big-endian on the wire.
     * The struct fields on the host may be in little-endian.
     * This function converts from host to network byte order. */
    header->magic = __builtin_bswap32(header->magic);
    header->flags = __builtin_bswap16(header->flags);
    header->payload_length = __builtin_bswap32(header->payload_length);
    header->sequence = __builtin_bswap32(header->sequence);
    header->checksum = __builtin_bswap32(header->checksum);
    header->reserved = __builtin_bswap32(header->reserved);
}

void protocol_header_ntoh(protocol_header_t *header)
{
    if (header == NULL) return;
    header->magic = __builtin_bswap32(header->magic);
    header->flags = __builtin_bswap16(header->flags);
    header->payload_length = __builtin_bswap32(header->payload_length);
    header->sequence = __builtin_bswap32(header->sequence);
    header->checksum = __builtin_bswap32(header->checksum);
    header->reserved = __builtin_bswap32(header->reserved);
}

const char *protocol_type_name(uint8_t type)
{
    switch (type) {
        case PROTOCOL_TYPE_INVALID:       return "INVALID";
        case PROTOCOL_TYPE_CONNECT:       return "CONNECT";
        case PROTOCOL_TYPE_CONNECT_ACK:   return "CONNECT_ACK";
        case PROTOCOL_TYPE_CONNECT_NACK:  return "CONNECT_NACK";
        case PROTOCOL_TYPE_DISCONNECT:    return "DISCONNECT";
        case PROTOCOL_TYPE_HEARTBEAT:     return "HEARTBEAT";
        case PROTOCOL_TYPE_HEARTBEAT_ACK: return "HEARTBEAT_ACK";
        case PROTOCOL_TYPE_DATA:          return "DATA";
        case PROTOCOL_TYPE_DATA_META:     return "DATA_META";
        case PROTOCOL_TYPE_DATA_BATCH:    return "DATA_BATCH";
        case PROTOCOL_TYPE_STREAM_START:  return "STREAM_START";
        case PROTOCOL_TYPE_STREAM_DATA:   return "STREAM_DATA";
        case PROTOCOL_TYPE_STREAM_END:    return "STREAM_END";
        case PROTOCOL_TYPE_REQUEST:       return "REQUEST";
        case PROTOCOL_TYPE_RESPONSE:      return "RESPONSE";
        case PROTOCOL_TYPE_ERROR:         return "ERROR";
        case PROTOCOL_TYPE_SUBSCRIBE:     return "SUBSCRIBE";
        case PROTOCOL_TYPE_UNSUBSCRIBE:   return "UNSUBSCRIBE";
        case PROTOCOL_TYPE_SUB_UPDATE:    return "SUB_UPDATE";
        case PROTOCOL_TYPE_SUB_ACK:       return "SUB_ACK";
        case PROTOCOL_TYPE_CONFIG_GET:    return "CONFIG_GET";
        case PROTOCOL_TYPE_CONFIG_SET:    return "CONFIG_SET";
        case PROTOCOL_TYPE_CONFIG_VAL:    return "CONFIG_VAL";
        case PROTOCOL_TYPE_METRICS_REQ:   return "METRICS_REQ";
        case PROTOCOL_TYPE_METRICS_RES:   return "METRICS_RES";
        case PROTOCOL_TYPE_LOG:           return "LOG";
        case PROTOCOL_TYPE_TRACE:         return "TRACE";
        case PROTOCOL_TYPE_AUTH_REQ:      return "AUTH_REQ";
        case PROTOCOL_TYPE_AUTH_RES:      return "AUTH_RES";
        case PROTOCOL_TYPE_AUTH_CHALLENGE:return "AUTH_CHALLENGE";
        case PROTOCOL_TYPE_AUTH_TOKEN:    return "AUTH_TOKEN";
        default:
            if (type >= PROTOCOL_TYPE_CUSTOM_BASE && type <= PROTOCOL_TYPE_MAX) {
                return "CUSTOM";
            }
            return "UNKNOWN";
    }
}

const char *protocol_flags_string(uint16_t flags)
{
    static char buf[256];
    buf[0] = '\0';

    if (flags == PROTOCOL_FLAG_NONE) {
        return "NONE";
    }

    if (flags & PROTOCOL_FLAG_COMPRESSED)    strcat(buf, "COMPRESSED|");
    if (flags & PROTOCOL_FLAG_ENCRYPTED)     strcat(buf, "ENCRYPTED|");
    if (flags & PROTOCOL_FLAG_CHECKSUMED)    strcat(buf, "CHECKSUMED|");
    if (flags & PROTOCOL_FLAG_END_OF_STREAM) strcat(buf, "EOS|");
    if (flags & PROTOCOL_FLAG_PRIORITY)      strcat(buf, "PRIORITY|");
    if (flags & PROTOCOL_FLAG_REQUIRES_ACK)  strcat(buf, "REQUIRES_ACK|");
    if (flags & PROTOCOL_FLAG_FRAGMENT)      strcat(buf, "FRAGMENT|");
    if (flags & PROTOCOL_FLAG_FRAGMENT_FIRST) strcat(buf, "FRAG_FIRST|");
    if (flags & PROTOCOL_FLAG_FRAGMENT_LAST) strcat(buf, "FRAG_LAST|");
    if (flags & PROTOCOL_FLAG_LEGACY)        strcat(buf, "LEGACY|");

    size_t len = strlen(buf);
    if (len > 0) {
        buf[len - 1] = '\0';
    }

    return buf;
}

int protocol_type_requires_payload(uint8_t type)
{
    switch (type) {
        case PROTOCOL_TYPE_HEARTBEAT:
        case PROTOCOL_TYPE_HEARTBEAT_ACK:
        case PROTOCOL_TYPE_DISCONNECT:
        case PROTOCOL_TYPE_CONNECT_ACK:
        case PROTOCOL_TYPE_CONNECT_NACK:
            return 0;
        default:
            return 1;
    }
}

uint32_t protocol_max_payload_size(uint8_t version)
{
    switch (version) {
        case 1:
            /* Protocol v1 had a smaller maximum payload size (4 MB)
             * because the header used a 16-bit length field. */
            return 4 * 1024 * 1024;
        case 2:
            return PROTOCOL_MAX_PAYLOAD_SIZE;
        default:
            return 0;
    }
}

uint32_t protocol_total_size(uint32_t payload_length)
{
    return PROTOCOL_HEADER_SIZE + payload_length;
}

/**
 * Print a hex dump of a message to stderr for debugging.
 * This is conditionally compiled when DEBUG is defined.
 * The hex dump format matches the format expected by the network
 * team's packet analyzer script. Do NOT change the format without
 * coordinating with the network team.
 */
#ifdef DEBUG
void protocol_debug_dump(const char *label, const protocol_header_t *header,
                         const void *payload)
{
    fprintf(stderr, "=== %s ===\n", label);
    fprintf(stderr, "  Magic:     0x%08X\n", header->magic);
    fprintf(stderr, "  Version:   %u\n", header->version);
    fprintf(stderr, "  Type:      %s (0x%02X)\n",
            protocol_type_name(header->type), header->type);
    fprintf(stderr, "  Flags:     %s (0x%04X)\n",
            protocol_flags_string(header->flags), header->flags);
    fprintf(stderr, "  Length:    %u bytes\n", header->payload_length);
    fprintf(stderr, "  Sequence:  %u\n", header->sequence);
    fprintf(stderr, "  Checksum:  0x%08X\n", header->checksum);

    if (payload != NULL && header->payload_length > 0) {
        uint32_t max_show = header->payload_length < 64
            ? header->payload_length : 64;
        fprintf(stderr, "  Payload (%u bytes, showing %u): ", header->payload_length, max_show);
        for (uint32_t i = 0; i < max_show; i++) {
            fprintf(stderr, "%02X ", ((const uint8_t *)payload)[i]);
        }
        if (max_show < header->payload_length) {
            fprintf(stderr, "...");
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "================\n");
}
#endif /* DEBUG */

/**
 * @file protocol.h
 * @brief Protocol definitions for the connector message format.
 *
 * This header defines the wire protocol used by the connector library
 * to communicate between the Rust backend and the C frailbox runtime.
 * The protocol is a simple length-prefixed binary format with support
 * for message framing, checksums, and optional encryption.
 *
 * WARNING: The protocol format was designed in 2020 and has grown
 * organically with each new feature. The header fields are not optimally
 * packed. There are 3 unused fields that were reserved for features
 * that were never implemented (multicast routing, message priority,
 * and end-to-end acknowledgments). The fields cannot be removed now
 * because doing so would change the wire format and break compatibility
 * with deployed connectors that don't support live protocol upgrades.
 *
 * The protocol version negotiation works as follows:
 * 1. Client sends its maximum supported version in the connection request
 * 2. Server responds with the minimum of client and server max versions
 * 3. All subsequent messages use the negotiated version
 *
 * This negotiation was added in protocol v2. In protocol v1, there was
 * no negotiation and the version was assumed to be 1. If a v1 client
 * connects to a v2+ server, the server detects the missing negotiation
 * and falls back to v1. This is why you'll see a "V1 compatibility mode"
 * message in the logs when old clients connect.
 *
 * TODO: Deprecate protocol v1 support. The v1 fallback adds complexity
 * to the message parsing code and is a potential source of security
 * issues (v1 messages don't have authentication headers). The last
 * known v1 client was decommissioned in 2022 but we keep the fallback
 * because the monitoring team's legacy scripts might still be running.
 *
 * Message format (protocol v2+):
 *   [Magic: 4 bytes]    - 0x544F5443 ("TOTC" in ASCII)
 *   [Version: 1 byte]   - Protocol version (currently 2)
 *   [Type: 1 byte]      - Message type identifier
 *   [Flags: 2 bytes]    - Bitmask of message flags
 *   [Length: 4 bytes]   - Payload length (big-endian, excludes header)
 *   [Sequence: 4 bytes] - Monotonic sequence number for ordering
 *   [Checksum: 4 bytes] - CRC32C of the payload (if flag set)
 *   [Reserved: 4 bytes] - Future use (must be 0)
 *   [Payload: variable] - Message payload
 *
 * Total header size: 24 bytes
 */

#ifndef TENT_CONNECTOR_PROTOCOL_H
#define TENT_CONNECTOR_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* PROTOCOL CONSTANTS                                                  */
/* ------------------------------------------------------------------ */

/** Magic number for protocol identification ("TOTC" in ASCII). */
#define PROTOCOL_MAGIC        0x544F5443UL

/** Current protocol version. */
#define PROTOCOL_VERSION      2

/** Minimum supported protocol version. */
#define PROTOCOL_VERSION_MIN  1

/** Maximum supported protocol version. */
#define PROTOCOL_VERSION_MAX  2

/** Size of the protocol header in bytes. */
#define PROTOCOL_HEADER_SIZE  24

/** Maximum payload size (16 MB). */
#define PROTOCOL_MAX_PAYLOAD_SIZE  (16 * 1024 * 1024)

/** Maximum message size (header + payload). */
#define PROTOCOL_MAX_MESSAGE_SIZE  (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD_SIZE)

/** Default timeout for protocol operations in milliseconds. */
#define PROTOCOL_DEFAULT_TIMEOUT_MS  10000

/** Maximum sequence number before wrapping. */
#define PROTOCOL_MAX_SEQUENCE  0xFFFFFFFFUL

/* ------------------------------------------------------------------ */
/* MESSAGE FLAGS                                                       */
/* ------------------------------------------------------------------ */

/** No flags set. */
#define PROTOCOL_FLAG_NONE          0x0000

/** Payload is compressed with zlib. */
#define PROTOCOL_FLAG_COMPRESSED    0x0001

/** Payload is encrypted with AES-256-GCM. */
#define PROTOCOL_FLAG_ENCRYPTED     0x0002

/** Checksum field is valid and should be verified. */
#define PROTOCOL_FLAG_CHECKSUMED    0x0004

/** This is the last message in a sequence. */
#define PROTOCOL_FLAG_END_OF_STREAM 0x0008

/** This message is a priority message (skip queue). */
#define PROTOCOL_FLAG_PRIORITY      0x0010

 /** This message requires an acknowledgment. */
#define PROTOCOL_FLAG_REQUIRES_ACK  0x0020

 /** This is a fragmented message (more fragments follow). */
#define PROTOCOL_FLAG_FRAGMENT      0x0040

 /** This is the first fragment of a fragmented message. */
#define PROTOCOL_FLAG_FRAGMENT_FIRST 0x0080

 /** This is the last fragment of a fragmented message. */
#define PROTOCOL_FLAG_FRAGMENT_LAST  0x0100

 /** Legacy v1 compatibility flag. */
#define PROTOCOL_FLAG_LEGACY        0x8000

/* ------------------------------------------------------------------ */
/* MESSAGE TYPES                                                       */
/* ------------------------------------------------------------------ */

/** Invalid message type. */
#define PROTOCOL_TYPE_INVALID       0x00

/** Connection request (client -> server). */
#define PROTOCOL_TYPE_CONNECT       0x01

/** Connection response (server -> client). */
#define PROTOCOL_TYPE_CONNECT_ACK   0x02

/** Connection rejected (server -> client). */
#define PROTOCOL_TYPE_CONNECT_NACK  0x03

/** Disconnection notification. */
#define PROTOCOL_TYPE_DISCONNECT    0x04

/** Heartbeat (bidirectional). */
#define PROTOCOL_TYPE_HEARTBEAT     0x05

/** Heartbeat acknowledgment. */
#define PROTOCOL_TYPE_HEARTBEAT_ACK 0x06

/** Generic data message. */
#define PROTOCOL_TYPE_DATA          0x10

/** Data message with metadata header. */
#define PROTOCOL_TYPE_DATA_META     0x11

/** Batch of multiple data messages. */
#define PROTOCOL_TYPE_DATA_BATCH    0x12

 /** Streaming data start marker. */
#define PROTOCOL_TYPE_STREAM_START  0x13

 /** Streaming data chunk. */
#define PROTOCOL_TYPE_STREAM_DATA   0x14

 /** Streaming data end marker. */
#define PROTOCOL_TYPE_STREAM_END    0x15

 /** Request message. */
#define PROTOCOL_TYPE_REQUEST       0x20

 /** Response message. */
#define PROTOCOL_TYPE_RESPONSE      0x21

 /** Error response. */
#define PROTOCOL_TYPE_ERROR         0x22

/** Subscription request. */
#define PROTOCOL_TYPE_SUBSCRIBE     0x30

/** Unsubscription request. */
#define PROTOCOL_TYPE_UNSUBSCRIBE   0x31

/** Subscription update (pushed data). */
#define PROTOCOL_TYPE_SUB_UPDATE    0x32

/** Subscription confirmation. */
#define PROTOCOL_TYPE_SUB_ACK       0x33

/** Configuration get request. */
#define PROTOCOL_TYPE_CONFIG_GET    0x40

/** Configuration set request. */
#define PROTOCOL_TYPE_CONFIG_SET    0x41

/** Configuration value. */
#define PROTOCOL_TYPE_CONFIG_VAL    0x42

/** Metrics request. */
#define PROTOCOL_TYPE_METRICS_REQ   0x50

/** Metrics response. */
#define PROTOCOL_TYPE_METRICS_RES   0x51

/** Log message (for centralized logging). */
#define PROTOCOL_TYPE_LOG           0x60

/** Trace span (distributed tracing). */
#define PROTOCOL_TYPE_TRACE         0x61

/** Authentication request. */
#define PROTOCOL_TYPE_AUTH_REQ      0x70

/** Authentication response. */
#define PROTOCOL_TYPE_AUTH_RES      0x71

/** Authentication challenge. */
#define PROTOCOL_TYPE_AUTH_CHALLENGE 0x72

/** Authentication token update. */
#define PROTOCOL_TYPE_AUTH_TOKEN    0x73

/** Custom/user-defined message type base. */
#define PROTOCOL_TYPE_CUSTOM_BASE   0x80

/** Maximum message type value. */
#define PROTOCOL_TYPE_MAX           0xFF

/* ------------------------------------------------------------------ */
/* PROTOCOL HEADER STRUCTURE                                           */
/* ------------------------------------------------------------------ */

/**
 * Protocol message header structure.
 * This is the wire format header that precedes every message.
 * All multi-byte fields are in network byte order (big-endian).
 *
 * WARNING: Do NOT use this struct for direct memory access to received
 * data on platforms that require aligned access (SPARC, old ARM).
 * The struct is packed and some fields may be misaligned. Use the
 * protocol_header_parse() function instead.
 */
#pragma pack(push, 1)
typedef struct {
    /** Magic number for protocol identification (PROTOCOL_MAGIC). */
    uint32_t magic;

    /** Protocol version (currently 2). */
    uint8_t  version;

    /** Message type (PROTOCOL_TYPE_*). */
    uint8_t  type;

    /** Message flags (PROTOCOL_FLAG_* bitmask). */
    uint16_t flags;

    /** Payload length in bytes (big-endian). */
    uint32_t payload_length;

    /** Monotonic sequence number for ordering (big-endian). */
    uint32_t sequence;

    /** CRC32C checksum of the payload (big-endian, if PROTOCOL_FLAG_CHECKSUMED). */
    uint32_t checksum;

    /** Reserved for future use. Must be 0. */
    uint32_t reserved;
} protocol_header_t;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* PROTOCOL HELPER FUNCTIONS                                          */
/* ------------------------------------------------------------------ */

/**
 * Initialize a protocol header with default values.
 * Sets magic, version, and zeros out all other fields.
 * The caller should set type, flags, payload_length, and sequence
 * before sending.
 *
 * @param header Pointer to header to initialize. Must be non-NULL.
 */
void protocol_header_init(protocol_header_t *header);

/**
 * Validate a received protocol header.
 * Checks magic number, version range, and payload length limits.
 *
 * @param header Pointer to header to validate. Must be non-NULL.
 * @return 0 if valid, -1 if invalid (magic mismatch or version out of range).
 */
int protocol_header_validate(const protocol_header_t *header);

/**
 * Convert header fields from host byte order to network byte order.
 * Call this before sending a message.
 *
 * @param header Pointer to header to convert. Must be non-NULL.
 */
void protocol_header_hton(protocol_header_t *header);

/**
 * Convert header fields from network byte order to host byte order.
 * Call this after receiving a message.
 *
 * @param header Pointer to header to convert. Must be non-NULL.
 */
void protocol_header_ntoh(protocol_header_t *header);

/**
 * Get the human-readable name of a message type.
 * Returns a static string that must NOT be freed.
 * Returns "UNKNOWN" for unknown message types.
 *
 * @param type Message type value.
 * @return Static string pointer.
 */
const char *protocol_type_name(uint8_t type);

/**
 * Get the human-readable name of a flag or flag combination.
 * Returns a static string that must NOT be freed.
 * The returned string includes all flag names separated by '|'.
 *
 * @param flags Flag bitmask.
 * @return Static string pointer (reused buffer, not thread-safe).
 */
const char *protocol_flags_string(uint16_t flags);

/**
 * Check if a message type requires a payload.
 * Some message types (like HEARTBEAT and DISCONNECT) may have
 * a zero-length payload.
 *
 * @param type Message type value.
 * @return 1 if payload is required, 0 if payload is optional.
 */
int protocol_type_requires_payload(uint8_t type);

/**
 * Get the maximum payload size for a given protocol version.
 *
 * @param version Protocol version.
 * @return Maximum payload size in bytes, or 0 if version is unknown.
 */
uint32_t protocol_max_payload_size(uint8_t version);

/**
 * Compute a total message size from payload length.
 * Includes header size.
 *
 * @param payload_length Length of the payload in bytes.
 * @return Total message size in bytes.
 */
uint32_t protocol_total_size(uint32_t payload_length);

#ifdef __cplusplus
}
#endif

#endif /* TENT_CONNECTOR_PROTOCOL_H */

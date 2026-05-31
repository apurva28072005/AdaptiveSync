#pragma once

// ════════════════════════════════════════════════════════════
// Wire Protocol — Frame encoding/decoding for AdaptiveSync
// ════════════════════════════════════════════════════════════

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <endian.h>    // htobe32/be32toh on Linux

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
inline void close_socket(socket_t s) { closesocket(s); }
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
using socket_t = int;
inline void close_socket(socket_t s) { close(s); }
#endif

namespace adaptivesync {

// Wire frame constants
constexpr uint32_t RAW_DATA_SENTINEL = 0xFFFFFFFF;
constexpr uint8_t  MSG_RAW_DATA       = 0xFE;
constexpr size_t   FRAME_HEADER_SIZE  = 5;  // 4 (length) + 1 (type)
constexpr size_t   RAW_HEADER_SIZE    = 13; // 4 (magic) + 1 (type) + 8 (raw length)

// ─── Host-to-network / Network-to-host helpers ─────────────

inline uint32_t host_to_net32(uint32_t val) {
    return htobe32(val);
}

inline uint32_t net_to_host32(uint32_t val) {
    return be32toh(val);
}

inline uint64_t host_to_net64(uint64_t val) {
    return htobe64(val);
}

inline uint64_t net_to_host64(uint64_t val) {
    return be64toh(val);
}

// ─── Frame structure ───────────────────────────────────────

struct WireFrame {
    uint8_t  message_type;
    std::vector<uint8_t> payload;

    WireFrame() = default;
    WireFrame(uint8_t type, const std::vector<uint8_t>& data)
        : message_type(type), payload(data) {}
};

// ─── Encoding ──────────────────────────────────────────────

inline std::vector<uint8_t> encode_frame(const WireFrame& frame) {
    std::vector<uint8_t> buffer(FRAME_HEADER_SIZE + frame.payload.size());
    uint32_t net_len = host_to_net32(static_cast<uint32_t>(frame.payload.size()));
    std::memcpy(buffer.data(), &net_len, 4);
    buffer[4] = frame.message_type;
    if (!frame.payload.empty()) {
        std::memcpy(buffer.data() + FRAME_HEADER_SIZE,
                     frame.payload.data(), frame.payload.size());
    }
    return buffer;
}

// Encode a raw-data frame header (for zero-copy follow-up)
inline std::vector<uint8_t> encode_raw_data_header(uint64_t raw_length) {
    std::vector<uint8_t> buffer(RAW_HEADER_SIZE);
    uint32_t sentinel = host_to_net32(RAW_DATA_SENTINEL);
    std::memcpy(buffer.data(), &sentinel, 4);
    buffer[4] = MSG_RAW_DATA;
    uint64_t net_len = host_to_net64(raw_length);
    std::memcpy(buffer.data() + 5, &net_len, 8);
    return buffer;
}

// ─── Decoding ──────────────────────────────────────────────

struct FrameHeader {
    uint32_t payload_length;
    uint8_t  message_type;
    bool     is_raw_data;
    uint64_t raw_data_length;  // only valid if is_raw_data == true
};

inline FrameHeader decode_frame_header(const uint8_t* data, size_t len) {
    FrameHeader hdr{};
    if (len < FRAME_HEADER_SIZE) {
        throw std::runtime_error("Frame header too short: " + std::to_string(len));
    }

    // Check for raw-data sentinel
    uint32_t first4;
    std::memcpy(&first4, data, 4);
    first4 = net_to_host32(first4);

    if (first4 == RAW_DATA_SENTINEL) {
        if (len < RAW_HEADER_SIZE) {
            throw std::runtime_error("Raw-data header too short");
        }
        hdr.is_raw_data = true;
        hdr.message_type = data[4];
        uint64_t raw_len;
        std::memcpy(&raw_len, data + 5, 8);
        hdr.raw_data_length = net_to_host64(raw_len);
        hdr.payload_length = 0;
    } else {
        hdr.is_raw_data = false;
        hdr.payload_length = first4;
        hdr.message_type = data[4];
        hdr.raw_data_length = 0;
    }
    return hdr;
}

// ─── Socket I/O helpers ────────────────────────────────────

inline ssize_t read_exact(socket_t fd, uint8_t* buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = ::recv(fd, reinterpret_cast<char*>(buf + total),
                            static_cast<int>(count - total), 0);
        if (n <= 0) {
            if (n == 0) return static_cast<ssize_t>(total);  // Connection closed
            if (errno == EINTR) continue;
            return -1;  // Error
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

inline ssize_t write_exact(socket_t fd, const uint8_t* buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = ::send(fd, reinterpret_cast<const char*>(buf + total),
                           static_cast<int>(count - total), 0);
        if (n <= 0) {
            if (n == 0) return static_cast<ssize_t>(total);
            if (errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// Read one complete wire frame from the socket
inline bool read_frame(socket_t fd, WireFrame& out_frame) {
    uint8_t hdr_buf[RAW_HEADER_SIZE];
    if (read_exact(fd, hdr_buf, FRAME_HEADER_SIZE) != static_cast<ssize_t>(FRAME_HEADER_SIZE)) {
        return false;
    }

    // Check for raw-data sentinel without calling decode_frame_header
    // (which would throw because we only have 5 bytes, not 13)
    uint32_t first4;
    std::memcpy(&first4, hdr_buf, 4);
    first4 = net_to_host32(first4);

    if (first4 == RAW_DATA_SENTINEL) {
        // Need to read remaining 8 bytes of raw-data header
        if (read_exact(fd, hdr_buf + FRAME_HEADER_SIZE, 8) != 8) {
            return false;
        }
        FrameHeader hdr = decode_frame_header(hdr_buf, RAW_HEADER_SIZE);
        out_frame.message_type = MSG_RAW_DATA;
        out_frame.payload.resize(hdr.raw_data_length);
        if (hdr.raw_data_length > 0) {
            if (read_exact(fd, out_frame.payload.data(),
                           static_cast<size_t>(hdr.raw_data_length))
                != static_cast<ssize_t>(hdr.raw_data_length)) {
                return false;
            }
        }
        return true;
    }

    // Standard frame
    out_frame.message_type = hdr_buf[4];
    out_frame.payload.resize(first4);
    if (first4 > 0) {
        if (read_exact(fd, out_frame.payload.data(), first4)
            != static_cast<ssize_t>(first4)) {
            return false;
        }
    }
    return true;
}

// Write one complete wire frame to the socket
inline bool write_frame(socket_t fd, const WireFrame& frame) {
    auto encoded = encode_frame(frame);
    return write_exact(fd, encoded.data(), encoded.size())
           == static_cast<ssize_t>(encoded.size());
}

}  // namespace adaptivesync

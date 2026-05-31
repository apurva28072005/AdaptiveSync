// ════════════════════════════════════════════════════════════
// File Manager — Implementation
// ════════════════════════════════════════════════════════════

#include "file_manager.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

// Embedded SHA-256 (lightweight, no OpenSSL dependency required)
namespace {

// ─── Embedded SHA-256 implementation ───────────────────────
// Based on FIPS 180-4. Used when OpenSSL is not available.

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

std::vector<uint8_t> sha256_raw(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85;
    uint32_t h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c;
    uint32_t h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    // Pre-processing: padding
    uint64_t bit_len = len * 8;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> padded(padded_len, 0);
    std::memcpy(padded.data(), data, len);
    padded[len] = 0x80;

    // Append length as big-endian 64-bit
    for (int i = 0; i < 8; ++i) {
        padded[padded_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    // Process each 64-byte block
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(padded[offset + i*4]) << 24)
                  | (static_cast<uint32_t>(padded[offset + i*4+1]) << 16)
                  | (static_cast<uint32_t>(padded[offset + i*4+2]) << 8)
                  | (static_cast<uint32_t>(padded[offset + i*4+3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a = h0, b = h1, c = h2, d = h3;
        uint32_t e = h4, f = h5, g = h6, hh = h7;

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + SHA256_K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += hh;
    }

    std::vector<uint8_t> result(32);
    auto put_be32 = [&](size_t pos, uint32_t val) {
        result[pos]   = static_cast<uint8_t>(val >> 24);
        result[pos+1] = static_cast<uint8_t>(val >> 16);
        result[pos+2] = static_cast<uint8_t>(val >> 8);
        result[pos+3] = static_cast<uint8_t>(val);
    };
    put_be32(0, h0);  put_be32(4, h1);
    put_be32(8, h2);  put_be32(12, h3);
    put_be32(16, h4); put_be32(20, h5);
    put_be32(24, h6); put_be32(28, h7);
    return result;
}

std::string to_hex(const std::vector<uint8_t>& data) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

}  // anonymous namespace

namespace adaptivesync {

// ─── Constructor ────────────────────────────────────────────

FileManager::FileManager(const std::string& storage_root)
    : storage_root_(storage_root)
{
    std::filesystem::create_directories(storage_root_);
}

// ─── Queries ───────────────────────────────────────────────

bool FileManager::file_exists(const std::string& relative_path) const {
    return std::filesystem::exists(resolve_path(relative_path));
}

uint64_t FileManager::get_file_size(const std::string& relative_path) const {
    auto p = resolve_path(relative_path);
    if (!std::filesystem::exists(p)) return 0;
    return static_cast<uint64_t>(std::filesystem::file_size(p));
}

uint64_t FileManager::get_last_modified_ns(const std::string& relative_path) const {
    auto p = resolve_path(relative_path);
    if (!std::filesystem::exists(p)) return 0;
    auto ftime = std::filesystem::last_write_time(p);
    auto sctp = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        ftime - std::filesystem::file_time_type::clock::now()
        + std::chrono::system_clock::now());
    auto dur = sctp.time_since_epoch();
    return static_cast<uint64_t>(dur.count());
}

std::string FileManager::compute_sha256(const std::string& relative_path) const {
    auto p = resolve_path(relative_path);
    std::ifstream file(p, std::ios::binary);
    if (!file.is_open()) return "";

    // Read entire file for hashing
    std::vector<uint8_t> buffer(std::istreambuf_iterator<char>(file), {});
    auto hash = sha256_raw(buffer.data(), buffer.size());
    return to_hex(hash);
}

std::optional<FileInfo> FileManager::get_file_info(const std::string& relative_path) const {
    if (!file_exists(relative_path)) return std::nullopt;
    FileInfo info;
    info.relative_path    = relative_path;
    info.file_size        = get_file_size(relative_path);
    info.last_modified_ns = get_last_modified_ns(relative_path);
    info.sha256_hex       = compute_sha256(relative_path);
    return info;
}

// ─── Block I/O ─────────────────────────────────────────────

std::vector<uint8_t> FileManager::read_block(const std::string& relative_path,
                                              uint64_t offset, uint32_t size) const {
    auto p = resolve_path(relative_path);
    std::ifstream file(p, std::ios::binary);
    if (!file.is_open()) return {};

    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.good()) return {};

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    auto bytes_read = static_cast<uint32_t>(file.gcount());
    buffer.resize(bytes_read);
    return buffer;
}

bool FileManager::write_block_atomic(const std::string& relative_path,
                                      uint64_t offset,
                                      const std::vector<uint8_t>& data) {
    auto tmp = tmp_path_for(relative_path);
    ensure_parent_dirs(relative_path);

    // Open or create the .tmp file
    std::fstream file(tmp, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        // Create if doesn't exist
        file.open(tmp, std::ios::out | std::ios::binary);
        if (!file.is_open()) return false;
        file.close();
        file.open(tmp, std::ios::in | std::ios::out | std::ios::binary);
    }

    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.flush();
    return file.good();
}

bool FileManager::write_file_atomic(const std::string& relative_path,
                                     const std::vector<uint8_t>& data) {
    auto tmp = tmp_path_for(relative_path);
    ensure_parent_dirs(relative_path);

    std::ofstream file(tmp, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.flush();
    return file.good();
}

// ─── Listing ───────────────────────────────────────────────

std::vector<FileInfo> FileManager::list_files(const std::string& prefix,
                                               bool recursive) const {
    std::vector<FileInfo> results;
    auto root = std::filesystem::path(storage_root_);

    if (!std::filesystem::exists(root)) return results;

    auto iterate = [&](const std::filesystem::path& dir) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            auto rel = std::filesystem::relative(entry.path(), root).string();
            if (!prefix.empty() && rel.substr(0, prefix.size()) != prefix) continue;

            FileInfo info;
            info.relative_path    = rel;
            info.file_size        = static_cast<uint64_t>(entry.file_size());
            info.last_modified_ns = get_last_modified_ns(rel);
            info.sha256_hex       = compute_sha256(rel);
            results.push_back(std::move(info));
        }
    };

    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            // Skip .tmp files
            if (entry.path().extension() == ".tmp") continue;

            auto rel = std::filesystem::relative(entry.path(), root).string();
            if (!prefix.empty() && rel.substr(0, prefix.size()) != prefix) continue;

            FileInfo info;
            info.relative_path    = rel;
            info.file_size        = static_cast<uint64_t>(entry.file_size());
            info.last_modified_ns = get_last_modified_ns(rel);
            // Defer SHA-256 for listings (expensive) — leave empty
            info.sha256_hex       = "";
            results.push_back(std::move(info));
        }
    } else {
        iterate(root);
    }

    return results;
}

// ─── Path helpers ──────────────────────────────────────────

std::string FileManager::resolve_path(const std::string& relative_path) const {
    // Sanitize: prevent directory traversal
    auto clean = std::filesystem::path(relative_path).lexically_normal();
    if (clean.string().substr(0, 2) == "..") {
        throw std::runtime_error("Directory traversal detected: " + relative_path);
    }
    return (std::filesystem::path(storage_root_) / clean).string();
}

std::string FileManager::tmp_path_for(const std::string& relative_path) const {
    return resolve_path(relative_path) + ".tmp";
}

bool FileManager::atomic_rename_tmp(const std::string& relative_path) {
    auto tmp = tmp_path_for(relative_path);
    auto target = resolve_path(relative_path);
    if (!std::filesystem::exists(tmp)) return false;

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    return !ec;
}

bool FileManager::ensure_parent_dirs(const std::string& relative_path) const {
    auto p = std::filesystem::path(resolve_path(relative_path)).parent_path();
    if (p.empty()) return true;
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec;
}

}  // namespace adaptivesync

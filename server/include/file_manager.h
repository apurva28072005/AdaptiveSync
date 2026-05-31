#pragma once

// ════════════════════════════════════════════════════════════
// File Manager — Direct OS filesystem interactions
// ════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <filesystem>

namespace adaptivesync {

struct FileInfo {
    std::string relative_path;
    uint64_t    file_size;
    uint64_t    last_modified_ns;
    std::string sha256_hex;
};

class FileManager {
public:
    explicit FileManager(const std::string& storage_root);

    // ─── Queries ────────────────────────────────────────────
    bool        file_exists(const std::string& relative_path) const;
    uint64_t    get_file_size(const std::string& relative_path) const;
    uint64_t    get_last_modified_ns(const std::string& relative_path) const;
    std::string compute_sha256(const std::string& relative_path) const;
    std::optional<FileInfo> get_file_info(const std::string& relative_path) const;

    // ─── Block I/O ─────────────────────────────────────────
    std::vector<uint8_t> read_block(const std::string& relative_path,
                                     uint64_t offset, uint32_t size) const;

    // Write block — uses .tmp + atomic rename for safety
    bool write_block_atomic(const std::string& relative_path,
                            uint64_t offset,
                            const std::vector<uint8_t>& data);

    // ─── Full file write (zero-copy upload destination) ─────
    bool write_file_atomic(const std::string& relative_path,
                           const std::vector<uint8_t>& data);

    // ─── Listing ───────────────────────────────────────────
    std::vector<FileInfo> list_files(const std::string& prefix = "",
                                      bool recursive = true) const;

    // ─── Path helpers ──────────────────────────────────────
    std::string resolve_path(const std::string& relative_path) const;
    std::string tmp_path_for(const std::string& relative_path) const;

    // ─── Atomic rename ─────────────────────────────────────
    bool atomic_rename_tmp(const std::string& relative_path);

    // ─── Directory creation ────────────────────────────────
    bool ensure_parent_dirs(const std::string& relative_path) const;

    const std::string& storage_root() const { return storage_root_; }

private:
    std::string storage_root_;
};

}  // namespace adaptivesync

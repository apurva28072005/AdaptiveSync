#pragma once

// ════════════════════════════════════════════════════════════
// File Mutex — Thread-safe per-file read/write locking
// ════════════════════════════════════════════════════════════

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>

namespace adaptivesync {

class FileMutex {
public:
    FileMutex() = default;

    // Acquire shared (read) access for a file path
    void acquire_read(const std::string& path);

    // Release shared (read) access
    void release_read(const std::string& path);

    // Acquire exclusive (write) access for a file path
    void acquire_write(const std::string& path);

    // Release exclusive (write) access
    void release_write(const std::string& path);

    // Clean up entries that have no active holders
    void cleanup_unused();

private:
    struct MutexEntry {
        std::shared_ptr<std::shared_mutex> mtx;
        int active_count = 0;
    };

    std::unordered_map<std::string, MutexEntry> mutex_map_;
    std::mutex map_mutex_;  // Protects the mutex_map_ itself

    std::shared_ptr<std::shared_mutex> get_or_create(const std::string& path);
};

}  // namespace adaptivesync

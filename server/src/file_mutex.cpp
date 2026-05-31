// ════════════════════════════════════════════════════════════
// File Mutex — Implementation
// ════════════════════════════════════════════════════════════

#include "file_mutex.h"

namespace adaptivesync {

std::shared_ptr<std::shared_mutex> FileMutex::get_or_create(const std::string& path) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = mutex_map_.find(path);
    if (it != mutex_map_.end()) {
        it->second.active_count++;
        return it->second.mtx;
    }
    MutexEntry entry;
    entry.mtx = std::make_shared<std::shared_mutex>();
    entry.active_count = 1;
    auto result = mutex_map_.emplace(path, std::move(entry));
    return result.first->second.mtx;
}

void FileMutex::acquire_read(const std::string& path) {
    auto mtx = get_or_create(path);
    mtx->lock_shared();
}

void FileMutex::release_read(const std::string& path) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = mutex_map_.find(path);
    if (it != mutex_map_.end()) {
        it->second.mtx->unlock_shared();
        it->second.active_count--;
    }
}

void FileMutex::acquire_write(const std::string& path) {
    auto mtx = get_or_create(path);
    mtx->lock();
}

void FileMutex::release_write(const std::string& path) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = mutex_map_.find(path);
    if (it != mutex_map_.end()) {
        it->second.mtx->unlock();
        it->second.active_count--;
    }
}

void FileMutex::cleanup_unused() {
    std::lock_guard<std::mutex> lock(map_mutex_);
    for (auto it = mutex_map_.begin(); it != mutex_map_.end(); ) {
        if (it->second.active_count <= 0) {
            it = mutex_map_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace adaptivesync

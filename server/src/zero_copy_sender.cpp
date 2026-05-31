// ════════════════════════════════════════════════════════════
// Zero-Copy Sender — Implementation
// ════════════════════════════════════════════════════════════

#include "zero_copy_sender.h"

#include <fcntl.h>    // for ::open(), O_RDONLY
#include <unistd.h>   // for ::close(), ::read(), ::lseek()
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#endif

#include "adaptivesync.pb.h"  // Generated protobuf header

namespace adaptivesync {

ZeroCopySender::ZeroCopySender(FileManager& file_manager)
    : file_manager_(file_manager)
{}

bool ZeroCopySender::send_file(socket_t client_fd,
                                const std::string& relative_path,
                                uint64_t offset)
{
    auto full_path = file_manager_.resolve_path(relative_path);

    if (!file_manager_.file_exists(relative_path)) {
        std::cerr << "[ZeroCopy] File not found: " << relative_path << std::endl;
        return false;
    }

    uint64_t total_size = file_manager_.get_file_size(relative_path);
    if (total_size == 0) {
        std::cerr << "[ZeroCopy] File is empty: " << relative_path << std::endl;
        return false;
    }

    if (offset >= total_size) {
        std::cerr << "[ZeroCopy] Offset beyond file size" << std::endl;
        return false;
    }

    uint64_t bytes_to_send = total_size - offset;

    // ─── Step 1: Send ZeroCopyDownloadHeader protobuf ──────
    adaptivesync::ZeroCopyDownloadHeader header;
    header.set_relative_path(relative_path);
    header.set_total_size(total_size);
    header.set_sha256_hash(file_manager_.compute_sha256(relative_path));
    header.set_offset(offset);

    std::vector<uint8_t> serialized(header.ByteSizeLong());
    header.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame header_frame(static_cast<uint8_t>(adaptivesync::MSG_ZERO_COPY_DOWNLOAD_HEADER),
                           serialized);
    if (!write_frame(client_fd, header_frame)) {
        std::cerr << "[ZeroCopy] Failed to send download header" << std::endl;
        return false;
    }

    // ─── Step 2: Send raw-data frame header ────────────────
    auto raw_hdr = encode_raw_data_header(bytes_to_send);
    if (write_exact(client_fd, raw_hdr.data(), raw_hdr.size())
        != static_cast<ssize_t>(raw_hdr.size())) {
        std::cerr << "[ZeroCopy] Failed to send raw-data header" << std::endl;
        return false;
    }

    // ─── Step 3: Stream the file bytes ─────────────────────
    std::cout << "[ZeroCopy] Sending " << bytes_to_send << " bytes of "
              << relative_path << " (offset=" << offset << ")" << std::endl;

#ifdef __linux__
    // ─── Linux: use sendfile() for kernel-space zero-copy ──
    int file_fd = ::open(full_path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        std::cerr << "[ZeroCopy] Failed to open file: " << full_path << std::endl;
        return false;
    }

    bool success = send_file_linux(client_fd, file_fd, offset, bytes_to_send);
    ::close(file_fd);
    return success;

#elif defined(_WIN32)
    // ─── Windows: use TransmitFile() ──────────────────────
    HANDLE hFile = CreateFileA(full_path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "[ZeroCopy] Failed to open file (Windows)" << std::endl;
        return false;
    }

    bool success = send_file_windows(client_fd, hFile, offset, bytes_to_send);
    CloseHandle(hFile);
    return success;

#else
    // ─── Fallback: read/send loop ─────────────────────────
    int file_fd = ::open(full_path.c_str(), O_RDONLY);
    if (file_fd < 0) return false;

    bool success = send_file_fallback(client_fd, file_fd, offset, bytes_to_send);
    ::close(file_fd);
    return success;
#endif
}

// ─── Linux sendfile() ──────────────────────────────────────

bool ZeroCopySender::send_file_linux(socket_t client_fd, int file_fd,
                                      uint64_t offset, uint64_t total_size)
{
    uint64_t remaining = total_size;
    off_t file_offset = static_cast<off_t>(offset);

    auto start = std::chrono::steady_clock::now();

    while (remaining > 0) {
        size_t chunk = std::min(remaining, static_cast<uint64_t>(0x7FFFFF00));  // sendfile max
        ssize_t sent = ::sendfile(client_fd, file_fd, &file_offset, chunk);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            std::cerr << "[ZeroCopy] sendfile() error: " << strerror(errno) << std::endl;
            return false;
        }

        remaining -= static_cast<uint64_t>(sent);

        // Progress logging
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start).count();
        if (elapsed_ms > 0 && (elapsed_ms % 1000 < 50)) {  // ~every second
            double rate = (total_size - remaining) / (1024.0 * 1024.0) / (elapsed_ms / 1000.0);
            std::cout << "[ZeroCopy] Progress: "
                      << (total_size - remaining) << "/" << total_size
                      << " bytes (" << rate << " MB/s)" << std::endl;
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double avg_rate = (total_size / (1024.0 * 1024.0)) / (total_ms / 1000.0);
    std::cout << "[ZeroCopy] Complete: " << total_size << " bytes in "
              << total_ms << "ms (" << avg_rate << " MB/s)" << std::endl;
    return true;
}

// ─── Windows TransmitFile() ────────────────────────────────

bool ZeroCopySender::send_file_windows(socket_t client_fd, void* file_handle,
                                        uint64_t offset, uint64_t total_size)
{
#ifdef _WIN32
    // Use TransmitFile for kernel-space zero-copy on Windows
    LPFN_TRANSMITFILE pTransmitFile = nullptr;
    GUID GuidTransmitFile = WSAID_TRANSMITFILE;
    DWORD dwBytes;

    int result = WSAIoctl(client_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                          &GuidTransmitFile, sizeof(GuidTransmitFile),
                          &pTransmitFile, sizeof(pTransmitFile),
                          &dwBytes, NULL, NULL);

    if (result == SOCKET_ERROR || !pTransmitFile) {
        std::cerr << "[ZeroCopy] TransmitFile not available, using fallback" << std::endl;
        // Fallback to read/send loop
        int file_fd = _open(full_path.c_str(), _O_RDONLY | _O_BINARY);
        if (file_fd < 0) return false;
        return send_file_fallback(client_fd, file_fd, offset, total_size);
    }

    // Set file pointer to offset
    LARGE_INTEGER li_offset;
    li_offset.QuadPart = offset;
    SetFilePointerEx(file_handle, li_offset, NULL, FILE_BEGIN);

    // TransmitFile sends the entire file (or nNumberOfBytesToWrite)
    BOOL ok = pTransmitFile(client_fd, file_handle,
                            static_cast<DWORD>(total_size), 0, NULL, NULL,
                            TF_USE_KERNEL_APC);
    if (!ok) {
        std::cerr << "[ZeroCopy] TransmitFile failed: " << WSAGetLastError() << std::endl;
        return false;
    }
    return true;
#else
    (void)client_fd; (void)file_handle; (void)offset; (void)total_size;
    return false;
#endif
}

// ─── Fallback: read/send loop ──────────────────────────────

bool ZeroCopySender::send_file_fallback(socket_t client_fd, int file_fd,
                                         uint64_t offset, uint64_t total_size)
{
    constexpr size_t BUFFER_SIZE = 256 * 1024;  // 256KB chunks
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    if (offset > 0) {
        ::lseek(file_fd, static_cast<off_t>(offset), SEEK_SET);
    }

    uint64_t remaining = total_size;
    auto start = std::chrono::steady_clock::now();

    while (remaining > 0) {
        size_t to_read = std::min(remaining, static_cast<uint64_t>(BUFFER_SIZE));
        ssize_t bytes_read = ::read(file_fd, buffer.data(), to_read);
        if (bytes_read <= 0) {
            std::cerr << "[ZeroCopy] Read error in fallback path" << std::endl;
            return false;
        }

        ssize_t sent = write_exact(client_fd, buffer.data(),
                                    static_cast<size_t>(bytes_read));
        if (sent != bytes_read) {
            std::cerr << "[ZeroCopy] Write error in fallback path" << std::endl;
            return false;
        }

        remaining -= static_cast<uint64_t>(bytes_read);
    }

    auto end = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double avg_rate = (total_size / (1024.0 * 1024.0)) / (total_ms / 1000.0 + 0.001);
    std::cout << "[ZeroCopy] Fallback complete: " << total_size << " bytes in "
              << total_ms << "ms (" << avg_rate << " MB/s)" << std::endl;
    return true;
}

}  // namespace adaptivesync

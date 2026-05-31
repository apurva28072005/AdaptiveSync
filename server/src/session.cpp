// ════════════════════════════════════════════════════════════
// Session — Per-client connection state machine
// ════════════════════════════════════════════════════════════

#include "session.h"
#include "wire_protocol.h"

#include <iostream>
#include <chrono>
#include <cstring>

#include "adaptivesync.pb.h"  // Generated protobuf header

namespace adaptivesync {

Session::Session(socket_t client_fd,
                 const std::string& client_addr,
                 FileManager& file_manager,
                 FileMutex& file_mutex,
                 uint32_t block_size)
    : client_fd_(client_fd)
    , client_addr_(client_addr)
    , state_(SessionState::AWAITING_HELLO)
    , shutdown_requested_(false)
    , file_manager_(file_manager)
    , file_mutex_(file_mutex)
    , negotiated_block_size_(block_size)
    , protocol_version_(1)
{
    negotiator_       = std::make_unique<Negotiator>(file_manager_, block_size);
    zero_copy_sender_ = std::make_unique<ZeroCopySender>(file_manager_);
    delta_sync_handler_ = std::make_unique<DeltaSyncHandler>(
        file_manager_, file_mutex_, block_size);

    std::cout << "[Session] New connection from " << client_addr_ << std::endl;
}

Session::~Session() {
    close_socket(client_fd_);
    std::cout << "[Session] Closed connection: " << client_addr_
              << " (client_id=" << client_id_ << ")" << std::endl;
}

void Session::run() {
    while (!shutdown_requested_ && state_ != SessionState::DISCONNECTED) {
        WireFrame frame;
        if (!read_frame(client_fd_, frame)) {
            if (!shutdown_requested_) {
                std::cout << "[Session] Client disconnected: " << client_addr_ << std::endl;
            }
            state_ = SessionState::DISCONNECTED;
            break;
        }

        // Dispatch based on message type
        switch (frame.message_type) {
            case adaptivesync::MSG_CLIENT_HELLO:
                handle_client_hello(frame.payload);
                break;

            case adaptivesync::MSG_NEGOTIATION_REQUEST:
                handle_negotiation_request(frame.payload);
                break;

            case adaptivesync::MSG_ZERO_COPY_DOWNLOAD_REQUEST:
                handle_zero_copy_download_request(frame.payload);
                break;

            case adaptivesync::MSG_ZERO_COPY_UPLOAD_REQUEST:
                handle_zero_copy_upload_request(frame.payload);
                break;

            case adaptivesync::MSG_DELTA_SYNC_SIGNATURE_REQUEST:
                handle_delta_sync_signature_request(frame.payload);
                break;

            case adaptivesync::MSG_DELTA_SYNC_BLOCK_DATA:
                handle_delta_sync_block_data(frame.payload);
                break;

            case adaptivesync::MSG_LIST_FILES_REQUEST:
                handle_list_files_request(frame.payload);
                break;

            default:
                std::cerr << "[Session] Unknown message type: "
                          << static_cast<int>(frame.message_type) << std::endl;
                send_error(6, "Unknown message type");
                break;
        }
    }
}

void Session::shutdown() {
    shutdown_requested_ = true;
}

// ─── Message Handlers ──────────────────────────────────────

void Session::handle_client_hello(const std::vector<uint8_t>& payload) {
    adaptivesync::ClientHello hello;
    if (!hello.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse ClientHello");
        return;
    }

    client_id_ = hello.client_id();
    protocol_version_ = hello.protocol_version();

    // Negotiate block size: use client's preferred if reasonable
    if (hello.block_size() >= 4096 && hello.block_size() <= 1048576) {
        negotiated_block_size_ = hello.block_size();
    }

    std::cout << "[Session] Client hello: id=" << client_id_
              << " protocol=" << protocol_version_
              << " block_size=" << negotiated_block_size_ << std::endl;

    // Send ServerHello
    adaptivesync::ServerHello server_hello;
    server_hello.set_protocol_version(1);
    server_hello.set_server_id("adaptivesync-cpp-1.0");
    server_hello.add_supported_modes(adaptivesync::ZERO_COPY);
    server_hello.add_supported_modes(adaptivesync::DELTA_SYNC);
    server_hello.set_max_concurrent_transfers(10);
    server_hello.set_max_file_size(0);  // No limit
    server_hello.set_block_size(negotiated_block_size_);

    std::vector<uint8_t> serialized(server_hello.ByteSizeLong());
    server_hello.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_SERVER_HELLO), serialized);
    write_frame(client_fd_, response);

    state_ = SessionState::READY;
}

void Session::handle_negotiation_request(const std::vector<uint8_t>& payload) {
    adaptivesync::NegotiationRequest req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse NegotiationRequest");
        return;
    }

    state_ = SessionState::NEGOTIATING;

    auto result = negotiator_->negotiate(
        req.relative_path(),
        req.client_file_size(),
        req.client_last_modified_ns(),
        req.client_sha256_hash());

    adaptivesync::NegotiationResponse resp;
    resp.set_file_mode(static_cast<adaptivesync::FileMode>(result.file_mode));
    resp.set_transfer_mode(static_cast<adaptivesync::TransferMode>(result.transfer_mode));
    resp.set_server_file_size(result.server_file_size);
    resp.set_server_sha256_hash(result.server_sha256);
    resp.set_block_size(result.block_size);

    std::vector<uint8_t> serialized(resp.ByteSizeLong());
    resp.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_NEGOTIATION_RESPONSE), serialized);
    write_frame(client_fd_, response);

    state_ = SessionState::READY;

    std::cout << "[Session] Negotiation: " << req.relative_path()
              << " -> file_mode=" << result.file_mode
              << " transfer_mode=" << result.transfer_mode << std::endl;
}

void Session::handle_zero_copy_download_request(const std::vector<uint8_t>& payload) {
    adaptivesync::ZeroCopyDownloadRequest req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse ZeroCopyDownloadRequest");
        return;
    }

    state_ = SessionState::TRANSFERRING;

    bool ok = zero_copy_sender_->send_file(client_fd_, req.relative_path(), req.offset());
    if (!ok) {
        send_error(3, "File not found or transfer failed: " + req.relative_path());
    }

    state_ = SessionState::READY;
}

void Session::handle_zero_copy_upload_request(const std::vector<uint8_t>& payload) {
    adaptivesync::ZeroCopyUploadRequest req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse ZeroCopyUploadRequest");
        return;
    }

    const auto& metadata = req.metadata();
    upload_state_.relative_path = metadata.relative_path();
    upload_state_.total_size    = metadata.file_size();
    upload_state_.received      = 0;
    upload_state_.expected_sha256 = metadata.sha256_hash();
    upload_state_.buffer.clear();
    upload_state_.buffer.reserve(metadata.file_size());

    state_ = SessionState::TRANSFERRING;

    // Send acknowledgment — client can start sending raw data
    adaptivesync::ZeroCopyUploadAck ack;
    ack.set_status(adaptivesync::OK);
    ack.set_offset(0);  // No resume support in initial version

    std::vector<uint8_t> serialized(ack.ByteSizeLong());
    ack.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_ZERO_COPY_UPLOAD_ACK), serialized);
    write_frame(client_fd_, response);

    std::cout << "[Session] Upload accepted: " << upload_state_.relative_path
              << " (" << upload_state_.total_size << " bytes)" << std::endl;

    // Read raw data frames until complete
    auto start = std::chrono::steady_clock::now();
    while (upload_state_.received < upload_state_.total_size) {
        WireFrame data_frame;
        if (!read_frame(client_fd_, data_frame)) {
            send_error(2, "Connection lost during upload");
            break;
        }

        if (data_frame.message_type == MSG_RAW_DATA) {
            upload_state_.buffer.insert(
                upload_state_.buffer.end(),
                data_frame.payload.begin(),
                data_frame.payload.end());
            upload_state_.received += data_frame.payload.size();

            // Progress reporting
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start).count();
            if (elapsed_ms > 0) {
                float rate = (upload_state_.received / (1024.0f * 1024.0f)) /
                             (elapsed_ms / 1000.0f);
                send_progress(upload_state_.relative_path,
                             upload_state_.received,
                             upload_state_.total_size,
                             rate,
                             static_cast<uint32_t>(elapsed_ms));
            }
        }
    }

    // Verify and save
    if (upload_state_.received == upload_state_.total_size) {
        bool ok = file_manager_.write_file_atomic(
            upload_state_.relative_path, upload_state_.buffer);
        if (ok) {
            file_manager_.atomic_rename_tmp(upload_state_.relative_path);
            std::cout << "[Session] Upload complete: " << upload_state_.relative_path << std::endl;
        } else {
            send_error(2, "Failed to save uploaded file");
        }
    }

    state_ = SessionState::READY;
}

void Session::handle_delta_sync_signature_request(const std::vector<uint8_t>& payload) {
    adaptivesync::DeltaSyncSignatureRequest req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse DeltaSyncSignatureRequest");
        return;
    }

    state_ = SessionState::NEGOTIATING;

    // Convert protobuf block signatures to internal format
    std::vector<DeltaBlockSignature> client_sigs;
    client_sigs.reserve(req.signatures_size());
    for (const auto& sig : req.signatures()) {
        DeltaBlockSignature bs;
        bs.block_index  = sig.block_index();
        bs.rolling_hash = sig.rolling_hash();
        bs.strong_hash  = sig.strong_hash();
        bs.block_offset = sig.block_offset();
        bs.block_size   = sig.block_size();
        client_sigs.push_back(bs);
    }

    // Perform block comparison
    auto result = delta_sync_handler_->handle_signature_request(
        req.relative_path(), req.file_size(), req.block_size(), client_sigs);

    // Send block request to client
    adaptivesync::DeltaSyncBlockRequest block_req;
    block_req.set_relative_path(result.relative_path);
    for (uint32_t idx : result.requested_indices) {
        block_req.add_requested_block_indices(idx);
    }

    std::vector<uint8_t> serialized(block_req.ByteSizeLong());
    block_req.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_DELTA_SYNC_BLOCK_REQUEST), serialized);
    write_frame(client_fd_, response);

    state_ = SessionState::READY;

    std::cout << "[Session] Delta-sync: requested "
              << result.requested_indices.size() << " blocks for "
              << req.relative_path() << std::endl;
}

void Session::handle_delta_sync_block_data(const std::vector<uint8_t>& payload) {
    adaptivesync::DeltaSyncBlockData req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse DeltaSyncBlockData");
        return;
    }

    state_ = SessionState::TRANSFERRING;

    std::vector<uint8_t> data(req.data().begin(), req.data().end());

    // Determine if this is the final block (heuristic: check if block_size < negotiated)
    bool is_final = (data.size() < negotiated_block_size_);

    int status = delta_sync_handler_->handle_block_data(
        req.relative_path(),
        req.block_index(),
        req.block_offset(),
        data,
        req.strong_hash(),
        is_final);

    if (is_final || status != 1) {
        // Send completion status
        adaptivesync::DeltaSyncComplete complete;
        complete.set_relative_path(req.relative_path());
        complete.set_status(static_cast<adaptivesync::StatusCode>(status));

        if (status == 1) {
            complete.set_final_sha256_hash(
                file_manager_.compute_sha256(req.relative_path()));
        } else {
            complete.set_error_message("Block write failed");
        }

        std::vector<uint8_t> serialized(complete.ByteSizeLong());
        complete.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

        WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_DELTA_SYNC_COMPLETE), serialized);
        write_frame(client_fd_, response);
    }

    state_ = SessionState::READY;
}

void Session::handle_list_files_request(const std::vector<uint8_t>& payload) {
    adaptivesync::ListFilesRequest req;
    if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        send_error(6, "Failed to parse ListFilesRequest");
        return;
    }

    auto files = file_manager_.list_files(req.directory_prefix(), req.recursive());

    adaptivesync::ListFilesResponse resp;
    for (const auto& fi : files) {
        auto* entry = resp.add_files();
        entry->set_relative_path(fi.relative_path);
        entry->set_file_size(fi.file_size);
        entry->set_last_modified_ns(fi.last_modified_ns);
        entry->set_sha256_hash(fi.sha256_hex);
        entry->set_file_mode(adaptivesync::FRESH);  // Mode is determined per-negotiation
    }

    std::vector<uint8_t> serialized(resp.ByteSizeLong());
    resp.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame response(static_cast<uint8_t>(adaptivesync::MSG_LIST_FILES_RESPONSE), serialized);
    write_frame(client_fd_, response);

    std::cout << "[Session] Listed " << files.size() << " files" << std::endl;
}

// ─── Helpers ───────────────────────────────────────────────

void Session::send_error(int status_code, const std::string& message) {
    adaptivesync::ErrorPayload err;
    err.set_code(static_cast<adaptivesync::StatusCode>(status_code));
    err.set_message(message);

    std::vector<uint8_t> serialized(err.ByteSizeLong());
    err.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame frame(static_cast<uint8_t>(adaptivesync::MSG_ERROR), serialized);
    write_frame(client_fd_, frame);

    std::cerr << "[Session] Error: " << message << std::endl;
}

void Session::send_progress(const std::string& path,
                             uint64_t bytes_transferred,
                             uint64_t bytes_total,
                             float rate_mbps,
                             uint32_t elapsed_ms)
{
    adaptivesync::ProgressUpdate prog;
    prog.set_relative_path(path);
    prog.set_bytes_transferred(bytes_transferred);
    prog.set_bytes_total(bytes_total);
    prog.set_rate_mbps(rate_mbps);
    prog.set_elapsed_ms(elapsed_ms);

    std::vector<uint8_t> serialized(prog.ByteSizeLong());
    prog.SerializeToArray(serialized.data(), static_cast<int>(serialized.size()));

    WireFrame frame(static_cast<uint8_t>(adaptivesync::MSG_PROGRESS), serialized);
    write_frame(client_fd_, frame);
}

}  // namespace adaptivesync

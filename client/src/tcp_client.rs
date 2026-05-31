// ════════════════════════════════════════════════════════════
// TCP Client — Connection management and protocol dispatch
// ════════════════════════════════════════════════════════════

use tokio::net::TcpStream;
use tokio::io::AsyncWriteExt;

use crate::proto::*;
use crate::wire;
use crate::delta_sync::DeltaSyncEngine;
use crate::file_ops::FileOps;
use crate::ClientError;

/// The AdaptiveSync TCP client
pub struct TcpClient {
    stream: Option<TcpStream>,
    server_addr: String,
    client_id: String,
    block_size: u32,
    protocol_version: u32,
    file_ops: FileOps,
    delta_engine: DeltaSyncEngine,
    connected: bool,
}

/// Result of a negotiation
#[derive(Debug)]
pub struct NegotiationResult {
    pub file_mode: i32,
    pub transfer_mode: i32,
    pub server_file_size: u64,
    pub server_sha256: String,
    pub block_size: u32,
}

/// Progress callback type
pub type ProgressCallback = Box<dyn Fn(&str, u64, u64, f32, u32) + Send + Sync>;

/// Read frames until a specific message type is received, ignoring progress frames
async fn read_expected(stream: &mut TcpStream, expected_type: u8) -> crate::Result<wire::WireFrame> {
    for _ in 0..100 {
        let frame = wire::read_frame(stream).await?;
        if frame.message_type == expected_type {
            return Ok(frame);
        }
        log::debug!("[Client] Ignored unexpected frame type {} while waiting for {}", frame.message_type, expected_type);
    }
    Err(ClientError::Protocol(format!("Too many unexpected frames while waiting for type {}", expected_type)))
}

impl TcpClient {
    /// Create a new client instance
    pub fn new(
        server_addr: &str,
        sync_root: impl Into<std::path::PathBuf>,
        block_size: u32,
    ) -> Self {
        let file_ops = FileOps::new(sync_root, block_size);
        let delta_engine = DeltaSyncEngine::new(FileOps::new(
            file_ops.sync_root().to_path_buf(),
            block_size,
        ));

        Self {
            stream: None,
            server_addr: server_addr.to_string(),
            client_id: format!("rust-client-{}", std::process::id()),
            block_size,
            protocol_version: 1,
            file_ops,
            delta_engine,
            connected: false,
        }
    }

    /// Connect to the server and perform handshake
    pub async fn connect(&mut self) -> crate::Result<()> {
        let mut stream = TcpStream::connect(&self.server_addr).await
            .map_err(|e| ClientError::Connection(
                format!("Failed to connect to {}: {}", self.server_addr, e)))?;

        // ─── Send ClientHello ──────────────────────────────
        let mut hello = ClientHello::default();
        hello.protocol_version = self.protocol_version;
        hello.client_id = self.client_id.clone();
        hello.supported_modes.push(TransferMode::ZeroCopy as i32);
        hello.supported_modes.push(TransferMode::DeltaSync as i32);
        hello.block_size = self.block_size;

        let payload = prost::Message::encode_to_vec(&hello);
        wire::write_frame(&mut stream, MessageType::MsgClientHello, &payload).await?;

        // ─── Read ServerHello ──────────────────────────────
        let frame = wire::read_frame(&mut stream).await?;
        if frame.message_type != MessageType::MsgServerHello as u8 {
            return Err(ClientError::Protocol(
                format!("Expected ServerHello, got type {}", frame.message_type)));
        }

        let server_hello: ServerHello = prost::Message::decode(frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse ServerHello: {}", e)))?;

        // Negotiate block size
        if server_hello.block_size >= 4096 && server_hello.block_size <= 1048576 {
            self.block_size = server_hello.block_size;
        }

        log::info!(
            "[Client] Connected to server {} (protocol={}, block_size={})",
            server_hello.server_id,
            server_hello.protocol_version,
            self.block_size
        );

        self.stream = Some(stream);
        self.connected = true;
        Ok(())
    }

    /// Disconnect from the server
    pub async fn disconnect(&mut self) {
        if let Some(mut stream) = self.stream.take() {
            // Best-effort close
            let _ = stream.shutdown().await;
        }
        self.connected = false;
    }

    /// Check if connected
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    /// Get the negotiated block size
    pub fn block_size(&self) -> u32 {
        self.block_size
    }

    /// Negotiate the transfer mode for a file
    pub async fn negotiate(&mut self, relative_path: &str) -> crate::Result<NegotiationResult> {
        let stream = self.stream.as_mut()
            .ok_or(ClientError::Connection("Not connected".to_string()))?;

        // Gather local file info
        let file_size = self.file_ops.file_size(relative_path).unwrap_or(0);
        let last_modified = self.file_ops.last_modified_ns(relative_path).unwrap_or(0);
        let sha256 = self.file_ops.sha256(relative_path).unwrap_or_default();

        let mut req = NegotiationRequest::default();
        req.relative_path = relative_path.to_string();
        req.client_file_size = file_size;
        req.client_last_modified_ns = last_modified;
        req.client_sha256_hash = sha256;

        let payload = prost::Message::encode_to_vec(&req);
        wire::write_frame(stream, MessageType::MsgNegotiationRequest, &payload).await?;

        // Read response (ignore progress frames)
        let frame = read_expected(stream, MessageType::MsgNegotiationResponse as u8).await?;
        let resp: NegotiationResponse = prost::Message::decode(frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse NegotiationResponse: {}", e)))?;

        Ok(NegotiationResult {
            file_mode: resp.file_mode,
            transfer_mode: resp.transfer_mode,
            server_file_size: resp.server_file_size,
            server_sha256: resp.server_sha256_hash,
            block_size: resp.block_size,
        })
    }

    /// Download a file from the server using zero-copy
    pub async fn download_file(
        &mut self,
        relative_path: &str,
        _progress: Option<&ProgressCallback>,
    ) -> crate::Result<String> {
        let stream = self.stream.as_mut()
            .ok_or(ClientError::Connection("Not connected".to_string()))?;

        // Send download request
        let mut req = ZeroCopyDownloadRequest::default();
        req.relative_path = relative_path.to_string();
        req.offset = 0;

        let payload = prost::Message::encode_to_vec(&req);
        wire::write_frame(stream, MessageType::MsgZeroCopyDownloadRequest, &payload).await?;

        // Read download header (ignore progress frames)
        let header_frame = read_expected(stream, MessageType::MsgZeroCopyDownloadHeader as u8).await?;

        let header: ZeroCopyDownloadHeader = prost::Message::decode(header_frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse header: {}", e)))?;

        // Read raw data
        let raw_frame = wire::read_frame(stream).await?;
        if raw_frame.message_type != 0xFE {
            return Err(ClientError::Protocol("Expected raw data frame".to_string()));
        }

        // Write to local file
        self.file_ops.write_file(relative_path, &raw_frame.payload)?;

        // Verify hash
        let local_hash = self.file_ops.sha256(relative_path)?;
        if !header.sha256_hash.is_empty() && local_hash != header.sha256_hash {
            return Err(ClientError::HashMismatch {
                expected: header.sha256_hash,
                actual: local_hash,
            });
        }

        log::info!("[Client] Download complete: {} ({} bytes)", relative_path, header.total_size);
        Ok(local_hash)
    }

    /// Upload a file to the server using zero-copy
    pub async fn upload_file(
        &mut self,
        relative_path: &str,
        _progress: Option<&ProgressCallback>,
    ) -> crate::Result<String> {
        let stream = self.stream.as_mut()
            .ok_or(ClientError::Connection("Not connected".to_string()))?;

        // Gather local file info
        let file_info = self.file_ops.get_file_info(relative_path)?;
        let file_hash = file_info.sha256_hash.clone();

        let mut metadata = FileMetadata::default();
        metadata.relative_path = file_info.relative_path;
        metadata.file_size = file_info.file_size;
        metadata.last_modified_ns = file_info.last_modified_ns;
        metadata.sha256_hash = file_info.sha256_hash;
        metadata.file_mode = FileMode::Fresh as i32;

        let mut upload_req = ZeroCopyUploadRequest::default();
        upload_req.metadata = Some(metadata);

        let payload = prost::Message::encode_to_vec(&upload_req);
        wire::write_frame(stream, MessageType::MsgZeroCopyUploadRequest, &payload).await?;

        // Read upload acknowledgment
        let ack_frame = wire::read_frame(stream).await?;
        let ack: ZeroCopyUploadAck = prost::Message::decode(ack_frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse upload ack: {}", e)))?;

        if ack.status != StatusCode::Ok as i32 {
            return Err(ClientError::ServerError {
                code: ack.status,
                message: ack.error_message,
            });
        }

        // Read the local file and send as raw data
        let file_data = std::fs::read(self.file_ops.resolve_path(relative_path))
            .map_err(|e| ClientError::FileIo(format!("Cannot read file: {}", e)))?;

        wire::write_raw_data(stream, &file_data).await?;

        log::info!("[Client] Upload complete: {} ({} bytes)", relative_path, file_data.len());
        Ok(file_hash)
    }

    /// Perform a delta-sync upload: compute signatures, get block requests, send blocks
    pub async fn delta_sync_upload(&mut self, relative_path: &str) -> crate::Result<String> {
        let stream = self.stream.as_mut()
            .ok_or(ClientError::Connection("Not connected".to_string()))?;

        // Step 1: Compute block signatures
        let signatures = self.delta_engine.compute_signatures(relative_path)?;
        let file_size = self.file_ops.file_size(relative_path)?;

        let sig_request = self.delta_engine.build_signature_request(
            relative_path, file_size, self.block_size, &signatures);

        let payload = prost::Message::encode_to_vec(&sig_request);
        wire::write_frame(stream, MessageType::MsgDeltaSyncSignatureRequest, &payload).await?;

        // Step 2: Read block request from server
        let block_req_frame = wire::read_frame(stream).await?;
        let block_req: DeltaSyncBlockRequest = prost::Message::decode(
            block_req_frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse block request: {}", e)))?;

        log::info!(
            "[Client] Server requested {} blocks for delta-sync",
            block_req.requested_block_indices.len()
        );

        // Step 3: Read and send requested blocks
        let blocks = self.delta_engine.read_requested_blocks(
            relative_path, &block_req.requested_block_indices)?;

        for block_data in &blocks {
            let payload = prost::Message::encode_to_vec(block_data);
            wire::write_frame(stream, MessageType::MsgDeltaSyncBlockData, &payload).await?;
        }

        // Step 4: Read completion status
        let complete_frame = wire::read_frame(stream).await?;
        let complete: DeltaSyncComplete = prost::Message::decode(
            complete_frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse completion: {}", e)))?;

        if complete.status != StatusCode::Ok as i32 {
            return Err(ClientError::ServerError {
                code: complete.status,
                message: complete.error_message,
            });
        }

        let final_hash = complete.final_sha256_hash;

        log::info!(
            "[Client] Delta-sync upload complete: {} (final hash: {})",
            relative_path, final_hash
        );

        Ok(final_hash)
    }

    /// List files on the server
    pub async fn list_server_files(&mut self, prefix: &str, recursive: bool) -> crate::Result<Vec<FileMetadata>> {
        let stream = self.stream.as_mut()
            .ok_or(ClientError::Connection("Not connected".to_string()))?;

        let mut req = ListFilesRequest::default();
        req.directory_prefix = prefix.to_string();
        req.recursive = recursive;

        let payload = prost::Message::encode_to_vec(&req);
        wire::write_frame(stream, MessageType::MsgListFilesRequest, &payload).await?;

        let frame = read_expected(stream, MessageType::MsgListFilesResponse as u8).await?;
        let resp: ListFilesResponse = prost::Message::decode(frame.payload.as_slice())
            .map_err(|e| ClientError::Protocol(format!("Failed to parse list response: {}", e)))?;
        Ok(resp.files)
    }

    /// Smart sync: negotiate first, then use the optimal transfer mode
    pub async fn sync_file(&mut self, relative_path: &str) -> crate::Result<String> {
        let negotiation = self.negotiate(relative_path).await?;

        match negotiation.file_mode {
            x if x == FileMode::Unchanged as i32 => {
                log::info!("[Client] File unchanged, no transfer needed: {}", relative_path);
                Ok(negotiation.server_sha256)
            }
            x if x == FileMode::Fresh as i32 => {
                log::info!("[Client] Fresh transfer (zero-copy download): {}", relative_path);
                self.download_file(relative_path, None).await
            }
            x if x == FileMode::Modified as i32 => {
                if negotiation.transfer_mode == TransferMode::DeltaSync as i32 {
                    log::info!("[Client] Delta-sync upload: {}", relative_path);
                    self.delta_sync_upload(relative_path).await
                } else {
                    log::info!("[Client] Full re-upload: {}", relative_path);
                    self.upload_file(relative_path, None).await
                }
            }
            _ => Err(ClientError::Protocol("Unknown file mode".to_string())),
        }
    }
}

impl Drop for TcpClient {
    fn drop(&mut self) {
        if self.connected {
            // Best-effort cleanup
            if let Some(_stream) = self.stream.take() {
                // Tokio TcpStream closes on drop
            }
        }
    }
}

// ════════════════════════════════════════════════════════════
// Delta-Sync Engine — Client-side block hashing and transfer
// ════════════════════════════════════════════════════════════

use crate::file_ops::FileOps;
use crate::hashing::{adler32, sha256_hex};
use crate::proto::{BlockSignature, DeltaSyncBlockData, DeltaSyncSignatureRequest};
use crate::ClientError;

/// Represents a block signature computed by the client
#[derive(Debug, Clone)]
pub struct ClientBlockSignature {
    pub block_index: u32,
    pub rolling_hash: u32,
    pub strong_hash: String,
    pub block_offset: u64,
    pub block_size: u32,
}

/// The delta-sync engine on the client side
pub struct DeltaSyncEngine {
    file_ops: FileOps,
}

impl DeltaSyncEngine {
    pub fn new(file_ops: FileOps) -> Self {
        Self { file_ops }
    }

    /// Compute block signatures for a local file.
    /// Reads the file in fixed-size blocks, computes Adler-32 and SHA-256
    /// for each block, and returns the complete signature table.
    pub fn compute_signatures(&self, relative_path: &str) -> crate::Result<Vec<ClientBlockSignature>> {
        let file_size = self.file_ops.file_size(relative_path)?;
        let block_size = self.file_ops.block_size();
        let total_blocks = ((file_size + block_size as u64 - 1) / block_size as u64) as u32;

        let mut signatures = Vec::with_capacity(total_blocks as usize);

        for block_index in 0..total_blocks {
            let block_offset = block_index as u64 * block_size as u64;
            let actual_size = if block_offset + block_size as u64 > file_size {
                (file_size - block_offset) as u32
            } else {
                block_size
            };

            let data = self.file_ops.read_block(relative_path, block_offset, actual_size)?;

            let rolling = adler32(&data);
            let strong = sha256_hex(&data);

            signatures.push(ClientBlockSignature {
                block_index,
                rolling_hash: rolling,
                strong_hash: strong,
                block_offset,
                block_size: actual_size,
            });
        }

        log::info!(
            "[DeltaSync] Computed {} block signatures for {} ({} bytes, block_size={})",
            signatures.len(),
            relative_path,
            file_size,
            block_size
        );

        Ok(signatures)
    }

    /// Convert client signatures to protobuf DeltaSyncSignatureRequest
    pub fn build_signature_request(
        &self,
        relative_path: &str,
        file_size: u64,
        block_size: u32,
        signatures: &[ClientBlockSignature],
    ) -> DeltaSyncSignatureRequest {
        let mut request = DeltaSyncSignatureRequest::default();
        request.relative_path = relative_path.to_string();
        request.file_size = file_size;
        request.block_size = block_size;

        for sig in signatures {
            let mut block_sig = BlockSignature::default();
            block_sig.block_index = sig.block_index;
            block_sig.rolling_hash = sig.rolling_hash;
            block_sig.strong_hash = sig.strong_hash.clone();
            block_sig.block_offset = sig.block_offset;
            block_sig.block_size = sig.block_size;
            request.signatures.push(block_sig);
        }

        request
    }

    /// Read specific blocks from the local file and create protobuf BlockData messages
    pub fn read_requested_blocks(
        &self,
        relative_path: &str,
        block_indices: &[u32],
    ) -> crate::Result<Vec<DeltaSyncBlockData>> {
        let file_size = self.file_ops.file_size(relative_path)?;
        let block_size = self.file_ops.block_size();

        let mut blocks = Vec::with_capacity(block_indices.len());

        for &block_index in block_indices {
            let block_offset = block_index as u64 * block_size as u64;
            if block_offset >= file_size {
                continue;  // Block index beyond file size
            }

            let actual_size = if block_offset + block_size as u64 > file_size {
                (file_size - block_offset) as u32
            } else {
                block_size
            };

            let data = self.file_ops.read_block(relative_path, block_offset, actual_size)?;
            let strong_hash = sha256_hex(&data);

            let mut block_data = DeltaSyncBlockData::default();
            block_data.relative_path = relative_path.to_string();
            block_data.block_index = block_index;
            block_data.block_offset = block_offset;
            block_data.data = data;
            block_data.strong_hash = strong_hash;

            blocks.push(block_data);
        }

        log::info!(
            "[DeltaSync] Read {} blocks for upload from {}",
            blocks.len(),
            relative_path
        );

        Ok(blocks)
    }

    /// Apply received blocks to reconstruct a file.
    /// For download delta-sync: write received blocks to a .tmp file
    /// at the correct offsets, then verify and finalize.
    pub fn apply_blocks(
        &self,
        relative_path: &str,
        total_size: u64,
        blocks: &[(u64, &[u8])],  // (offset, data) pairs
    ) -> crate::Result<String> {
        // Pre-allocate the .tmp file
        {
            let path = self.file_ops.resolve_path(relative_path);
            let tmp_path = path.with_extension("tmp");
            if let Some(parent) = path.parent() {
                std::fs::create_dir_all(parent)
                    .map_err(|e| ClientError::FileIo(format!("Cannot create dirs: {}", e)))?;
            }
            // Create/truncate the tmp file to the expected size
            let file = std::fs::File::create(&tmp_path)
                .map_err(|e| ClientError::FileIo(format!("Cannot create tmp: {}", e)))?;
            file.set_len(total_size)
                .map_err(|e| ClientError::FileIo(format!("Cannot pre-allocate: {}", e)))?;
        }

        // Write each block at its offset
        for &(offset, data) in blocks {
            self.file_ops.write_block(relative_path, offset, data)?;
        }

        // Verify the reconstructed file
        let final_hash = {
            let path = self.file_ops.resolve_path(relative_path);
            let tmp_path = path.with_extension("tmp");
            crate::hashing::sha256_file(&tmp_path)?
        };

        // Atomic rename
        self.file_ops.finalize_file(relative_path)?;

        log::info!(
            "[DeltaSync] File reconstructed: {} (SHA-256: {})",
            relative_path, final_hash
        );

        Ok(final_hash)
    }

    /// Get a reference to the underlying FileOps
    pub fn file_ops(&self) -> &FileOps {
        &self.file_ops
    }
}

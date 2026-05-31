// ════════════════════════════════════════════════════════════
// File Operations — Local disk I/O for the Rust client
// ════════════════════════════════════════════════════════════

use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;
use crate::ClientError;
use crate::hashing::sha256_hex;

/// Local file operations for the AdaptiveSync client
pub struct FileOps {
    sync_root: PathBuf,
    block_size: u32,
}

/// Metadata for a local file
#[derive(Debug, Clone)]
pub struct LocalFileInfo {
    pub relative_path: String,
    pub file_size: u64,
    pub last_modified_ns: u64,
    pub sha256_hash: String,
}

impl FileOps {
    pub fn new(sync_root: impl Into<PathBuf>, block_size: u32) -> Self {
        Self {
            sync_root: sync_root.into(),
            block_size,
        }
    }

    /// Get the absolute path for a relative path
    pub fn resolve_path(&self, relative_path: &str) -> PathBuf {
        self.sync_root.join(relative_path)
    }

    /// Check if a file exists
    pub fn file_exists(&self, relative_path: &str) -> bool {
        self.resolve_path(relative_path).exists()
    }

    /// Get file size in bytes
    pub fn file_size(&self, relative_path: &str) -> crate::Result<u64> {
        let path = self.resolve_path(relative_path);
        let metadata = std::fs::metadata(&path)
            .map_err(|e| ClientError::FileIo(format!("Cannot stat {}: {}", path.display(), e)))?;
        Ok(metadata.len())
    }

    /// Get last modified time in nanoseconds since epoch
    pub fn last_modified_ns(&self, relative_path: &str) -> crate::Result<u64> {
        let path = self.resolve_path(relative_path);
        let metadata = std::fs::metadata(&path)
            .map_err(|e| ClientError::FileIo(format!("Cannot stat {}: {}", path.display(), e)))?;
        let modified = metadata.modified()
            .map_err(|e| ClientError::FileIo(format!("Cannot get mtime: {}", e)))?;
        let duration = modified.duration_since(UNIX_EPOCH)
            .map_err(|e| ClientError::FileIo(format!("Time error: {}", e)))?;
        Ok(duration.as_nanos() as u64)
    }

    /// Compute SHA-256 hex digest of a file
    pub fn sha256(&self, relative_path: &str) -> crate::Result<String> {
        let path = self.resolve_path(relative_path);
        crate::hashing::sha256_file(&path)
    }

    /// Get complete file info
    pub fn get_file_info(&self, relative_path: &str) -> crate::Result<LocalFileInfo> {
        Ok(LocalFileInfo {
            relative_path: relative_path.to_string(),
            file_size: self.file_size(relative_path)?,
            last_modified_ns: self.last_modified_ns(relative_path)?,
            sha256_hash: self.sha256(relative_path)?,
        })
    }

    /// Read a single block from a file at the given offset
    pub fn read_block(&self, relative_path: &str, offset: u64, size: u32) -> crate::Result<Vec<u8>> {
        let path = self.resolve_path(relative_path);
        use std::io::{Read, Seek, SeekFrom};

        let mut file = std::fs::File::open(&path)
            .map_err(|e| ClientError::FileIo(format!("Cannot open {}: {}", path.display(), e)))?;

        file.seek(SeekFrom::Start(offset))
            .map_err(|e| ClientError::FileIo(format!("Seek error: {}", e)))?;

        let mut buffer = vec![0u8; size as usize];
        let bytes_read = file.read(&mut buffer)
            .map_err(|e| ClientError::FileIo(format!("Read error: {}", e)))?;
        buffer.truncate(bytes_read);
        Ok(buffer)
    }

    /// Write an entire file (for zero-copy downloads)
    pub fn write_file(&self, relative_path: &str, data: &[u8]) -> crate::Result<()> {
        let path = self.resolve_path(relative_path);

        // Ensure parent directories exist
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| ClientError::FileIo(format!("Cannot create dirs: {}", e)))?;
        }

        // Write to .tmp file first, then atomic rename
        let tmp_path = path.with_extension("tmp");
        std::fs::write(&tmp_path, data)
            .map_err(|e| ClientError::FileIo(format!("Write error: {}", e)))?;

        std::fs::rename(&tmp_path, &path)
            .map_err(|e| ClientError::FileIo(format!("Rename error: {}", e)))?;

        Ok(())
    }

    /// Write a block at a specific offset (for delta-sync reconstruction)
    pub fn write_block(&self, relative_path: &str, offset: u64, data: &[u8]) -> crate::Result<()> {
        let path = self.resolve_path(relative_path);
        let tmp_path = path.with_extension("tmp");

        use std::io::{Seek, SeekFrom, Write};

        // Create parent dirs
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| ClientError::FileIo(format!("Cannot create dirs: {}", e)))?;
        }

        // Open or create the tmp file
        let mut file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(&tmp_path)
            .map_err(|e| ClientError::FileIo(format!("Cannot open tmp: {}", e)))?;

        file.seek(SeekFrom::Start(offset))
            .map_err(|e| ClientError::FileIo(format!("Seek error: {}", e)))?;

        file.write_all(data)
            .map_err(|e| ClientError::FileIo(format!("Write error: {}", e)))?;

        file.flush()
            .map_err(|e| ClientError::FileIo(format!("Flush error: {}", e)))?;

        Ok(())
    }

    /// Finalize a file (atomic rename from .tmp to final path)
    pub fn finalize_file(&self, relative_path: &str) -> crate::Result<()> {
        let path = self.resolve_path(relative_path);
        let tmp_path = path.with_extension("tmp");

        if tmp_path.exists() {
            std::fs::rename(&tmp_path, &path)
                .map_err(|e| ClientError::FileIo(format!("Rename error: {}", e)))?;
        }
        Ok(())
    }

    /// List all files under the sync root
    pub fn list_files(&self, prefix: &str, recursive: bool) -> crate::Result<Vec<LocalFileInfo>> {
        let mut results = Vec::new();
        let root = &self.sync_root;

        if !root.exists() {
            return Ok(results);
        }

        let walk = if recursive {
            walkdir::WalkDir::new(root).into_iter()
        } else {
            // Only top-level: use a single directory iteration
            walkdir::WalkDir::new(root).max_depth(1).into_iter()
        };

        for entry in walk {
            let entry = entry.map_err(|e| ClientError::FileIo(format!("Walk error: {}", e)))?;
            if !entry.file_type().is_file() { continue; }

            let rel = entry.path().strip_prefix(root)
                .map_err(|e| ClientError::FileIo(format!("Path error: {}", e)))?;
            let rel_str = rel.to_string_lossy().to_string();

            // Skip .tmp files
            if rel_str.ends_with(".tmp") { continue; }

            // Apply prefix filter
            if !prefix.is_empty() && !rel_str.starts_with(prefix) { continue; }

            let metadata = entry.metadata()
                .map_err(|e| ClientError::FileIo(format!("Metadata error: {}", e)))?;

            // Compute SHA-256 only if file is small enough (defer for large files)
            let sha256 = if metadata.len() < 100 * 1024 * 1024 {
                sha256_hex(&std::fs::read(entry.path()).unwrap_or_default())
            } else {
                String::new()  // Defer expensive hash
            };

            let modified = metadata.modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0);

            results.push(LocalFileInfo {
                relative_path: rel_str,
                file_size: metadata.len(),
                last_modified_ns: modified,
                sha256_hash: sha256,
            });
        }

        Ok(results)
    }

    /// Get the configured block size
    pub fn block_size(&self) -> u32 {
        self.block_size
    }

    /// Get the sync root path
    pub fn sync_root(&self) -> &Path {
        &self.sync_root
    }
}

// ════════════════════════════════════════════════════════════
// Hashing — Adler-32 rolling checksum and SHA-256 strong hash
// ════════════════════════════════════════════════════════════

use sha2::{Sha256, Digest};

/// Compute Adler-32 rolling checksum for a byte slice.
/// Used as the fast/weak hash in the delta-sync rsync algorithm.
pub fn adler32(data: &[u8]) -> u32 {
    const MOD: u32 = 65521;
    let mut a: u32 = 1;
    let mut b: u32 = 0;

    for &byte in data {
        a = (a + byte as u32) % MOD;
        b = (b + a) % MOD;
    }

    (b << 16) | a
}

/// Compute SHA-256 hex digest for a byte slice.
/// Used as the strong/verified hash in the delta-sync algorithm
/// and for full-file integrity verification.
pub fn sha256_hex(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    let result = hasher.finalize();
    hex::encode(result)
}

/// Compute SHA-256 of a file, reading in chunks to minimize memory usage.
pub fn sha256_file(path: &std::path::Path) -> crate::Result<String> {
    use std::io::Read;
    let mut file = std::fs::File::open(path)
        .map_err(|e| crate::ClientError::FileIo(format!("Failed to open {}: {}", path.display(), e)))?;

    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 65536];

    loop {
        let bytes_read = file.read(&mut buffer)
            .map_err(|e| crate::ClientError::FileIo(format!("Read error: {}", e)))?;
        if bytes_read == 0 { break; }
        hasher.update(&buffer[..bytes_read]);
    }

    let result = hasher.finalize();
    Ok(hex::encode(result))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_adler32_empty() {
        assert_eq!(adler32(&[]), 1);  // a=1, b=0
    }

    #[test]
    fn test_adler32_hello() {
        // Known test vector
        let data = b"Hello";
        let result = adler32(data);
        assert_ne!(result, 0);
    }

    #[test]
    fn test_sha256_empty() {
        // SHA-256 of empty string is well-known
        let result = sha256_hex(&[]);
        assert_eq!(result, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    #[test]
    fn test_sha256_abc() {
        let result = sha256_hex(b"abc");
        assert_eq!(result, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

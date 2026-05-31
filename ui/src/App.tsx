import { useState, useCallback, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import ConnectionStatus from "./components/ConnectionStatus";
import FileList from "./components/FileList";
import FileUpload from "./components/FileUpload";
import TransferProgress from "./components/TransferProgress";

// ════════════════════════════════════════════════════════════
// Types
// ════════════════════════════════════════════════════════════

export interface ConnectionConfig {
  serverAddr: string;
  blockSize: number;
  syncRoot: string;
}

export interface FileEntry {
  relativePath: string;
  fileSize: number;
  lastModifiedNs: number;
  sha256Hash: string;
  fileMode: string;
  transferMode?: string;
}

export interface TransferJob {
  id: string;
  relativePath: string;
  transferMode: "zero_copy" | "delta_sync" | "unchanged";
  bytesTransferred: number;
  bytesTotal: number;
  rateMbps: number;
  elapsedMs: number;
  status: "pending" | "negotiating" | "transferring" | "complete" | "error";
  errorMessage?: string;
}

// ════════════════════════════════════════════════════════════
// App Component
// ════════════════════════════════════════════════════════════

function App() {
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [config, setConfig] = useState<ConnectionConfig>({
    serverAddr: "127.0.0.1:9090",
    blockSize: 65536,
    syncRoot: "/tmp/adaptivesync_sync",
  });
  const [files, setFiles] = useState<FileEntry[]>([]);
  const [transfers, setTransfers] = useState<TransferJob[]>([]);
  const [selectedFiles, setSelectedFiles] = useState<string[]>([]);
  const syncingRef = useRef(false);

  // ─── Connect to server ──────────────────────────────────
  const handleConnect = useCallback(async () => {
    setConnecting(true);
    try {
      await invoke("connect_to_server", {
        serverAddr: config.serverAddr,
        blockSize: config.blockSize,
        syncRoot: config.syncRoot,
      });
      setConnected(true);
      // Load file list
      const serverFiles = await invoke<FileEntry[]>("list_server_files", {
        prefix: "",
        recursive: true,
      });
      setFiles(serverFiles);
    } catch (err) {
      console.error("Connection failed:", err);
      setConnected(false);
    } finally {
      setConnecting(false);
    }
  }, [config]);

  // ─── Disconnect ─────────────────────────────────────────
  const handleDisconnect = useCallback(async () => {
    try {
      await invoke("disconnect_from_server");
    } catch (err) {
      console.error("Disconnect error:", err);
    }
    setConnected(false);
    setFiles([]);
    setTransfers([]);
  }, []);

  // ─── Sync files ─────────────────────────────────────────
  const handleSync = useCallback(async (paths: string[]) => {
    if (syncingRef.current) {
      console.warn("[Sync] Already syncing, ignoring concurrent request");
      return;
    }
    syncingRef.current = true;
    try {
      for (const path of paths) {
      const jobId = `job-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
      const newJob: TransferJob = {
        id: jobId,
        relativePath: path,
        transferMode: "zero_copy",
        bytesTransferred: 0,
        bytesTotal: 0,
        rateMbps: 0,
        elapsedMs: 0,
        status: "negotiating",
      };

      setTransfers(prev => [...prev, newJob]);

      try {
        // Negotiate transfer mode
        const result = await invoke<{
          fileMode: string;
          transferMode: string;
          serverFileSize: number;
          serverSha256: string;
          blockSize: number;
        }>("negotiate_file", { relativePath: path });

        const mode = result.transferMode === "ZERO_COPY" ? "zero_copy"
                   : result.transferMode === "DELTA_SYNC" ? "delta_sync"
                   : "unchanged";

        setTransfers(prev => prev.map(j =>
          j.id === jobId ? { ...j, transferMode: mode, status: "transferring" } : j
        ));

        if (mode === "unchanged") {
          setTransfers(prev => prev.map(j =>
            j.id === jobId ? { ...j, status: "complete" } : j
          ));
          continue;
        }

        // Execute transfer
        await invoke("sync_file", {
          relativePath: path,
          fileMode: result.fileMode,
          transferMode: result.transferMode,
        });

        setTransfers(prev => prev.map(j =>
          j.id === jobId ? {
            ...j,
            status: "complete",
            bytesTransferred: j.bytesTotal,
          } : j
        ));
      } catch (err) {
        setTransfers(prev => prev.map(j =>
          j.id === jobId ? {
            ...j,
            status: "error",
            errorMessage: String(err),
          } : j
        ));
      }
    }

    // Refresh file list
    try {
      const serverFiles = await invoke<FileEntry[]>("list_server_files", {
        prefix: "",
        recursive: true,
      });
      setFiles(serverFiles);
    } catch (err) {
      console.error("Failed to refresh file list:", err);
    }
  } finally {
      syncingRef.current = false;
    }
  }, []);

  // ─── Download file ──────────────────────────────────────
  const handleDownload = useCallback(async (relativePath: string) => {
    try {
      const result = await invoke<{
        success: boolean;
        relativePath: string;
        sha256: string;
        error?: string;
      }>("download_file", { relativePath });
      if (result.success) {
        const newJob: TransferJob = {
          id: `dl-${Date.now()}`,
          relativePath: result.relativePath,
          transferMode: "zero_copy",
          bytesTransferred: 0,
          bytesTotal: 0,
          rateMbps: 0,
          elapsedMs: 0,
          status: "complete",
        };
        setTransfers(prev => [...prev, newJob]);
      } else {
        console.error("Download failed:", result.error);
      }
    } catch (err) {
      console.error("Download error:", err);
    }
    // Refresh file list
    try {
      const serverFiles = await invoke<FileEntry[]>("list_server_files", {
        prefix: "",
        recursive: true,
      });
      setFiles(serverFiles);
    } catch (err) {
      console.error("Failed to refresh file list:", err);
    }
  }, []);

  // ─── File dialog ────────────────────────────────────────
  const handleOpenFileDialog = useCallback(async () => {
    if (!connected) return;
    try {
      const selected = await openDialog({
        multiple: true,
        filters: [{ name: "All Files", extensions: ["*"] }],
      });
      if (!selected) return;
      const paths = Array.isArray(selected) ? selected : [selected];
      // Copy files to sync root
      const relPaths: string[] = [];
      for (const absPath of paths) {
        const relPathsArray = await invoke<string[]>("upload_local_file", {
          absolutePath: absPath,
        });
        relPaths.push(...relPathsArray);
      }
      await handleSync(relPaths);
    } catch (err) {
      console.error("File dialog error:", err);
    }
  }, [connected, handleSync]);

  // ─── Folder dialog ──────────────────────────────────────
  const handleOpenFolderDialog = useCallback(async () => {
    if (!connected) return;
    try {
      const selected = await openDialog({
        directory: true,
      });
      if (!selected) return;
      const folderPath = Array.isArray(selected) ? selected[0] : selected;
      const relPaths = await invoke<string[]>("upload_local_file", {
        absolutePath: folderPath,
      });
      await handleSync(relPaths);
    } catch (err) {
      console.error("Folder dialog error:", err);
    }
  }, [connected, handleSync]);

  // ─── File drag-and-drop ─────────────────────────────────
  const handleFilesDropped = useCallback(async (filePaths: string[]) => {
    if (!connected) return;
    const relPaths: string[] = [];
    for (const fp of filePaths) {
      // If it looks like an absolute path, copy to sync root first
      if (fp.startsWith("/") || fp.startsWith("\\") || fp.match(/^[A-Za-z]:\\/)) {
        try {
          const relPathsArray = await invoke<string[]>("upload_local_file", {
            absolutePath: fp,
          });
          relPaths.push(...relPathsArray);
        } catch (err) {
          console.error("Failed to copy file:", fp, err);
        }
      } else {
        relPaths.push(fp);
      }
    }
    await handleSync(relPaths);
  }, [connected, handleSync]);

  // ─── Render ─────────────────────────────────────────────
  return (
    <div className="app-layout">
      {/* Top Connection Bar */}
      <ConnectionStatus
        connected={connected}
        connecting={connecting}
        config={config}
        onConfigChange={setConfig}
        onConnect={handleConnect}
        onDisconnect={handleDisconnect}
      />

      {/* Main Content */}
      <div className="main-content">
        {/* Sidebar: File List */}
        <div className="sidebar">
          <h3 style={{ marginBottom: 12, fontSize: 14, color: "var(--text-secondary)" }}>
            Server Files
          </h3>
          <FileList
            files={files}
            selectedFiles={selectedFiles}
            onSelectionChange={setSelectedFiles}
            onSync={handleSync}
            onDownload={handleDownload}
            connected={connected}
          />
        </div>

        {/* Content Area */}
        <div className="content-area">
          {/* Drop Zone */}
          <FileUpload
            onFilesDropped={handleFilesDropped}
            onBrowse={handleOpenFileDialog}
            onBrowseFolder={handleOpenFolderDialog}
            connected={connected}
          />

          {/* Active Transfers */}
          <div style={{ marginTop: 24 }}>
            <h3 style={{ marginBottom: 12, fontSize: 14, color: "var(--text-secondary)" }}>
              Transfer Queue
            </h3>
            <TransferProgress transfers={transfers} />
          </div>
        </div>
      </div>

      {/* Footer Status Bar */}
      <div className="connection-bar" style={{ justifyContent: "space-between" }}>
        <span style={{ color: "var(--text-secondary)", fontSize: 12 }}>
          AdaptiveSync v1.0 — C++ Server + Rust Client + Tauri
        </span>
        <span style={{ color: "var(--text-secondary)", fontSize: 12 }}>
          Block Size: {config.blockSize.toLocaleString()} bytes
          {transfers.length > 0 && ` | Active: ${transfers.filter(t => t.status === "transferring").length}`}
        </span>
      </div>
    </div>
  );
}

export default App;

// ════════════════════════════════════════════════════════════
// FileUpload — Drag-and-drop zone for file uploads
// ════════════════════════════════════════════════════════════

import { useState, useCallback } from "react";

interface Props {
  onFilesDropped: (paths: string[]) => void;
  onBrowse: () => void;
  onBrowseFolder: () => void;
  connected: boolean;
}

function FileUpload({ onFilesDropped, onBrowse, onBrowseFolder, connected }: Props) {
  const [isDragging, setIsDragging] = useState(false);
  const [manualPath, setManualPath] = useState("");

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    if (connected) setIsDragging(true);
  }, [connected]);

  const handleDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    setIsDragging(false);
  }, []);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    setIsDragging(false);

    if (!connected) return;

    const paths: string[] = [];
    if (e.dataTransfer.files) {
      for (let i = 0; i < e.dataTransfer.files.length; i++) {
        // In Tauri, we can get file paths from the drop event
        const file = e.dataTransfer.files[i];
        // Note: In a real Tauri app, we'd use the file path via Tauri's FS API
        // For now, we'll use the file name
        paths.push(file.name);
      }
    }
    if (paths.length > 0) {
      onFilesDropped(paths);
    }
  }, [connected, onFilesDropped]);

  const handleManualAdd = useCallback(() => {
    if (manualPath.trim() && connected) {
      onFilesDropped([manualPath.trim()]);
      setManualPath("");
    }
  }, [manualPath, connected, onFilesDropped]);

  return (
    <div>
      {/* Drag-and-Drop Zone */}
      <div
        className={`drop-zone ${isDragging ? "active" : ""}`}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
        style={{ opacity: connected ? 1 : 0.5 }}
      >
        <div style={{ fontSize: 32, marginBottom: 12, color: "var(--text-secondary)" }}>
          {isDragging ? "⬇" : "📁"}
        </div>
        <div style={{ fontSize: 14, fontWeight: 500, marginBottom: 4 }}>
          {isDragging ? "Drop files here" : "Drag & drop files to sync"}
        </div>
        <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
          Files will be analyzed and synced using zero-copy or delta-sync
        </div>
      </div>

      {/* Browse Buttons */}
      <div style={{ marginTop: 12, display: "flex", gap: 8 }}>
        <button
          className="btn btn-primary"
          onClick={onBrowse}
          disabled={!connected}
          style={{ flex: 1 }}
        >
          Browse Files...
        </button>
        <button
          className="btn"
          onClick={onBrowseFolder}
          disabled={!connected}
          style={{ flex: 1 }}
        >
          Browse Folder...
        </button>
      </div>

      {/* Manual Path Input */}
      <div style={{ display: "flex", gap: 8, marginTop: 12 }}>
        <input
          className="input"
          style={{ flex: 1 }}
          value={manualPath}
          onChange={(e) => setManualPath(e.target.value)}
          placeholder="Enter file path to sync..."
          disabled={!connected}
          onKeyDown={(e) => e.key === "Enter" && handleManualAdd()}
        />
        <button
          className="btn btn-primary"
          onClick={handleManualAdd}
          disabled={!connected || !manualPath.trim()}
        >
          Sync
        </button>
      </div>
    </div>
  );
}

export default FileUpload;

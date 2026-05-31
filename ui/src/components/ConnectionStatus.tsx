import { ConnectionConfig } from "../App";

// ════════════════════════════════════════════════════════════
// ConnectionStatus — Top bar for server connection management
// ════════════════════════════════════════════════════════════

interface Props {
  connected: boolean;
  connecting: boolean;
  config: ConnectionConfig;
  onConfigChange: (config: ConnectionConfig) => void;
  onConnect: () => void;
  onDisconnect: () => void;
}

function ConnectionStatus({
  connected,
  connecting,
  config,
  onConfigChange,
  onConnect,
  onDisconnect,
}: Props) {
  return (
    <div className="connection-bar">
      {/* Status Dot */}
      <span
        className={`status-dot ${connected ? "connected" : connecting ? "syncing" : "disconnected"}`}
      />

      {/* Server Address */}
      <input
        className="input"
        style={{ width: 200 }}
        value={config.serverAddr}
        onChange={(e) => onConfigChange({ ...config, serverAddr: e.target.value })}
        placeholder="Server address (host:port)"
        disabled={connected || connecting}
      />

      {/* Sync Root */}
      <input
        className="input"
        style={{ width: 160 }}
        value={config.syncRoot}
        onChange={(e) => onConfigChange({ ...config, syncRoot: e.target.value })}
        placeholder="Local sync directory"
        disabled={connected || connecting}
      />

      {/* Block Size */}
      <select
        className="input"
        value={config.blockSize}
        onChange={(e) => onConfigChange({ ...config, blockSize: Number(e.target.value) })}
        disabled={connected || connecting}
      >
        <option value={4096}>4 KB blocks</option>
        <option value={16384}>16 KB blocks</option>
        <option value={65536}>64 KB blocks</option>
        <option value={262144}>256 KB blocks</option>
        <option value={1048576}>1 MB blocks</option>
      </select>

      {/* Connect/Disconnect Button */}
      {connected ? (
        <button className="btn btn-danger" onClick={onDisconnect}>
          Disconnect
        </button>
      ) : (
        <button className="btn btn-primary" onClick={onConnect} disabled={connecting}>
          {connecting ? "Connecting..." : "Connect"}
        </button>
      )}

      {/* Status Text */}
      <span style={{ color: "var(--text-secondary)", fontSize: 12, marginLeft: "auto" }}>
        {connected
          ? `Connected to ${config.serverAddr}`
          : connecting
          ? "Establishing connection..."
          : "Disconnected"}
      </span>
    </div>
  );
}

export default ConnectionStatus;

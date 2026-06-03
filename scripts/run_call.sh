#!/usr/bin/env bash
# run_call.sh — launch the SFU server or a group_call client.
#
# Usage:
#   scripts/run_call.sh server [port]                # run the hub (default 8080)
#   scripts/run_call.sh client <server-ip> [room] [port]
#
# Example (two laptops on the same Wi-Fi):
#   Laptop A:  scripts/run_call.sh server
#              scripts/run_call.sh client <A-ip> demo
#   Laptop B:  scripts/run_call.sh client <A-ip> demo
#
# Assumes the project is already built (see the README for the build command).
# Grant the terminal Camera AND Microphone permission, and use headphones.

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
SERVER_BIN="$BUILD_DIR/sfu-server/sfu_server"
CLIENT_BIN="$BUILD_DIR/src/ui/group_call"

lan_ip() { ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo "<your-LAN-ip>"; }

mode="${1:-}"
case "$mode" in
  server)
    port="${2:-8080}"
    [ -x "$SERVER_BIN" ] || { echo "Not built: $SERVER_BIN" >&2; exit 1; }
    echo "Starting SFU server on port $port."
    echo "Clients on this network should connect to:  $(lan_ip):$port"
    exec "$SERVER_BIN" "$port"
    ;;
  client)
    host="${2:-}"
    room="${3:-demo}"
    port="${4:-8080}"
    [ -n "$host" ] || { echo "Usage: $0 client <server-ip> [room] [port]" >&2; exit 1; }
    [ -x "$CLIENT_BIN" ] || { echo "Not built: $CLIENT_BIN" >&2; exit 1; }
    echo "Joining room \"$room\" at $host:$port"
    exec "$CLIENT_BIN" --server "$host" --port "$port" --room "$room"
    ;;
  *)
    echo "Usage:" >&2
    echo "  $0 server [port]" >&2
    echo "  $0 client <server-ip> [room] [port]" >&2
    exit 1
    ;;
esac

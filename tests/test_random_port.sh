#!/usr/bin/env bash
# Verify that the supervisor appends `--port 0` and discovers the OS-assigned port.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$ROOT/tests/.tmp"
LOG="$TMP/random-port.log"
PORT_FILE="$TMP/random-dsh-port"
PROXY_PORT="${RANDOM_PROXY_PORT:-18081}"
PROXY_PID=""

mkdir -p "$TMP"
rm -f "$LOG" "$PORT_FILE"
chmod +x "$ROOT/tests/mock_dsh.py"

cleanup() {
	local status=$?
	set +e
	if [[ -n "$PROXY_PID" ]]; then
		kill -TERM "$PROXY_PID" 2>/dev/null
		wait "$PROXY_PID" 2>/dev/null
	fi
	pkill -TERM -f "^python3 .*mock_dsh.py --port-file $PORT_FILE" 2>/dev/null
	if [[ $status -ne 0 ]]; then
		echo "---- proxy log ----" >&2
		tail -100 "$LOG" >&2 || true
	fi
	exit "$status"
}
trap cleanup EXIT

"$ROOT/dsh-proxy" -l "127.0.0.1:$PROXY_PORT" \
	-d "python3 $ROOT/tests/mock_dsh.py --port-file $PORT_FILE" \
	-t 30 -v >"$LOG" 2>&1 &
PROXY_PID=$!

STATUS=""
for _ in $(seq 1 150); do
	STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
		"http://127.0.0.1:$PROXY_PORT/" 2>/dev/null || true)
	[[ "$STATUS" == "200" && -s "$PORT_FILE" ]] && break
	sleep 0.2
done
if [[ "$STATUS" != "200" || ! -s "$PORT_FILE" ]]; then
	echo "random-port proxy did not become ready (status=$STATUS)" >&2
	exit 1
fi

MOCK_PORT="$(cat "$PORT_FILE")"
if ! [[ "$MOCK_PORT" =~ ^[0-9]+$ ]] || [[ "$MOCK_PORT" -le 1024 ]] ||
	[[ "$MOCK_PORT" -gt 65535 ]]; then
	echo "unexpected dsh port: $MOCK_PORT" >&2
	exit 1
fi

grep -q -- "--port 0" "$LOG"
grep -q "upstream=127.0.0.1:$MOCK_PORT" "$LOG"
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$MOCK_PORT/")" == "401" ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PROXY_PORT/")" == "200" ]]

echo "random port test passed (dsh picked 127.0.0.1:$MOCK_PORT)"

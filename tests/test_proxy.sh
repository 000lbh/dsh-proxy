#!/usr/bin/env bash
# End-to-end tests for dsh-proxy against tests/mock_dsh.py.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$ROOT/tests/.tmp"
PROXY_LOG="$TMP/proxy.log"
ENV_FILE="$TMP/dsh-env.json"
MOCK_PORT="${MOCK_PORT:-18099}"
PROXY_PORT="${PROXY_PORT:-18080}"
PROXY_PID=""

mkdir -p "$TMP"
rm -f "$PROXY_LOG" "$ENV_FILE"
chmod +x "$ROOT/tests/mock_dsh.py" "$ROOT/tests/ws_check.py"

cleanup() {
	local status=$?
	set +e
	if [[ -n "$PROXY_PID" ]]; then
		kill -TERM "$PROXY_PID" 2>/dev/null
		wait "$PROXY_PID" 2>/dev/null
	fi
	pkill -TERM -f "^python3 .*mock_dsh.py --port $MOCK_PORT" 2>/dev/null
	if [[ $status -ne 0 ]]; then
		echo "---- proxy log ----" >&2
		tail -100 "$PROXY_LOG" >&2 || true
	fi
	exit "$status"
}
trap cleanup EXIT

DISPLAY=:99 \
WAYLAND_DISPLAY=wayland-fake \
XDG_RUNTIME_DIR="$TMP/fake-runtime" \
XDG_SESSION_TYPE=wayland \
XDG_CURRENT_DESKTOP=FAKE \
KDE_FULL_SESSION=true \
DBUS_SESSION_BUS_ADDRESS="unix:path=$TMP/fake-bus" \
DSH_HOME="$TMP/inherited-dsh-home" \
"$ROOT/dsh-proxy" -l "127.0.0.1:$PROXY_PORT" \
	-d "python3 $ROOT/tests/mock_dsh.py --port $MOCK_PORT --prompt --delay-token 3 --public-url --expire-once --env-file $ENV_FILE" \
	-u "127.0.0.1:$MOCK_PORT" \
	-t 30 -v >"$PROXY_LOG" 2>&1 &
PROXY_PID=$!

EARLY_STATUS=""
for _ in $(seq 1 25); do
	EARLY_STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
		"http://127.0.0.1:$PROXY_PORT/" 2>/dev/null || true)
	[[ "$EARLY_STATUS" == "503" ]] && break
	sleep 0.1
done
if [[ "$EARLY_STATUS" != "503" ]]; then
	echo "expected 503 before the token is available, got $EARLY_STATUS" >&2
	exit 1
fi

wait_ready() {
	local i code
	for i in $(seq 1 150); do
		code=$(curl -s -o /dev/null -w '%{http_code}' \
			"http://127.0.0.1:$PROXY_PORT/" 2>/dev/null || true)
		if [[ "$code" == "200" ]]; then
			return 0
		fi
		sleep 0.2
	done
	echo "proxy did not become ready (last status: ${code:-none})" >&2
	return 1
}

wait_ready

echo "== auto answer and delayed token =="
grep -q "auto-answered a dsh install/update prompt" "$PROXY_LOG"
grep -q "mock: prompt answered with y" "$PROXY_LOG"
grep -q "upstream=127.0.0.1:$MOCK_PORT" "$PROXY_LOG"
grep -q "dsh rejected the injected session cookie" "$PROXY_LOG"
echo "ok"

echo "== dsh environment is scrubbed =="
python3 - "$ENV_FILE" "$TMP/inherited-dsh-home" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as env_file:
    env = json.load(env_file)
for key in (
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XDG_RUNTIME_DIR",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "KDE_FULL_SESSION",
    "DBUS_SESSION_BUS_ADDRESS",
):
    assert key not in env, f"{key} leaked into dsh: {env.get(key)!r}"
assert env.get("HOME"), "HOME should be preserved"
assert env.get("PATH"), "PATH should be preserved"
assert env.get("DSH_HOME") == sys.argv[2], env.get("DSH_HOME")
PY
echo "ok"

echo "== direct dsh requires auth =="
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$MOCK_PORT/")" == "401" ]]
echo "ok"

echo "== proxy injects cookie and serves index =="
BODY=$(curl -sS "http://127.0.0.1:$PROXY_PORT/")
grep -q "mock dsh" <<<"$BODY"
echo "ok"

echo "== header, cookie and query rewriting =="
ECHO=$(curl -sS \
	-H "Host: 192.0.2.10:$PROXY_PORT" \
	-H "Origin: http://192.0.2.10:$PROXY_PORT" \
	-H "Cookie: junk=1" \
	-H "Sec-Fetch-Site: cross-site" \
	"http://127.0.0.1:$PROXY_PORT/api/echo?token=stale&a=1")
python3 - "$ECHO" "$MOCK_PORT" "$PROXY_PORT" <<'PY'
import json
import sys

data = json.loads(sys.argv[1])
mock_port = sys.argv[2]
proxy_port = sys.argv[3]
headers = data["headers"]
assert data["target"] == "/api/echo?a=1", data
assert data["query"] == "a=1", data
assert headers["host"] == f"127.0.0.1:{mock_port}", headers
assert headers["cookie"] == "dsh-auth-mock=testsecret", headers
assert headers["origin"] == f"http://127.0.0.1:{mock_port}", headers
assert headers["sec-fetch-site"] == "same-origin", headers
assert headers["x-forwarded-host"] == f"192.0.2.10:{proxy_port}", headers
assert headers["x-forwarded-proto"] == "http", headers
assert headers["x-forwarded-for"].startswith("127.0.0.1"), headers
PY
echo "ok"

echo "== content-length request body =="
PAYLOAD="hello body 123"
SHA=$(printf '%s' "$PAYLOAD" | sha256sum | cut -d' ' -f1)
BODY_JSON=$(curl -sS -X POST --data-binary "$PAYLOAD" \
	"http://127.0.0.1:$PROXY_PORT/api/echo-body")
python3 - "$BODY_JSON" "$PAYLOAD" "$SHA" <<'PY'
import json
import sys

data = json.loads(sys.argv[1])
assert data["length"] == len(sys.argv[2].encode()), data
assert data["sha256"] == sys.argv[3], data
assert data["text"] == sys.argv[2], data
PY
echo "ok"

echo "== chunked request body =="
BODY_JSON=$(curl -sS -X POST -H "Transfer-Encoding: chunked" \
	--data-binary "$PAYLOAD" \
	"http://127.0.0.1:$PROXY_PORT/api/echo-body")
python3 - "$BODY_JSON" "$PAYLOAD" "$SHA" <<'PY'
import json
import sys

data = json.loads(sys.argv[1])
assert data["length"] == len(sys.argv[2].encode()), data
assert data["sha256"] == sys.argv[3], data
PY
echo "ok"

echo "== expect: 100-continue =="
BODY_JSON=$(curl -sS -X POST -H "Expect: 100-continue" \
	--data-binary "$PAYLOAD" \
	"http://127.0.0.1:$PROXY_PORT/api/echo-body")
python3 - "$BODY_JSON" "$PAYLOAD" <<'PY'
import json
import sys

data = json.loads(sys.argv[1])
assert data["length"] == len(sys.argv[2].encode()), data
PY
echo "ok"

echo "== location rewrite =="
LOCATION=$(curl -sS -D - -o /dev/null \
	-H "Host: 192.0.2.10:$PROXY_PORT" \
	"http://127.0.0.1:$PROXY_PORT/redirect" |
	tr -d '\r' | awk -F': ' 'tolower($1)=="location" {print $2}')
[[ "$LOCATION" == "http://192.0.2.10:$PROXY_PORT/destination" ]]
echo "ok ($LOCATION)"

echo "== server-sent events =="
SSE=$(curl -sS --max-time 5 "http://127.0.0.1:$PROXY_PORT/sse")
[[ "$(grep -c '^data: tick-' <<<"$SSE")" == "3" ]]
echo "ok"

echo "== websocket tunnel =="
python3 "$ROOT/tests/ws_check.py" 127.0.0.1 "$PROXY_PORT" /api/ws
echo "ok"

echo "== dsh restart and token re-acquisition =="
MOCK_PID=$(pgrep -f "^python3 .*mock_dsh.py --port $MOCK_PORT" | head -1)
[[ -n "$MOCK_PID" ]]
kill -TERM "$MOCK_PID"
wait_ready
[[ "$(grep -c 'auto-answered a dsh install/update prompt' "$PROXY_LOG")" -ge 2 ]]
grep -q "restarting dsh" "$PROXY_LOG"
echo "ok"

echo "ALL TESTS PASSED"

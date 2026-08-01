#!/bin/sh
# Runs the two-process half of the virtual network tests.
#
# One process hosts a lobby and serves whatever the other asks of it; the other
# joins and asserts that unicast, broadcast, streams, echo and reconnecting all
# work across the link. Neither can prove anything on its own, which is why this
# is not an ordinary test.
#
# Usage: run-peer-test.sh [path to tests.exe] [port]

set -e

TESTS=${1:-"$(dirname "$0")/../../../../build/release/x64/Binaries/Tests/tests.exe"}
PORT=${2:-38921}

if [ ! -x "$TESTS" ]; then
  echo "not found: $TESTS" >&2
  echo "build it with: build.ps1 -Target tests" >&2
  exit 1
fi

TESTS=$(cd "$(dirname "$TESTS")" && pwd)/$(basename "$TESTS")

# Both halves write their trace to VirtualNet.log in the working directory, so
# the host gets its own directory. Otherwise they interleave into one file and
# neither is readable - which matters, because the trace is the first thing
# anyone looks at when this fails.
HOST_DIR=$(mktemp -d)

echo "==> host on port $PORT (trace in $HOST_DIR)"
(cd "$HOST_DIR" && VNET_PEER_ROLE=host VNET_PEER_PORT="$PORT" \
  "$TESTS" --gtest_filter='VirtualNetPeer.Host' >host.log 2>&1) &
HOST_PID=$!

# The host has to be listening before the client dials, or the client spends its
# first attempt on a closed port and waits out the backoff.
sleep 2

echo "==> client"
set +e
VNET_PEER_ROLE=client VNET_PEER_PORT="$PORT" \
  "$TESTS" --gtest_filter='VirtualNetPeer.Client' 2>&1
CLIENT_STATUS=$?
set -e

kill "$HOST_PID" 2>/dev/null || true
wait "$HOST_PID" 2>/dev/null || true

if [ "$CLIENT_STATUS" -ne 0 ]; then
  echo "==> host log:"
  cat "$HOST_DIR/host.log"
  echo "==> host trace:"
  cat "$HOST_DIR/VirtualNet.log"
fi
rm -rf "$HOST_DIR" 2>/dev/null || true

exit "$CLIENT_STATUS"

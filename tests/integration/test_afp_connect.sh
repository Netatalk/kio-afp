#!/usr/bin/env bash
set -euo pipefail

echo "Running afp_connect integration test"

AFP_SERVER=localhost
AFP_SHARE=${1:-afp1}

# Try to mount
MOUNT_PATH=$(afp_connect --server "${AFP_SERVER}" --share "${AFP_SHARE}" 2>/tmp/afp_connect.err | tr -d '\r' | tr -d '\n' || true)
RET=$?
if [ -z "$MOUNT_PATH" ]; then
    echo "afp_connect failed or returned no mount path. stderr:" >&2
    sed -n '1,200p' /tmp/afp_connect.err >&2 || true
    exit 2
fi

echo "Mounted at: $MOUNT_PATH"

# Check mountpoint exists and is accessible
if [ ! -d "$MOUNT_PATH" ]; then
    echo "Mount path does not exist: $MOUNT_PATH" >&2
    exit 3
fi

if ! ls "$MOUNT_PATH" >/dev/null 2>&1; then
    echo "Mount path not readable: $MOUNT_PATH" >&2
    exit 4
fi

# Use kioclient to stat the AFP URL
if ! kioclient stat afp://${AFP_SERVER}/${AFP_SHARE} >/dev/null 2>&1; then
    echo "kioclient stat failed" >&2
    fusermount -u "$MOUNT_PATH" || true
    exit 5
fi

# List via kioclient
if ! kioclient ls afp://${AFP_SERVER}/${AFP_SHARE} >/dev/null 2>&1; then
    echo "kioclient ls failed" >&2
    fusermount -u "$MOUNT_PATH" || true
    exit 6
fi

# If all checks passed, unmount and exit success
echo "Unmounting $MOUNT_PATH"
fusermount -u "$MOUNT_PATH" || true

echo "afp_connect integration test PASSED"
exit 0

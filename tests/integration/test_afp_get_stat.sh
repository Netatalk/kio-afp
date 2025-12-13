#!/usr/bin/env bash
set -euo pipefail

echo "Running afp_get_stat integration test"

AFP_SERVER=localhost
AFP_SHARE=${1:-afp1}
TESTFILE="ci_testfile.txt"
CONTENT="Integration test $(date +%s%N)"
TMPDIR=$(mktemp -d)
TMPLOCAL="$TMPDIR/$TESTFILE"

MOUNT_PATH=$(afp_connect --server "${AFP_SERVER}" --share "${AFP_SHARE}" 2>/tmp/afp_connect.err | tr -d '\r' | tr -d '\n' || true)
if [ -z "$MOUNT_PATH" ]; then
    echo "afp_connect failed or returned no mount path. stderr:" >&2
    sed -n '1,200p' /tmp/afp_connect.err >&2 || true
    exit 2
fi

echo "Mounted at: $MOUNT_PATH"

if [ ! -d "$MOUNT_PATH" ]; then
    echo "Mount path does not exist: $MOUNT_PATH" >&2
    exit 3
fi

trap 'rc=$?; if [ "${CREATED_TESTFILE:-0}" -eq 1 ]; then rm -f "$MOUNT_PATH/$TESTFILE" || true; fi; fusermount -u "$MOUNT_PATH" || true; rm -rf "$TMPDIR"; exit $rc' EXIT

if echo -n "$CONTENT" > "$MOUNT_PATH/$TESTFILE" 2>/dev/null; then
    sync
    CREATED_TESTFILE=1
else
    CREATED_TESTFILE=0
    # If we couldn't create a file, try to find an existing one to test against
    # Prefer a non-empty regular file
    EXISTING=$(find "$MOUNT_PATH" -maxdepth 1 -type f -size +0c -printf '%f\n' | head -n 1 || true)
    if [ -z "$EXISTING" ]; then
        echo "Mount appears read-only and empty; skipping get/stat content checks" >&2
        # 77 is commonly used to indicate skipped tests
        fusermount -u "$MOUNT_PATH" || true
        exit 77
    fi
    TESTFILE="$EXISTING"
    echo "Using existing file for test: $TESTFILE"
fi

if ! kioclient stat "afp://${AFP_SERVER}/${AFP_SHARE}/${TESTFILE}" >/dev/null 2>&1; then
    echo "kioclient stat failed" >&2
    exit 4
fi

# Try kioclient get, fallback to copy
if kioclient get "afp://${AFP_SERVER}/${AFP_SHARE}/${TESTFILE}" "$TMPLOCAL" >/dev/null 2>&1; then
    :
elif kioclient copy "afp://${AFP_SERVER}/${AFP_SHARE}/${TESTFILE}" "file://$TMPLOCAL" >/dev/null 2>&1; then
    :
else
    echo "kioclient get/copy failed" >&2
    exit 5
fi

if [ "$CREATED_TESTFILE" -eq 1 ]; then
    if ! cmp -s "$TMPLOCAL" <(printf "%s" "$CONTENT"); then
        echo "Content mismatch" >&2
        exit 6
    fi

    expected_len=${#CONTENT}
    actual_len=$(stat -c%s "$TMPLOCAL")
    if [ "$expected_len" -ne "$actual_len" ]; then
        echo "Size mismatch: expected=$expected_len actual=$actual_len" >&2
        exit 7
    fi
else
    # For existing remote files we can't predict content; just ensure we retrieved a non-empty file
    actual_len=$(stat -c%s "$TMPLOCAL")
    if [ "$actual_len" -eq 0 ]; then
        echo "Downloaded file is empty" >&2
        exit 8
    fi
fi

echo "Cleaning up test file and unmounting"
if [ "${CREATED_TESTFILE:-0}" -eq 1 ]; then
    rm -f "$MOUNT_PATH/$TESTFILE" || true
fi
fusermount -u "$MOUNT_PATH" || true
rm -rf "$TMPDIR"

echo "afp_get_stat integration test PASSED"
exit 0

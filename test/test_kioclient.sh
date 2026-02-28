#!/bin/bash
# CLI integration tests for kio-afp using kioclient
# Copyright (c) 2026 Daniel Markstedt <daniel@mindani.net>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.

set -uo pipefail

VERBOSE=0
while getopts "v" opt; do
    case $opt in
        v) VERBOSE=1 ;;
        *) echo "Usage: $0 [-v]" >&2; exit 1 ;;
    esac
done

[[ $VERBOSE -eq 1 ]] && export QT_LOGGING_RULES="kio.*=true"

AFP_USER="${AFP_USER:-testuser}"
AFP_PASS="${AFP_PASS:-testpass}"
AFP_VOLUME="${AFP_VOLUME:-kio-afp-test}"
AFP_HOST="${AFP_HOST:-localhost}"
AFP_SERVER="afp://${AFP_USER}:${AFP_PASS}@${AFP_HOST}"
AFP_URL="${AFP_SERVER}/${AFP_VOLUME}"

PASS=0
FAIL=0

ok() { echo "PASS: $1"; ((PASS++)); }
fail() { echo "FAIL: $1"; ((FAIL++)); }

run() {
    local desc="$1"; shift
    local rc=0
    if [[ $VERBOSE -eq 1 ]]; then
        "$@" || rc=$?
    else
        "$@" >/dev/null || rc=$?
    fi
    if [[ $rc -eq 0 ]]; then
        ok "$desc"
    else
        fail "$desc"
    fi
}

TEST_CONTENT_LONG="kio-afp-test-$(printf '%04x%04x%04x%04x' $RANDOM $RANDOM $RANDOM $RANDOM)"
TEST_CONTENT_SHORT="short-$(printf '%04x%04x' $RANDOM $RANDOM)"

DOWNLOAD=$(mktemp)
echo "== SETUP. Cleanup leftover test files"
trap 'rm -f "$DOWNLOAD" /tmp/afp_put_test.txt /tmp/afp_small.txt /tmp/afp_verify.txt' EXIT
kioclient remove "$AFP_URL/afp_put_test.txt" 2>/dev/null || true

echo "== TEST 1. Upload a new file (put - create)"
echo "$TEST_CONTENT_LONG" > /tmp/afp_put_test.txt
run "upload new file" kioclient copy /tmp/afp_put_test.txt "$AFP_URL/afp_put_test.txt"

echo "== TEST 2. Upload overwrite (put - overwrite with smaller file)"
echo "$TEST_CONTENT_SHORT" > /tmp/afp_small.txt
run "upload overwrite" kioclient copy --overwrite /tmp/afp_small.txt "$AFP_URL/afp_put_test.txt"

echo "== TEST 3. Verify overwrite correctness"
kioclient copy "$AFP_URL/afp_put_test.txt" /tmp/afp_verify.txt
if [[ "$(cat /tmp/afp_verify.txt)" == "$TEST_CONTENT_SHORT" ]]; then
    ok "overwrite content correct"
else
    fail "overwrite content correct"
fi

echo "== TEST 4. List volumes and directory"
run "list volumes"   kioclient ls "$AFP_SERVER/"
run "list directory" kioclient ls "$AFP_URL/"

echo "== TEST 5. Download a file (get)"
kioclient copy --overwrite "$AFP_URL/afp_put_test.txt" "$DOWNLOAD" || true
if [[ "$(cat "$DOWNLOAD")" == "$TEST_CONTENT_SHORT" ]]; then
    ok "download file"
else
    fail "download file"
fi

echo "== TEST 6. mkdir, rename, delete"
run "mkdir"  kioclient mkdir "$AFP_URL/testdir"
run "rename" kioclient move "$AFP_URL/afp_put_test.txt" "$AFP_URL/testdir/moved.txt"
run "delete file" kioclient remove "$AFP_URL/testdir/moved.txt"
run "delete dir"  kioclient remove "$AFP_URL/testdir"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]

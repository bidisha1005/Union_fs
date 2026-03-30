#!/bin/bash
# test_unionfs.sh — Full test suite for Mini-UnionFS
# Run from the same directory as the compiled binary: ./test_unionfs.sh

#!/bin/bash

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER="$TEST_DIR/lower"
UPPER="$TEST_DIR/upper"
MNT="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

check() {
    if [ "$1" = "ok" ]; then
        echo -e "${GREEN}PASSED${NC} - $2"
        ((PASS++))
    else
        echo -e "${RED}FAILED${NC} - $2"
        ((FAIL++))
    fi
}

cleanup() {
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
    rm -rf "$TEST_DIR"
}

cleanup

echo "======================================"
echo "  Mini-UnionFS FULL Test Suite"
echo "======================================"

mkdir -p "$LOWER" "$UPPER" "$MNT"

# -------- Setup --------
echo "lower_file" > "$LOWER/a.txt"
echo "delete_me" > "$LOWER/b.txt"
echo "lower_version" > "$LOWER/shared.txt"

mkdir -p "$LOWER/dir"
echo "dir_content" > "$LOWER/dir/x.txt"

echo "upper_version" > "$UPPER/shared.txt"

# -------- Mount --------
$FUSE_BINARY "$LOWER" "$UPPER" "$MNT" &
sleep 1

# -------- Test 1: getattr + read --------
res="fail"
grep -q "lower_file" "$MNT/a.txt" && res="ok"
check "$res" "getattr + read"

# -------- Test 2: upper overrides lower --------
res="fail"
[ "$(cat $MNT/shared.txt)" = "upper_version" ] && res="ok"
check "$res" "upper precedence"

# -------- Test 3: readdir --------
res="fail"
ls "$MNT" | grep -q "a.txt" && \
ls "$MNT" | grep -q "shared.txt" && res="ok"
check "$res" "readdir merged view"

# -------- Test 4: subdirectory --------
res="fail"
grep -q "dir_content" "$MNT/dir/x.txt" && res="ok"
check "$res" "directory access"

# -------- Test 5: Copy-on-Write --------
echo "modified" >> "$MNT/a.txt"
sleep 0.5

res="fail"
grep -q "modified" "$UPPER/a.txt" && \
! grep -q "modified" "$LOWER/a.txt" && res="ok"
check "$res" "copy-on-write"

# -------- Test 6: write --------
res="fail"
grep -q "modified" "$MNT/a.txt" && res="ok"
check "$res" "write correctness"
echo ""
echo "Partial Results: $PASS passed, $FAIL failed"
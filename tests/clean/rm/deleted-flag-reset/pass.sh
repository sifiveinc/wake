#!/bin/sh
# Test: deleted flag is properly reset when file is re-created after being deleted

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"
WAKE="$(realpath "$WAKE")"

rm -rf wake.db* wake.log .wake .build dir

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# Create a file and add it to the database.
"${WAKE}" -q --no-tty makeFile dir/file.txt
"${WAKE}" --output dir/file.txt | sed 's:\$ \(\(/usr\)\?/bin/\)dash:$ dash:'

# Verify they exist and that both show deleted=0 ("not deleted").
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir/file.txt'")
test "$DELETED" = "0" || fail "Initial file should have deleted=0, got: $DELETED"
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir'")
test "$DELETED" = "0" || fail "Initial directory should have deleted=0, got: $DELETED"

# Remove the file (sets deleted=1 in database).
"${WAKE}" --rm dir/file.txt

# Verify deleted=1 for the file, but that the directory is still deleted=0.
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir/file.txt'")
test "$DELETED" = "1" || fail "After --rm, file should have deleted=1, got: $DELETED"
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir'")
test "$DELETED" = "0" || fail "Initial directory should have deleted=0, got: $DELETED"

# Re-create the exact same file (same path, same hash) to test recursive deletion.
"${WAKE}" -q --no-tty makeFile dir/file.txt

# Verify deleted was reset to 0 since it's owned by the job once again.
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir/file.txt'")
test "$DELETED" = "0" || fail "After re-creating file, deleted should be reset to 0, got: $DELETED"

# Delete both paths to ensure implicit (recursive) deletion marks the children properly.
"${WAKE}" --rm -r dir

DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir/file.txt'")
test "$DELETED" = "1" || fail "After --rm, recreated file should have deleted=1, got: $DELETED"
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir'")
test "$DELETED" = "1" || fail "After --rm, directory should have deleted=0, got: $DELETED"

# Re-create the file once more to test (lack of) job reuse -- there's no longer any blob, so the job has to be re-run.
"${WAKE}" -q --no-tty makeFile dir/file.txt
"${WAKE}" --output dir/file.txt | sed 's:\$ \(\(/usr\)\?/bin/\)dash:$ dash:'

# Verify the flag is reset to deleted=0 for the directory as well.
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='dir'")
test "$DELETED" = "0" || fail "After re-creating directory, deleted should be reset to 0, got: $DELETED"

echo "PASS: deleted flag reset for files and directories" >&2

# Clean up
rm -rf dir

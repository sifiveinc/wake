#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

# Since we are writing jobs to the database in this test we should clear it for each run if it
# happens to exist from a previous run
rm -f wake.db wake.log

"${WAKE:-wake}" --stdout=warning,report testRunnerFailFinish

#! /bin/sh

WAKE="${1:+$1/wake}"

# Since we are writing jobs to the database in this test we should clear it for each run if it
# happens to exist from a previous run
rm -f wake.db wake.log

# Run the basic runner tests
"${WAKE:-wake}" --stdout=warning,report testRunnerFailSuccess
"${WAKE:-wake}" --stdout=warning,report testRunnerFailWithJobFailure
"${WAKE:-wake}" --stdout=warning,report testRunnerOutputStatus
"${WAKE:-wake}" --stdout=warning,report testRunnerFailFinish
"${WAKE:-wake}" --stdout=warning,report testRunnerOkSuccess
"${WAKE:-wake}" --stdout=warning,report testWrapperRunnerStatus

# Test file descriptor outputs
"${WAKE:-wake}" --stdout=warning,report testFdOutputs

# Test the --failed command to verify it shows jobs with runner errors
"${WAKE:-wake}" --stdout=warning,report --failed

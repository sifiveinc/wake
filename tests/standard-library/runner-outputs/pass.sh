#! /bin/sh

WAKE="${1:+$1/wake}"

rm -f wake.db wake.log

"${WAKE:-wake}" --stdout=warning,report testRunnerFailSuccess
"${WAKE:-wake}" --stdout=warning,report testRunnerFailWithJobFailure
"${WAKE:-wake}" --stdout=warning,report testRunnerOutputStatus
"${WAKE:-wake}" --stdout=warning,report testRunnerFailFinish
"${WAKE:-wake}" --stdout=warning,report testRunnerOkSuccess
"${WAKE:-wake}" --stdout=warning,report testWrapperRunnerStatus
"${WAKE:-wake}" --stdout=warning,report testFdOutputs
"${WAKE:-wake}" --stdout=warning,report testRunnerCustomOutput
"${WAKE:-wake}" --stdout=warning,report --failed

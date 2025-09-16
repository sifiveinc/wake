#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

"${WAKE:-wake}" --stdout=warning,report test1
"${WAKE:-wake}" --stdout=warning,report test2
"${WAKE:-wake}" --stdout=warning,report test3
"${WAKE:-wake}" --stdout=warning,report test4
"${WAKE:-wake}" --stdout=warning,report test5
"${WAKE:-wake}" --stdout=warning,report test6
"${WAKE:-wake}" --stdout=warning,report test7
"${WAKE:-wake}" --stdout=warning,report test8
"${WAKE:-wake}" --stdout=warning,report test9
"${WAKE:-wake}" --stdout=warning,report test10
"${WAKE:-wake}" --stdout=warning,report test11
"${WAKE:-wake}" --stdout=warning,report test12
"${WAKE:-wake}" --stdout=warning,report test13
"${WAKE:-wake}" --stdout=warning,report test14
"${WAKE:-wake}" --stdout=warning,report test15
"${WAKE:-wake}" --stdout=warning,report test16
"${WAKE:-wake}" --stdout=warning,report test17
"${WAKE:-wake}" --stdout=warning,report test18

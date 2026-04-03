#!/bin/sh
# Two runs write different data to same file, with downstream jobs reading it.
# Check each run gets the expected data, and check reuse works as well.

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf .cas wake.db* wake.log output.txt result-a.txt result-b.txt

echo "Fresh concurrent runs:"

"${WAKE}" -q -x "consumerA Unit" &
"${WAKE}" -q -x "consumerB Unit" &

wait

# (output.txt can have either value)
tail result-a.txt result-b.txt

echo
echo "Reuse:"

"${WAKE}" -q -x "consumerA Unit" &
"${WAKE}" -q -x "consumerB Unit" &

wait

# (output.txt can have either value)
tail result-a.txt result-b.txt

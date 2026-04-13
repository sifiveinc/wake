#! /bin/sh

WAKE="${1:+$1/wake}"
# Prune the fields which are not commonly deterministic.
"${WAKE:-wake}" --config | sed '/^  \(user_config\|log_header\|log_header_source_width\) = /d'

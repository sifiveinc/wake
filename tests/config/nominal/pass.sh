#! /bin/sh

if [ $(uname) != Linux ] ; then
  cat stdout
  exit 0
fi

WAKE="${1:+$1/wake}"
WAKE_SHARED_CACHE_MAX_SIZE=1024 WAKE_PROPERTIES='c=env-c,d=env-d' \
  "${WAKE:-wake}" --property d=cli-d --property d=cli-final --property e=value=with=equals --config

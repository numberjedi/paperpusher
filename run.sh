#!/bin/bash
export LSAN_OPTIONS="suppressions=./lsan.supp:log_threads=1"
exec ./paperpusher "$@"

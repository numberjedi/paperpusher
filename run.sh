#!/bin/bash
export LSAN_OPTIONS="suppressions=./lsan.supp:log_threads=1"
export ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0:strict_string_checks=1:abort_on_error=1"
exec ./paperpusher "$@"

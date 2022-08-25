#!/bin/bash -e

[ mutex-test.c -ot mutex-test ] || gcc -O0 -g mutex-test.c -o mutex-test -I/usr/evl/include /usr/evl/lib/aarch64-linux-gnu/libevl.so -lpthread -pthread
[ mutex-test.c -ot mutex-test-pthread ] || gcc -O0 -g mutex-test.c -DUSE_PTHREAD -o mutex-test-pthread -lpthread -pthread

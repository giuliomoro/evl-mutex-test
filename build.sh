#!/bin/bash

[ mutex-test.c -ot mutex-test ] || gcc -O0 mutex-test.c -o mutex-test -I/usr/evl/include /usr/evl/lib/aarch64-linux-gnu/libevl.so -lpthread -pthread

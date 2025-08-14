#!/bin/bash
cmake -S $1/ -B $1/build
cmake --build $1/build/
ln -s ../koya-plugin/$1/build/libsm-$1.so ../koya-base/libsm-$1.so
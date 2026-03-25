#!/bin/bash
if [ -d "build" ]; then
  rm -rf build
else
    mkdir build
fi

cd build
cmake ..
make
if (( $? !=0 )); then
    echo "Build failed"
    exit 1
else
    echo "Build succeeded"
fi
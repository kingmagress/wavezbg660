#!/bin/bash
set -e

echo "update built-in wave"
echo "const char default_wave_txt[] =" > src/default_wave.h
sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^\(.*\)$/"\1\\n"/' wave.txt >> src/default_wave.h
echo ";" >> src/default_wave.h
mkdir -p build
cd build
echo "CMake..."
psp-cmake -DBUILD_PRX=1 -DENC_PRX=1 ..
echo "Build..."
make
echo "Kachow!"

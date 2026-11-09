#!/bin/sh
set -e
IMAGE=${REBENCH_IMAGE:-quay.io/pypa/manylinux2014_i686:2026.11.01-2}
SRC=$(cd "$(dirname "$0")/.." && pwd)
RUNTIME=${CONTAINER_RUNTIME:-$(command -v podman || command -v docker)}

"$RUNTIME" run --rm --platform linux/386 \
    -v "$SRC":/src -w /src "$IMAGE" \
    sh -c 'cmake -S . -B build/manylinux -DCMAKE_BUILD_TYPE=Release &&
           cmake --build build/manylinux -j"$(nproc)"'
echo "$SRC/build/manylinux/rebench_mm_i386.so"
echo "$SRC/build/manylinux/rebench_amxx_i386.so"

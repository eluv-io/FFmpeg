#!/usr/bin/env bash

set -euo pipefail

if [[ ! -x ./configure ]]; then
    echo "Run this script from the FFmpeg repository root inside the container." >&2
    exit 1
fi

ensure_build_toolchain() {
    local needs_install=0

    command -v clang >/dev/null 2>&1 || needs_install=1
    command -v make >/dev/null 2>&1 || needs_install=1
    command -v nasm >/dev/null 2>&1 || needs_install=1
    command -v pkg-config >/dev/null 2>&1 || needs_install=1
    command -v perl >/dev/null 2>&1 || needs_install=1
    command -v python3 >/dev/null 2>&1 || needs_install=1

    if [[ ${needs_install} -eq 0 ]]; then
        return
    fi

    apt-get update
    apt-get install --no-install-recommends --yes \
        build-essential \
        ca-certificates \
        nasm \
        perl \
        pkg-config \
        python3 \
        yasm
    apt-get clean
    rm -rf /var/lib/apt/lists/*
}

ensure_build_toolchain

export LD_LIBRARY_PATH="/runtime/lib:/runtime/lib/uf:/runtime/lib/3rdparty:${LD_LIBRARY_PATH:-}"

extra_cflags="-DUSE_UF_RENDERLIB -I/runtime/include"
extra_ldflags="-L/runtime/lib -L/runtime/lib/uf -L/runtime/lib/3rdparty \
-Wl,-rpath,/runtime/lib -Wl,-rpath,/runtime/lib/uf -Wl,-rpath,/runtime/lib/3rdparty \
-Wl,-rpath-link,/runtime/lib -Wl,-rpath-link,/runtime/lib/uf -Wl,-rpath-link,/runtime/lib/3rdparty"
extra_libs="-Wl,--unresolved-symbols=ignore-in-shared-libs -luf-renderlib"

./configure \
    --cc=clang \
    --enable-debug=3 \
    --disable-optimizations \
    --disable-stripping \
    --extra-cflags="${extra_cflags}" \
    --extra-ldflags="${extra_ldflags}" \
    --extra-libs="${extra_libs}" \
    "$@"

make -j"$(nproc)" doc/examples/transcoding
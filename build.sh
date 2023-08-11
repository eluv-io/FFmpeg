#!/bin/bash
ffmpeg_proj_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "$DIST" ]; then
    DIST=${ffmpeg_proj_dir}/"dist"
fi

if [ ! -d ${DIST} ]; then
    mkdir -p ${DIST}
    echo "CREATED ${DIST}"
fi

echo "DIST=${DIST}"

CONFIGURE_TO_USE="./configure"

command_options=(
    --prefix=${DIST}
    --enable-libfreetype
    --enable-libfribidi
    --enable-libfontconfig
    --enable-shared
    --enable-avresample
    --enable-pthreads
    --enable-version3
    --enable-hardcoded-tables
    --cc=clang
    --host-cflags=-fPIC
    --host-ldflags=
    --enable-gpl
    --enable-libmp3lame
    --enable-libx264
    --enable-libx265
    --enable-libxvid
    --enable-opencl
    --disable-lzma
)

if [ "$1" == "--DEBUG" ] || [ "$1" == "--debug" ]; then
    CONFIGURE_TO_USE="./configure.debug"
    command_options+=(
        --disable-stripping
        --enable-debug=3
        --extra-cflags=-ggdb
        --extra-cflags=-O0
        --extra-cflags=-g
    )
fi

if [ "$(uname)" == "Darwin" ]; then
    command_options+=(
        --enable-videotoolbox
    )
    HOMEBREW_LOCATION="$(brew --prefix)"
    if [ -n "${HOMEBREW_LOCATION}" -a -d "${HOMEBREW_LOCATION}" ]; then
        command_options+=(
            --extra-ldflags=-L${HOMEBREW_LOCATION}/lib
            --extra-cflags=-I${HOMEBREW_LOCATION}/include
        )
    fi
    echo "configuring with command_options=${command_options[@]}"
elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
    command_options+=(
        --extra-cflags=-fPIC
    )
    if find /usr/local/cuda* -name version.txt | read; then
        if [ -d /usr/local/cuda/include -a -d /usr/local/cuda/lib64 ]; then
            command_options+=(
                --enable-cuda
                --enable-cuvid
                --enable-nvenc
                --enable-nonfree
                --enable-libnpp
                --extra-cflags=-I/usr/local/cuda/include
                --extra-ldflags=-L/usr/local/cuda/lib64
            )
        else
            echo "CUDA found, but not linked to /usr/local/cuda, so skipping setup"
        fi
    fi
    echo "configuring with command_options=${command_options[@]}"

elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW32_NT" ]; then
    echo "MINGW32 Not yet supported"
    exit 1
elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW64_NT" ]; then
    echo "MINGW64 Not yet supported"
    exit 1
fi

(
    cd ${ffmpeg_proj_dir}
    ${CONFIGURE_TO_USE} ${command_options[@]}
)

NPROCS=$(getconf _NPROCESSORS_ONLN)
ELUVIO_BUILD_THREAD_COUNT=${ELUVIO_BUILD_THREAD_COUNT:-"-j${NPROCS}"}
ELUVIO_TEST_THREAD_COUNT=${ELUVIO_TEST_THREAD_COUNT:-"${NPROCS}"}
ELUVIO_BUILD_THREAD_COUNT=${ELUVIO_BUILD_THREAD_COUNT:-"-j4"}

make ${ELUVIO_BUILD_THREAD_COUNT} && make install

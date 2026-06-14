#!/usr/bin/env bash

set -euo pipefail

if [[ ! -x ./doc/examples/transcoding ]]; then
    echo "doc/examples/transcoding is not built yet. Run tools/build-uniqfeed-example.sh first." >&2
    exit 1
fi

repo_root=$(pwd)

export LD_LIBRARY_PATH="${repo_root}/libavcodec:${repo_root}/libavdevice:${repo_root}/libavfilter:${repo_root}/libavformat:${repo_root}/libavresample:${repo_root}/libavutil:${repo_root}/libpostproc:${repo_root}/libswresample:${repo_root}/libswscale:/runtime/lib:/runtime/lib/uf:/runtime/lib/3rdparty:${LD_LIBRARY_PATH:-}"

exec ./doc/examples/transcoding "$@"
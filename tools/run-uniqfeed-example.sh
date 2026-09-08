#!/usr/bin/env bash

set -euo pipefail

if [[ ! -x ./doc/examples/transcode ]]; then
    echo "doc/examples/transcode is not built yet. Run tools/build-uniqfeed-example.sh first." >&2
    exit 1
fi

repo_root=$(pwd)
runtime_root="${UF_RUNTIME_ROOT:-/runtime}"
extra_runtime_lib_dirs="${UF_RUNTIME_EXTRA_LIB_DIRS:-}"

runtime_ld_path="${repo_root}/libavcodec:${repo_root}/libavdevice:${repo_root}/libavfilter:${repo_root}/libavformat:${repo_root}/libavresample:${repo_root}/libavutil:${repo_root}/libpostproc:${repo_root}/libswresample:${repo_root}/libswscale:${runtime_root}/lib:${runtime_root}/lib/uf:${runtime_root}/lib/3rdparty"
if [[ -n "${extra_runtime_lib_dirs}" ]]; then
    runtime_ld_path="${runtime_ld_path}:${extra_runtime_lib_dirs}"
fi
export LD_LIBRARY_PATH="${runtime_ld_path}:${LD_LIBRARY_PATH:-}"

exec ./doc/examples/transcode "$@"
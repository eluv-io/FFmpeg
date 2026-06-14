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

runtime_root="${UF_RUNTIME_ROOT:-/runtime}"
cc_bin="${CC:-clang}"
extra_runtime_lib_dirs="${UF_RUNTIME_EXTRA_LIB_DIRS:-}"
build_state_dir=".ffbuild-uniqfeed"
build_config_file="${build_state_dir}/last-build-config"
clean_build=0
declare -a configure_args=()

for arg in "$@"; do
    case "${arg}" in
        --clean)
            clean_build=1
            ;;
        *)
            configure_args+=("${arg}")
            ;;
    esac
done

if [[ ! -d "${runtime_root}/include" ]]; then
    echo "uniqFEED include directory not found at ${runtime_root}/include" >&2
    exit 1
fi

if [[ ! -d "${runtime_root}/lib" ]]; then
    echo "uniqFEED library directory not found at ${runtime_root}/lib" >&2
    exit 1
fi

runtime_ld_path="${runtime_root}/lib:${runtime_root}/lib/uf:${runtime_root}/lib/3rdparty"
if [[ -n "${extra_runtime_lib_dirs}" ]]; then
    runtime_ld_path="${runtime_ld_path}:${extra_runtime_lib_dirs}"
fi
export LD_LIBRARY_PATH="${runtime_ld_path}:${LD_LIBRARY_PATH:-}"

extra_cflags="-DUSE_UF_RENDERLIB -I${runtime_root}/include"
extra_ldflags="-L${runtime_root}/lib -L${runtime_root}/lib/uf -L${runtime_root}/lib/3rdparty \
-Wl,--disable-new-dtags \
-Wl,-rpath,${runtime_root}/lib -Wl,-rpath,${runtime_root}/lib/uf -Wl,-rpath,${runtime_root}/lib/3rdparty \
-Wl,-rpath-link,${runtime_root}/lib -Wl,-rpath-link,${runtime_root}/lib/uf -Wl,-rpath-link,${runtime_root}/lib/3rdparty"
extra_libs="-Wl,--unresolved-symbols=ignore-in-shared-libs -luf-renderlib"

mkdir -p "${build_state_dir}"

new_build_config_file=$(mktemp)
cleanup_temp_files() {
    rm -f "${new_build_config_file}"
}
trap cleanup_temp_files EXIT

{
    printf 'cc=%s\n' "${cc_bin}"
    printf 'runtime_root=%s\n' "${runtime_root}"
    printf 'extra_runtime_lib_dirs=%s\n' "${extra_runtime_lib_dirs}"
    printf 'extra_cflags=%s\n' "${extra_cflags}"
    printf 'extra_ldflags=%s\n' "${extra_ldflags}"
    printf 'extra_libs=%s\n' "${extra_libs}"
    printf 'arg_count=%s\n' "${#configure_args[@]}"
    arg_index=0
    for arg in "${configure_args[@]}"; do
        printf 'arg_%s=%s\n' "${arg_index}" "${arg}"
        arg_index=$((arg_index + 1))
    done
} > "${new_build_config_file}"

./configure \
    --cc="${cc_bin}" \
    --enable-debug=3 \
    --disable-optimizations \
    --disable-stripping \
    --extra-cflags="${extra_cflags}" \
    --extra-ldflags="${extra_ldflags}" \
    --extra-libs="${extra_libs}" \
    "${configure_args[@]}"

if [[ ${clean_build} -eq 1 ]] || [[ ! -f "${build_config_file}" ]] || ! cmp -s "${build_config_file}" "${new_build_config_file}"; then
    # The repo can carry stale dependency files across materially different configure states.
    find . \( -name '*.o' -o -name '*.d' \) -delete
fi

mv "${new_build_config_file}" "${build_config_file}"

make -j"$(nproc)" doc/examples/transcode
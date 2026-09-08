#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  tools/uniqfeed-container.sh build [ffmpeg-configure-args...]
  tools/uniqfeed-container.sh run <input> <output> <project_path> <metadata_dir>
    tools/uniqfeed-container.sh shell [bash-args...]

Environment:
  UF_BASE_IMAGE   uniqFEED container image tag
                  default: uf_render_interface:ubuntu_22
    UF_SKIP_RUNTIME_PREFLIGHT
                                    set to 1 to skip host-side shared-library preflight checks
                                    when running against a host-mounted uniqFEED runtime

    CC, CXX, CFLAGS, CXXFLAGS, CPPFLAGS, LDFLAGS, PKG_CONFIG_PATH
                                    optional toolchain and build environment overrides passed
                                    through to docker compose run invocations

Examples:
  tools/uniqfeed-container.sh build
  tools/uniqfeed-container.sh build --enable-gpl --enable-shared
  tools/uniqfeed-container.sh run input.mp4 output.mp4 /runtime/project /runtime/example_input
  tools/uniqfeed-container.sh shell
    tools/uniqfeed-container.sh shell -lc 'ffmpeg -hide_banner -h filter=abuffersink'
EOF
}

require_image_exists() {
    local image_name=$1

    if docker image inspect "${image_name}" >/dev/null 2>&1; then
        return
    fi

    echo "uniqFEED base image not found locally: ${image_name}" >&2
    echo "Build or pull that image from the uniqFEED repository workflow, then rerun this wrapper." >&2
    exit 1
}

expand_path() {
    local path=$1

    case "${path}" in
        ~)
            printf '%s\n' "${HOME}"
            ;;
        ~/*)
            printf '%s\n' "${HOME}/${path#~/}"
            ;;
        *)
            printf '%s\n' "${path}"
            ;;
    esac
}

add_mount_if_needed() {
    local -n mounts_ref=$1
    local -n seen_ref=$2
    local host_path=$3
    local mode=$4
    local mount_path

    [[ "${host_path}" = /* ]] || return 0
    [[ "${host_path}" = /runtime || "${host_path}" = /runtime/* ]] && return 0

    if [[ -d "${host_path}" ]]; then
        mount_path=${host_path}
    else
        mount_path=$(dirname "${host_path}")
    fi

    [[ -d "${mount_path}" ]] || return 0

    if [[ -z "${seen_ref[${mount_path}]+x}" ]]; then
        mounts_ref+=( -v "${mount_path}:${mount_path}:${mode}" )
        seen_ref[${mount_path}]=1
    fi
}

append_env_if_set() {
    local -n cmd_ref=$1
    local var_name=$2

    if [[ -n "${!var_name+x}" ]]; then
        cmd_ref+=( -e "${var_name}=${!var_name}" )
    fi
}

preflight_runtime_dependencies() {
    local runtime_root_path=$1
    local extra_lib_dirs=$2
    local renderlib_path
    local runtime_ld_path
    local missing_libs

    if [[ "${UF_SKIP_RUNTIME_PREFLIGHT:-0}" = "1" ]]; then
        return
    fi

    # /runtime paths are container-local and cannot be validated on the host.
    if [[ "${runtime_root_path}" = /runtime || "${runtime_root_path}" = /runtime/* ]]; then
        return
    fi

    renderlib_path="${runtime_root_path}/lib/libuf-renderlib.so"
    if [[ ! -f "${renderlib_path}" ]]; then
        echo "uniqFEED preflight warning: render library not found at ${renderlib_path}; skipping dependency check." >&2
        return
    fi

    if ! command -v ldd >/dev/null 2>&1; then
        echo "uniqFEED preflight warning: ldd is not available on host; skipping dependency check." >&2
        return
    fi

    runtime_ld_path="${runtime_root_path}/lib:${runtime_root_path}/lib/uf:${runtime_root_path}/lib/3rdparty"
    if [[ -n "${extra_lib_dirs}" ]]; then
        runtime_ld_path="${runtime_ld_path}:${extra_lib_dirs}"
    fi

    missing_libs=$(LD_LIBRARY_PATH="${runtime_ld_path}:${LD_LIBRARY_PATH:-}" \
        ldd "${renderlib_path}" 2>/dev/null | awk '/=> not found/{print $1}')

    if [[ -n "${missing_libs}" ]]; then
        echo "uniqFEED runtime preflight failed: unresolved shared-library dependencies for ${renderlib_path}" >&2
        echo "Missing libraries:" >&2
        while IFS= read -r lib_name; do
            [[ -n "${lib_name}" ]] && echo "  - ${lib_name}" >&2
        done <<< "${missing_libs}"
        echo "Set UF_RUNTIME_EXTRA_LIB_DIRS to include directories containing these libraries," >&2
        echo "or use a runtime root that bundles the required dependencies." >&2
        exit 1
    fi
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 1
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
compose_file="${repo_root}/docker-compose.uniqfeed.yml"

if [[ -z "${UF_BASE_IMAGE+x}" && -f "${repo_root}/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "${repo_root}/.env"
    set +a
fi

base_image="${UF_BASE_IMAGE:-uf_render_interface:ubuntu_22}"
runtime_root=$(expand_path "${UF_RUNTIME_ROOT:-/runtime}")
extra_runtime_lib_dirs="${UF_RUNTIME_EXTRA_LIB_DIRS:-}"
host_uid=$(id -u)
host_gid=$(id -g)

require_image_exists "${base_image}"

declare -a runtime_mounts=()
declare -A runtime_seen_mounts=()

add_mount_if_needed runtime_mounts runtime_seen_mounts "${runtime_root}" ro

compose_run_cmd=(docker compose -f "${compose_file}" run --rm -e UF_BASE_IMAGE="${base_image}" -e UF_RUNTIME_ROOT="${runtime_root}")
append_env_if_set compose_run_cmd UF_RUNTIME_EXTRA_LIB_DIRS
append_env_if_set compose_run_cmd UF_RENDERLIB_PASSTHROUGH_ON_FAILURE
append_env_if_set compose_run_cmd CC
append_env_if_set compose_run_cmd CXX
append_env_if_set compose_run_cmd CFLAGS
append_env_if_set compose_run_cmd CXXFLAGS
append_env_if_set compose_run_cmd CPPFLAGS
append_env_if_set compose_run_cmd LDFLAGS
append_env_if_set compose_run_cmd PKG_CONFIG_PATH

command_name=$1
shift

case "${command_name}" in
    build)
        "${compose_run_cmd[@]}" "${runtime_mounts[@]}" ffmpeg-uniqfeed tools/build-uniqfeed-example.sh "$@"
        ;;
    run)
        if [[ $# -ne 4 ]]; then
            echo "run requires 4 arguments" >&2
            usage >&2
            exit 1
        fi

        input_path=$(expand_path "$1")
        output_path=$(expand_path "$2")
        project_path=$(expand_path "$3")
        metadata_path=$(expand_path "$4")

        declare -a extra_mounts=()
        declare -A seen_mounts=()

        add_mount_if_needed extra_mounts seen_mounts "${input_path}" ro
        add_mount_if_needed extra_mounts seen_mounts "${output_path}" rw
        add_mount_if_needed extra_mounts seen_mounts "${project_path}" ro
        add_mount_if_needed extra_mounts seen_mounts "${metadata_path}" ro

        preflight_runtime_dependencies "${runtime_root}" "${extra_runtime_lib_dirs}"

        "${compose_run_cmd[@]}" --user "${host_uid}:${host_gid}" "${runtime_mounts[@]}" "${extra_mounts[@]}" ffmpeg-uniqfeed tools/run-uniqfeed-example.sh \
            "${input_path}" "${output_path}" "${project_path}" "${metadata_path}"
        ;;
    shell)
        exec "${compose_run_cmd[@]}" \
            "${runtime_mounts[@]}" \
            ffmpeg-uniqfeed "$@"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        echo "Unknown command: ${command_name}" >&2
        usage >&2
        exit 1
        ;;
esac
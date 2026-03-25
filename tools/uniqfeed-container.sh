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

Examples:
  tools/uniqfeed-container.sh build
  tools/uniqfeed-container.sh build --enable-gpl --enable-shared
  tools/uniqfeed-container.sh run input.mp4 output.mp4 /runtime/project /runtime/example_input
  tools/uniqfeed-container.sh shell
    tools/uniqfeed-container.sh shell -lc 'ffmpeg -hide_banner -h filter=abuffersink'
EOF
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
    [[ "${host_path}" = /runtime/* ]] && return 0

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
host_uid=$(id -u)
host_gid=$(id -g)

compose_run_cmd=(docker compose -f "${compose_file}" run --rm -e UF_BASE_IMAGE="${base_image}")

if [[ -n "${UF_RENDERLIB_PASSTHROUGH_ON_FAILURE+x}" ]]; then
    compose_run_cmd+=( -e UF_RENDERLIB_PASSTHROUGH_ON_FAILURE="${UF_RENDERLIB_PASSTHROUGH_ON_FAILURE}" )
fi

command_name=$1
shift

case "${command_name}" in
    build)
        "${compose_run_cmd[@]}" ffmpeg-uniqfeed tools/build-uniqfeed-example.sh "$@"
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

        "${compose_run_cmd[@]}" --user "${host_uid}:${host_gid}" "${extra_mounts[@]}" ffmpeg-uniqfeed tools/run-uniqfeed-example.sh \
            "${input_path}" "${output_path}" "${project_path}" "${metadata_path}"
        ;;
    shell)
        exec docker compose -f "${compose_file}" run --rm \
            -e UF_BASE_IMAGE="${base_image}" \
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
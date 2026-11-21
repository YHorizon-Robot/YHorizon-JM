#!/usr/bin/env bash
# Build YHorizon-JM firmware on Ubuntu (CMake + Ninja + ARM GCC).
# Usage: ./tools/build.sh [--preset gd32f303|gd32f303-debug|gd32f303-factory|gd32f303-dm4310]
# Run ./tools/setup-env.sh first on a new machine.

set -euo pipefail

PRESET="gd32f303"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            PRESET="${2:-}"
            shift 2
            ;;
        gd32f303|gd32f303-debug|gd32f303-factory|gd32f303-dm4310)
            PRESET="$1"
            shift
            ;;
        -h|--help)
            sed -n '2,4p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            echo "usage: $0 [--preset gd32f303|gd32f303-debug|gd32f303-factory|gd32f303-dm4310]" >&2
            exit 1
            ;;
    esac
done

case "${PRESET}" in
    gd32f303|gd32f303-debug|gd32f303-factory|gd32f303-dm4310) ;;
    *)
        echo "unsupported preset: ${PRESET}" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=yhjm-env.sh
. "${SCRIPT_DIR}/yhjm-env.sh"
yhjm_import_env

cmake_exe="$(yhjm_find_cmake)" || {
    echo "cmake not found. Run ./tools/setup-env.sh first." >&2
    exit 1
}

echo "cmake: ${cmake_exe}"
echo "preset: ${PRESET}"

cd "${YHJM_FIRMWARE}"
"${cmake_exe}" --preset "${PRESET}"
"${cmake_exe}" --build --preset "${PRESET}"
echo "OK: ${YHJM_FIRMWARE}/build/${PRESET}/YHorizon-JM.elf"

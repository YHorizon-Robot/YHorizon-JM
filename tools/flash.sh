#!/usr/bin/env bash
# Flash YHorizon-JM onto GD32F303 with OpenOCD (CMSIS-DAP / SWD).
# Ubuntu OpenOCD typically uses tools/openocd-gd32f303-stm32f1x.cfg
# (256 KB, no mass_erase). Prefer a build with gd32f30x if you have one.
#
#   ./tools/flash.sh
#   ./tools/flash.sh --build
#   ./tools/flash.sh --preset gd32f303-debug

set -euo pipefail

PRESET="gd32f303"
ELF_FILE=""
OPENOCD_EXE=""
OPENOCD_SCRIPTS=""
CONFIG_FILE=""
BUILD_BEFORE=0
NO_RESET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            PRESET="${2:-}"
            shift 2
            ;;
        --elf)
            ELF_FILE="${2:-}"
            shift 2
            ;;
        --openocd)
            OPENOCD_EXE="${2:-}"
            shift 2
            ;;
        --scripts)
            OPENOCD_SCRIPTS="${2:-}"
            shift 2
            ;;
        --config)
            CONFIG_FILE="${2:-}"
            shift 2
            ;;
        --build|-b)
            BUILD_BEFORE=1
            shift
            ;;
        --no-reset)
            NO_RESET=1
            shift
            ;;
        -h|--help)
            sed -n '2,9p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

case "${PRESET}" in
    gd32f303|gd32f303-debug|gd32f303-factory) ;;
    *)
        echo "unsupported preset: ${PRESET}" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=yhjm-env.sh
. "${SCRIPT_DIR}/yhjm-env.sh"
yhjm_import_env

if [[ "${BUILD_BEFORE}" -eq 1 ]]; then
    "${SCRIPT_DIR}/build.sh" --preset "${PRESET}"
fi

if [[ -z "${OPENOCD_EXE}" || -z "${OPENOCD_SCRIPTS}" ]]; then
    yhjm_find_openocd || true
    [[ -z "${OPENOCD_EXE}" ]] && OPENOCD_EXE="${YHJM_OPENOCD_EXE:-}"
    [[ -z "${OPENOCD_SCRIPTS}" ]] && OPENOCD_SCRIPTS="${YHJM_OPENOCD_SCRIPTS:-}"
fi

has_gd32=0
if [[ -n "${OPENOCD_SCRIPTS}" ]] && yhjm_openocd_has_gd32 "${OPENOCD_SCRIPTS}"; then
    has_gd32=1
fi

if [[ -z "${CONFIG_FILE}" ]]; then
    if [[ "${has_gd32}" -eq 1 ]]; then
        CONFIG_FILE="${SCRIPT_DIR}/openocd-gd32f303.cfg"
    else
        CONFIG_FILE="${SCRIPT_DIR}/openocd-gd32f303-stm32f1x.cfg"
    fi
fi

if [[ -z "${ELF_FILE}" ]]; then
    ELF_FILE="${YHJM_FIRMWARE}/build/${PRESET}/YHorizon-JM.elf"
fi

if [[ -z "${OPENOCD_EXE}" || ! -x "${OPENOCD_EXE}" ]]; then
    echo "OpenOCD executable not found. Run ./tools/setup-env.sh or pass --openocd." >&2
    exit 1
fi
if [[ -z "${OPENOCD_SCRIPTS}" || ! -d "${OPENOCD_SCRIPTS}" ]]; then
    echo "OpenOCD scripts directory not found. Run ./tools/setup-env.sh or pass --scripts." >&2
    exit 1
fi
if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "OpenOCD config not found: ${CONFIG_FILE}" >&2
    exit 1
fi
if [[ ! -f "${ELF_FILE}" ]]; then
    echo "ELF file not found: ${ELF_FILE}. Run ./tools/build.sh --preset ${PRESET} first." >&2
    exit 1
fi

resolved_elf="$(readlink -f "${ELF_FILE}")"

echo "Preset: ${PRESET}"
echo "ELF: ${resolved_elf}"
echo "OpenOCD: ${OPENOCD_EXE}"
if [[ "${has_gd32}" -eq 1 ]]; then
    echo "Flash: gd32f30x program+verify (no mass_erase)"
else
    echo "Flash: stm32f1x fallback 256 KB program+verify (no mass_erase)"
fi

prog_cmd="program \"${resolved_elf}\" verify reset exit"
if [[ "${NO_RESET}" -eq 1 ]]; then
    prog_cmd="program \"${resolved_elf}\" verify exit"
fi

"${OPENOCD_EXE}" \
    -s "${OPENOCD_SCRIPTS}" \
    -f "${CONFIG_FILE}" \
    -c "tcl port disabled" \
    -c "gdb port disabled" \
    -c "${prog_cmd}"

echo "OK: flashed ${resolved_elf}"

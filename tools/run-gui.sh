#!/usr/bin/env bash
# Launch the Rust host. Build it first if the release binary is missing.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=yhjm-env.sh
. "${SCRIPT_DIR}/yhjm-env.sh"
yhjm_import_env

host="${YHJM_ROOT}/host-rs/target/release/yfoc-host"
if [[ ! -x "${host}" ]]; then
    if ! command -v cargo >/dev/null 2>&1; then
        echo "Rust host is not built, and cargo was not found." >&2
        echo "Install Rust from https://rustup.rs, then run:" >&2
        echo "  cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host" >&2
        exit 1
    fi
    echo "Building ${host}"
    cargo build --release --locked --manifest-path "${YHJM_ROOT}/host-rs/Cargo.toml" -p yfoc-host
fi

exec "${host}" "$@"

#!/usr/bin/env bash
# Clones and builds the pinned picoquic + picotls pair for source-tree builds.
# Usage: eval "$(scripts/setup_picoquic_deps.sh)"
# Prints export lines for ROQR_PICOQUIC_SOURCE_DIR and ROQR_PICOTLS_PREFIX.
set -euo pipefail

PICOQUIC_REPO="https://github.com/private-octopus/picoquic"
PICOQUIC_REF="55b473e207e436d06ea9a2895cc1fc555d42c81c"
PICOTLS_REPO="https://github.com/h2o/picotls.git"
PICOTLS_REF="7c32032f91449d695b24b82955f20d04d47e6cff"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${ROOT}/.deps"
mkdir -p "${DEPS}"

fetch_ref() { # dir repo ref
    local dir="$1" repo="$2" ref="$3"
    if [ ! -d "${dir}/.git" ]; then
        git init -q "${dir}"
        git -C "${dir}" remote add origin "${repo}"
    fi
    if ! git -C "${dir}" cat-file -e "${ref}^{commit}" 2>/dev/null; then
        git -C "${dir}" fetch -q --depth 1 origin "${ref}"
    fi
    git -C "${dir}" checkout -q "${ref}"
}

fetch_ref "${DEPS}/picotls" "${PICOTLS_REPO}" "${PICOTLS_REF}" >&2
git -C "${DEPS}/picotls" submodule update --init --recursive -q >&2
if [ ! -f "${DEPS}/picotls/build/libpicotls-core.a" ]; then
    # PIC is required: the picotls static libs get linked into the SHARED
    # libroqr-ffi.so. Without it, ld rejects picotls's local-exec TLS
    # relocations (R_X86_64_TPOFF32) when building the shared object. picoquic
    # itself gets PIC via the root project's CMAKE_POSITION_INDEPENDENT_CODE.
    cmake -S "${DEPS}/picotls" -B "${DEPS}/picotls/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON >&2
    cmake --build "${DEPS}/picotls/build" --parallel \
        --target picotls-core picotls-openssl picotls-minicrypto >&2
fi

fetch_ref "${DEPS}/picoquic" "${PICOQUIC_REPO}" "${PICOQUIC_REF}" >&2

echo "export ROQR_PICOQUIC_SOURCE_DIR=${DEPS}/picoquic"
echo "export ROQR_PICOTLS_PREFIX=${DEPS}/picotls/build"

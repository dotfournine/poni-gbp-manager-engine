#!/usr/bin/env bash
# Runs inside the toolchain container (see deploy/docker-compose.dev.yml).
# Don't call it directly; use engine/scripts/linux.sh.
#
#   in-container.sh test    configure, build and run every test with the linux-clang preset
#   in-container.sh serve   migrate and seed with poni-tools, then run the engine built by `test`
#   in-container.sh shell   interactive shell
#   in-container.sh ai-live run the adversarial suite against the real model
set -euo pipefail

build="/build/linux-clang"

case "$(uname -m)" in
  aarch64) export PONI_VCPKG_ARCH=arm64 ;;
  x86_64)  export PONI_VCPKG_ARCH=x64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
mkdir -p "${VCPKG_DEFAULT_BINARY_CACHE}"

# poni-tools (D-079) from the mounted repository. /src is read-only, so its
# environment lives on the /build volume, installed exactly as locked. ctest
# runs it too, to prepare the database the integration tests start from.
export UV_PROJECT_ENVIRONMENT=/build/tools-venv
tools() {
  uv run --project /src/tools --locked --no-dev poni-tools "$@"
}

case "${1:-test}" in
  test)
    # /src is read-only, so the build tree goes to the /build volume.
    cmake --preset linux-clang -B "${build}"
    cmake --build "${build}" --parallel "${PONI_BUILD_JOBS:-4}"
    ctest --test-dir "${build}" --output-on-failure
    ;;
  serve)
    tools migrate
    tools seed
    exec "${build}/poni-gbp-engine"
    ;;
  shell)
    exec bash
    ;;
  ai-live)
    # The adversarial suite against the real model. Never part of `test`.
    cmake --preset linux-clang -B "${build}"
    cmake --build "${build}" --target poni_ai_live --parallel "${PONI_BUILD_JOBS:-4}"
    exec "${build}/tests/poni_ai_live"
    ;;
  *)
    echo "usage: in-container.sh test|serve|shell|ai-live" >&2
    exit 2
    ;;
esac

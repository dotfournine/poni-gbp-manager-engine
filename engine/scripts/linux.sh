#!/usr/bin/env bash
# Builds, tests or runs the engine in the Linux toolchain container. CI runs
# exactly this, so a green run here means the same commands pass there.
#
#   engine/scripts/linux.sh test                            build + all tests with the linux-clang preset
#   engine/scripts/linux.sh serve                           run the built engine on 127.0.0.1:8081
#   engine/scripts/linux.sh contract                        run the built engine and check it against api/openapi.yaml
#   engine/scripts/linux.sh shell                           shell in the container
#   engine/scripts/linux.sh ai-live                         run the adversarial suite against the real model
#
# `serve` and `contract` use the binary that `test` built. `contract` needs
# Node.js and `npm ci` in api/ on the host.
#
# `serve` and `contract` run against their own database, poni_linux, with
# development sign-in on: the contract check signs in and writes test rows,
# which must never land in a database that holds real reviews. The tests use
# databases of their own on the development server, as they do natively.
#
# Environment:
#   PONI_SKIP_IMAGE_BUILD=1   use the existing poni-gbp-toolchain image (CI builds it with a cache)
#   PONI_VCPKG_CACHE=<dir>    host directory for vcpkg's binary cache (default: a Docker volume)
#   PONI_LINUX_HTTP_PORT      host port for `serve` (default 8081)
#   PONI_LLM_DASHSCOPE_BASE_URL, PONI_LLM_DASHSCOPE_API_KEY   for `ai-live`: the provider's address and key (it calls the LLM,
#                             about 120 requests). PONI_AI_LIVE_MODEL picks another model
#   PONI_BUILD_JOBS=<n>       parallel compiles (default 4). If the compiler is "Killed", the
#                             container ran out of memory: use 2, or give Docker more than 4 GB
set -euo pipefail

command="${1:-test}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
compose=(docker compose -f "${root}/deploy/docker-compose.dev.yml" --profile linux)

if [[ "${PONI_SKIP_IMAGE_BUILD:-0}" != "1" ]]; then
  "${compose[@]}" build engine-linux
fi

# Creates the poni_linux database on first use.
ensure_linux_database() {
  "${compose[@]}" up -d --wait db >/dev/null
  "${compose[@]}" exec -T db sh -c \
    "psql -U poni -d poni -tAc \"SELECT 1 FROM pg_database WHERE datname = 'poni_linux'\" | grep -q 1 || createdb -U poni poni_linux"
}

case "${command}" in
  test)  exec "${compose[@]}" run --rm -T engine-linux test ;;
  serve)
    ensure_linux_database
    exec "${compose[@]}" run --rm -T --service-ports engine-linux serve
    ;;
  contract)
    port="${PONI_LINUX_HTTP_PORT:-8081}"
    name="poni-gbp-engine-contract"
    docker rm -f "${name}" >/dev/null 2>&1 || true
    ensure_linux_database
    "${compose[@]}" run -d -T --service-ports --name "${name}" engine-linux serve >/dev/null
    trap 'docker rm -f "${name}" >/dev/null 2>&1 || true' EXIT
    for _ in $(seq 1 30); do
      curl -fs -o /dev/null "http://127.0.0.1:${port}/api/health" && break
      sleep 1
    done
    # The database is migrated and seeded by the time the engine answers. Made-up rows give the
    # check a location and reviews to validate (there is no Google here).
    "${compose[@]}" exec -T db psql -q -U poni -d poni_linux -v ON_ERROR_STOP=1 < "${root}/api/scripts/contract-seed.sql"
    status=0
    PONI_API_BASE_URL="http://127.0.0.1:${port}" PONI_CONTRACT_REQUIRE_DATA=1 \
      npm --prefix "${root}/api" run -s check:contract || status=$?
    if [[ ${status} -ne 0 ]]; then docker logs "${name}" 2>&1 | tail -n 40; fi
    exit "${status}"
    ;;
  shell) exec "${compose[@]}" run --rm engine-linux shell ;;
  ai-live) exec "${compose[@]}" run --rm -T engine-linux ai-live ;;
  *) echo "usage: linux.sh test|serve|contract|shell|ai-live" >&2; exit 2 ;;
esac

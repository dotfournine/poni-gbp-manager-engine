#!/usr/bin/env bash
# Builds three production images (engine, tools, backup) for both x86_64
# and arm64 (D-034), so the same tag runs on either kind of VM.
#
#   deploy/build-images.sh                    build for this machine only, into the local Docker
#   deploy/build-images.sh --push PREFIX      build both architectures and push
#
# PREFIX is the registry path the image names start with, for example
#   us-docker.pkg.dev/xvisionmedia-gbp/poni/poni-gbp
# which gives ...-engine, ...-tools and ...-backup (the fourth, ...-web, is
# built in the dashboard's repository with the same prefix). Pushing needs `docker login`
# for that registry first (for Artifact Registry: `gcloud auth configure-docker
# us-docker.pkg.dev`). The tag is the git commit, so a deployment always names
# an exact build; set PONI_IMAGE_TAG to override.
#
# The engine build for the other architecture runs under emulation and is
# slow the first time (the C++ dependencies take about 25 minutes).
# PONI_BUILD_JOBS sets the number of parallel compiles (default 4); use 2 if
# Docker has 4 GB of memory or less.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="${PONI_IMAGE_TAG:-$(git -C "${root}" rev-parse --short=12 HEAD)}"

push=0
prefix="poni-gbp"
if [[ "${1:-}" == "--push" ]]; then
  push=1
  prefix="${2:?usage: build-images.sh --push PREFIX}"
elif [[ $# -gt 0 ]]; then
  echo "usage: build-images.sh [--push PREFIX]" >&2
  exit 2
fi

if [[ ${push} -eq 1 ]]; then
  if [[ -n "$(git -C "${root}" status --porcelain)" ]]; then
    echo "refusing to push images built from uncommitted changes" >&2
    exit 1
  fi
  # A multi-platform build can only go to a registry, not into the local Docker.
  output=(--platform linux/amd64,linux/arm64 --push)
else
  output=(--load)
fi

build() {  # name, context, dockerfile, extra arguments
  local name="$1" context="$2" dockerfile="$3"
  shift 3
  docker buildx build "${output[@]}" "$@" -f "${dockerfile}" -t "${prefix}-${name}:${tag}" "${context}"
}

build engine "${root}/engine" "${root}/engine/Dockerfile" --build-arg "BUILD_JOBS=${PONI_BUILD_JOBS:-4}"
build tools "${root}" "${root}/tools/Dockerfile"
build backup "${root}/deploy/backup" "${root}/deploy/backup/Dockerfile"

echo
echo "Built ${prefix}-{engine,tools,backup}:${tag}"
echo "In the server's poni.env set:"
echo "  PONI_IMAGE_PREFIX=${prefix}"
echo "  PONI_IMAGE_TAG=${tag}"

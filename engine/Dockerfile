# syntax=docker/dockerfile:1
# Linux toolchain and production image for the engine.
#
#   toolchain  Ubuntu + clang (apt.llvm.org) + CMake + Ninja + vcpkg + uv.
#              Used by engine/scripts/linux.sh and by CI to build and test.
#   build      Compiles the engine (no tests) for the runtime image.
#   runtime    The image that is deployed: the binary, and the migrations it checks at start.
#
#   docker build -t poni-gbp-engine engine

ARG UBUNTU=26.04

# ---- toolchain -----------------------------------------------------------------
FROM ubuntu:${UBUNTU} AS toolchain
ARG LLVM=23
# Must match "builtin-baseline" in vcpkg.json.
ARG VCPKG_COMMIT=ec48c11eae9448c8c6e7739d93e06fa2433aa95f
ENV DEBIAN_FRONTEND=noninteractive
# build-essential brings libstdc++, which clang uses on Linux.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl ca-certificates gnupg \
        zip unzip tar pkg-config python3 python3-venv perl bison flex \
        autoconf automake autoconf-archive libtool linux-libc-dev tzdata \
    && rm -rf /var/lib/apt/lists/*
# Clang from apt.llvm.org, to match Homebrew LLVM on macOS.
RUN . /etc/os-release \
    && install -d /etc/apt/keyrings \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc \
    && echo "deb [signed-by=/etc/apt/keyrings/llvm.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends clang-${LLVM} lld-${LLVM} \
    && ln -sf /usr/bin/clang-${LLVM} /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM} /usr/local/bin/clang++ \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout --quiet ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
# VCPKG_FORCE_SYSTEM_BINARIES: vcpkg on arm64 Linux must use the system cmake and ninja.
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg/archives
# uv runs poni-tools (tools/, D-079): the tests and `serve` migrate and seed
# with the same scripts production uses. Same version as tools/Dockerfile.
COPY --from=ghcr.io/astral-sh/uv:0.13.5 /uv /usr/local/bin/uv
ENV UV_LINK_MODE=copy

# ---- build ---------------------------------------------------------------------
FROM toolchain AS build
ARG VCPKG_MAX_CONCURRENCY=4
ENV VCPKG_MAX_CONCURRENCY=${VCPKG_MAX_CONCURRENCY}
# Parallel compiles. Ninja's default (one per CPU and two more) needs more
# memory than a small Docker VM has.
ARG BUILD_JOBS=4
WORKDIR /src/engine
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY triplets triplets
COPY src src
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    case "$(uname -m)" in aarch64) arch=arm64 ;; x86_64) arch=x64 ;; *) echo "unsupported architecture" >&2; exit 1 ;; esac \
    && export PONI_VCPKG_ARCH=$arch \
    && mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE" \
    && cmake --preset linux-clang -DPONI_BUILD_TESTS=OFF \
    && cmake --build --preset linux-clang --target poni-gbp-engine --parallel ${BUILD_JOBS} \
    && cp build/linux-clang/poni-gbp-engine /usr/local/bin/

# ---- runtime -------------------------------------------------------------------
FROM ubuntu:${UBUNTU} AS runtime
# ca-certificates: the engine loads the OS CA bundle for outbound HTTPS.
# tzdata: posting windows are in each location's local time (D-071).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin poni
COPY --from=build /usr/local/bin/poni-gbp-engine /usr/local/bin/
COPY migrations /usr/local/share/poni-gbp/migrations
ENV PONI_MIGRATIONS_DIR=/usr/local/share/poni-gbp/migrations \
    PONI_HTTP_HOST=0.0.0.0
USER poni
EXPOSE 8080
# The daemon: it takes no command (D-079).
ENTRYPOINT ["/usr/local/bin/poni-gbp-engine"]

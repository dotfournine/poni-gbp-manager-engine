# syntax=docker/dockerfile:1
# The tools image: poni-tools (D-079) with the migrations. In production it
# runs the one-shot `migrate` and `seed` services before the engine starts.
# Build context is the repository root:
#
#   docker build -f tools/Dockerfile -t poni-gbp-tools .

FROM python:3.14-slim
COPY --from=ghcr.io/astral-sh/uv:0.13.5 /uv /usr/local/bin/uv
ENV UV_COMPILE_BYTECODE=1 UV_LINK_MODE=copy UV_PYTHON_DOWNLOADS=never
WORKDIR /opt/poni-tools
# Dependencies first, exactly as locked, so a change to the scripts reuses this layer.
COPY tools/pyproject.toml tools/uv.lock ./
RUN uv sync --locked --no-dev --no-install-project
COPY tools/src src
RUN uv sync --locked --no-dev --no-editable \
    && useradd --system --no-create-home --shell /usr/sbin/nologin poni
COPY engine/migrations /usr/local/share/poni-gbp/migrations
ENV PATH=/opt/poni-tools/.venv/bin:$PATH \
    PONI_MIGRATIONS_DIR=/usr/local/share/poni-gbp/migrations
USER poni
ENTRYPOINT ["poni-tools"]

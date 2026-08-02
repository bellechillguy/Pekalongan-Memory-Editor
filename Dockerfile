FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake \
        g++ \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY CMakeLists.txt ./
COPY src ./src
COPY demo ./demo
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel

FROM python:3.11-slim AS python_runtime

FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libasound2t64 \
        libgl1 \
        libx11-6 \
        libxcursor1 \
        libxi6 \
        libxinerama1 \
        libxrandr2 \
        libgl1-mesa-dri \
        mesa-utils \
        x11vnc \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=python_runtime /usr/local /usr/local
COPY --from=builder /source/build/memory_editor ./build/memory_editor
COPY --from=builder /source/build/other_target ./build/other_target
COPY --from=builder /source/build/scanner_tests ./build/scanner_tests
COPY --from=builder /source/build/integration_test ./build/integration_test
COPY gui ./gui
COPY --chmod=0755 scripts/run-vnc.sh ./scripts/run-vnc.sh

CMD ["./build/memory_editor"]

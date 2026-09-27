# SplitFlap Emulator -- the SplitFlapGateway firmware and SplitFlapUniversalFirmware modules,
# compiled natively and wired together over an emulated RS-485 bus, plus the emulator's own
# control panel and virtual wall.
#
# Stage 1 builds the two native binaries; stage 2 is the slim runtime image.

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY common ./common
COPY vendor ./vendor
COPY module ./module
COPY gateway ./gateway
RUN make -C module -j"$(nproc)" && make -C gateway -j"$(nproc)" \
 && strip module/build/sfmodule gateway/build/sfgateway

FROM python:3.12-slim
LABEL org.opencontainers.image.title="SplitFlap Emulator" \
      org.opencontainers.image.description="Emulates a Split-Flap Gateway and a wall of SplitFlapUniversalFirmware modules, RS-485 protocol and timing included" \
      org.opencontainers.image.licenses="CC-BY-NC-SA-4.0"
RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY emulator/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY emulator ./emulator
COPY --from=build /src/module/build/sfmodule /src/gateway/build/sfgateway /app/bin/
ENV SFEMU_DATA=/data \
    SFEMU_BIN=/app/bin \
    SFEMU_PORT=8090 \
    SFEMU_GATEWAY_PORT=80 \
    SFEMU_GATEWAY_URL=http://localhost:8080 \
    PYTHONUNBUFFERED=1
VOLUME ["/data"]
EXPOSE 80 8090
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s CMD curl -fsS http://localhost:8090/api/emu/state >/dev/null || exit 1
WORKDIR /app/emulator
CMD ["python", "-m", "sfemu"]

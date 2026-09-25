# AVR32 (UC3) build environment for the UC3-A3 Xplained.
# The Atmel toolchain only exists as an x86_64 Linux binary, so this image is
# always built as linux/amd64 (Docker Desktop runs it via Rosetta on Apple Silicon).
FROM --platform=linux/amd64 ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        make unzip curl ca-certificates git python3 file \
        libncurses5 libexpat1 \
    && rm -rf /var/lib/apt/lists/*

# Atmel AVR 32-bit GNU Toolchain 3.4.3 + device headers 6.2.0.742.
# Official source: https://www.microchip.com/en-us/development-tool/atmel-avr-toolchain-for-linux
# These URLs point at a community mirror (monome) of the same Atmel files, because
# Microchip's download links are not stable enough for an automated build.
ARG MIRROR=https://github.com/monome/avr32-toolchain-linux/raw/master
ARG TOOLCHAIN=avr32-gnu-toolchain-3.4.3.820-linux.any.x86_64.tar.gz
ARG HEADERS=avr32-headers-6.2.0.742.zip

RUN set -eux; cd /tmp; \
    curl -fsSL -o toolchain.tar.gz "$MIRROR/$TOOLCHAIN"; \
    curl -fsSL -o headers.zip      "$MIRROR/$HEADERS"; \
    mkdir -p /opt/avr32; \
    tar -xzf toolchain.tar.gz -C /opt/avr32 --strip-components=1; \
    unzip -q headers.zip -d /opt/avr32/avr32/include; \
    rm -f toolchain.tar.gz headers.zip; \
    /opt/avr32/bin/avr32-gcc --version

ENV PATH="/opt/avr32/bin:${PATH}"
WORKDIR /workspace

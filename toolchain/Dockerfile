# Ambiente di cross-compilazione per TrimUI Smart Pro (TG5040) e Brick Pro (TG4040).
# Usa il kit ufficiale TrimUI (github.com/trimui/toolchain_sdk_smartpro, release 20231018):
# compilatore Linaro GCC 7.5 aarch64 (glibc 2.25) e le librerie del firmware (/usr).
# Il compilatore è un programma x86-64: su Mac Apple gira con l'emulazione di Docker.
#
# Costruzione (dalla cartella trimui):
#   docker build --platform linux/amd64 -t trimui-tg5040 -f toolchain/Dockerfile deps/trimui-sdk/dl
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        make cmake git python3 pkg-config file ca-certificates curl xz-utils bzip2 patch \
        autoconf automake libtool perl zip \
    && rm -rf /var/lib/apt/lists/*

# ADD estrae da solo gli archivi .tgz
ADD aarch64-linux-gnu-7.5.0-linaro.tgz /opt/
ADD SDK_usr_tg5040_a133p.tgz /opt/sdk/

ENV CROSS=aarch64-linux-gnu- \
    PATH=/opt/aarch64-linux-gnu-7.5.0-linaro/bin:$PATH \
    SDK=/opt/sdk/usr \
    PKG_CONFIG_LIBDIR=/opt/sdk/usr/lib/pkgconfig \
    PKG_CONFIG_SYSROOT_DIR=/opt/sdk

WORKDIR /work

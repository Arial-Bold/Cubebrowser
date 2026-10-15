#!/bin/sh
# Downloads the libraries the build needs into third_party/:
#   mbedTLS 3.6 LTS (TLS/HTTPS) and stb_image (JPEG/PNG/GIF decoding).
# Use the mbedTLS release tarball, not a git clone: it includes generated files.
set -e
VER=3.6.2
cd "$(dirname "$0")"
mkdir -p third_party/stb
if [ ! -f third_party/mbedtls/library/ssl_tls.c ]; then
    URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2"
    echo "Downloading $URL"
    curl -fL -o third_party/mbedtls.tar.bz2 "$URL"
    tar -xjf third_party/mbedtls.tar.bz2 -C third_party
    mv "third_party/mbedtls-$VER" third_party/mbedtls
    rm third_party/mbedtls.tar.bz2
fi
if [ ! -f third_party/stb/stb_image.h ]; then
    echo "Downloading stb_image.h"
    curl -fL -o third_party/stb/stb_image.h "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
fi
echo "Done. Now run: make"

#!/bin/sh
# Downloads the mbedTLS 3.6 LTS release into third_party/mbedtls.
# Use a release tarball, not a git clone: the tarball includes generated files.
set -e
VER=3.6.2
cd "$(dirname "$0")"
mkdir -p third_party
if [ -f third_party/mbedtls/library/ssl_tls.c ]; then
    echo "mbedTLS already present in third_party/mbedtls"
    exit 0
fi
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2"
echo "Downloading $URL"
curl -L -o third_party/mbedtls.tar.bz2 "$URL"
tar -xjf third_party/mbedtls.tar.bz2 -C third_party
mv "third_party/mbedtls-$VER" third_party/mbedtls
rm third_party/mbedtls.tar.bz2
echo "Done. Now run: make"

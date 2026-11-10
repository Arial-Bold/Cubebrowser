#!/bin/sh
# Downloads the libraries the build needs into third_party/:
#   mbedTLS 3.6 LTS (TLS/HTTPS), stb_image (JPEG/PNG/GIF decoding) and
#   QuickJS (JavaScript, a fixed version; only the engine's own files).
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
if [ ! -f third_party/quickjs/quickjs.c ]; then
    # QuickJS by Fabrice Bellard and Charlie Gordon (MIT licence), release 2026-06-04
    QJS=3d5e064e9dd67c70f7962836505a7fa067bf0a4e
    URL="https://github.com/bellard/quickjs/archive/$QJS.tar.gz"
    echo "Downloading $URL"
    curl -fL -o third_party/quickjs.tar.gz "$URL"
    rm -rf third_party/quickjs-src
    mkdir -p third_party/quickjs-src third_party/quickjs
    tar -xzf third_party/quickjs.tar.gz -C third_party/quickjs-src --strip-components=1
    for f in quickjs.c quickjs.h quickjs-atom.h quickjs-opcode.h libregexp.c libregexp.h libregexp-opcode.h \
             libunicode.c libunicode.h libunicode-table.h cutils.c cutils.h dtoa.c dtoa.h list.h LICENSE; do
        cp "third_party/quickjs-src/$f" third_party/quickjs/
    done
    # the GameCube has no OS threads for Atomics, and no malloc_usable_size()
    sed -i 's/^#define CONFIG_ATOMICS$/#if !defined(__GEKKO__)\n#define CONFIG_ATOMICS\n#endif/; s/^#elif defined(__EMSCRIPTEN__)$/#elif defined(__EMSCRIPTEN__) || defined(__GEKKO__)/' third_party/quickjs/quickjs.c
    grep -q 'defined(__GEKKO__)' third_party/quickjs/quickjs.c || { echo "Couldn't adapt QuickJS for the GameCube"; exit 1; }
    rm -rf third_party/quickjs-src third_party/quickjs.tar.gz
fi
echo "Done. Now run: make"

#!/bin/sh
# Build the curl CLI used by NextUI's HTTP layer without relying on the stock OS.
set -eu

: "${CROSS_COMPILE:?CROSS_COMPILE is required}"

CURL_VERSION=8.21.0
CURL_SHA256=aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
OPENSSL_VERSION=3.5.7
OPENSSL_SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
MUSL_VERSION=1.2.6
MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ARCHIVE="$SCRIPT_DIR/curl-$CURL_VERSION.tar.xz"
SOURCE_DIR="$SCRIPT_DIR/curl-$CURL_VERSION"
MUSL_ARCHIVE="$SCRIPT_DIR/musl-$MUSL_VERSION.tar.gz"
MUSL_SOURCE_DIR="$SCRIPT_DIR/musl-$MUSL_VERSION"
MUSL_PREFIX="$SCRIPT_DIR/musl-prefix"
MUSL_STAMP="$MUSL_PREFIX/.musl-$MUSL_VERSION-$MUSL_SHA256"
MUSL_CC="$MUSL_PREFIX/bin/musl-gcc"
OPENSSL_ARCHIVE="$SCRIPT_DIR/openssl-$OPENSSL_VERSION.tar.gz"
OPENSSL_SOURCE_DIR="$SCRIPT_DIR/openssl-$OPENSSL_VERSION"
OPENSSL_PREFIX="$SCRIPT_DIR/openssl-prefix"
OPENSSL_STAMP="$OPENSSL_PREFIX/.openssl-$OPENSSL_VERSION-$OPENSSL_SHA256-musl-$MUSL_VERSION-$MUSL_SHA256"
OUTPUT_DIR="$SCRIPT_DIR/output"
STAMP="$OUTPUT_DIR/.curl-$CURL_VERSION-$CURL_SHA256-openssl-$OPENSSL_VERSION-$OPENSSL_SHA256-musl-$MUSL_VERSION-$MUSL_SHA256"

if [ -x "$OUTPUT_DIR/curl" ] && [ -s "$OUTPUT_DIR/ca-certificates.crt" ] && [ -f "$STAMP" ]; then
	exit 0
fi

mkdir -p "$OUTPUT_DIR"
if [ ! -f "$ARCHIVE" ]; then
	wget -q "https://curl.se/download/curl-$CURL_VERSION.tar.xz" -O "$ARCHIVE.tmp"
	mv "$ARCHIVE.tmp" "$ARCHIVE"
fi
if ! echo "$CURL_SHA256  $ARCHIVE" | sha256sum -c -; then
	rm -f "$ARCHIVE"
	exit 1
fi

if [ ! -f "$MUSL_ARCHIVE" ]; then
	wget -q "https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz" -O "$MUSL_ARCHIVE.tmp"
	mv "$MUSL_ARCHIVE.tmp" "$MUSL_ARCHIVE"
fi
if ! echo "$MUSL_SHA256  $MUSL_ARCHIVE" | sha256sum -c -; then
	rm -f "$MUSL_ARCHIVE"
	exit 1
fi

if [ ! -f "$MUSL_STAMP" ] || [ ! -x "$MUSL_CC" ] || [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
	chmod -R u+w "$MUSL_PREFIX" 2>/dev/null || true
	rm -rf "$MUSL_SOURCE_DIR" "$MUSL_PREFIX"
	tar -xzf "$MUSL_ARCHIVE" -C "$SCRIPT_DIR"
	cd "$MUSL_SOURCE_DIR"
	CROSS_COMPILE="$CROSS_COMPILE" \
	./configure --target=aarch64 --prefix="$MUSL_PREFIX" >/dev/null
	make -j"$(nproc)" >/dev/null
	make install >/dev/null
	touch "$MUSL_STAMP"
fi

if [ ! -f "$OPENSSL_ARCHIVE" ]; then
	wget -q "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" -O "$OPENSSL_ARCHIVE.tmp"
	mv "$OPENSSL_ARCHIVE.tmp" "$OPENSSL_ARCHIVE"
fi
if ! echo "$OPENSSL_SHA256  $OPENSSL_ARCHIVE" | sha256sum -c -; then
	rm -f "$OPENSSL_ARCHIVE"
	exit 1
fi

if [ ! -f "$OPENSSL_STAMP" ] || [ ! -f "$OPENSSL_PREFIX/lib/libssl.a" ] || [ ! -f "$OPENSSL_PREFIX/lib/libcrypto.a" ]; then
	rm -rf "$OPENSSL_SOURCE_DIR" "$OPENSSL_PREFIX"
	tar -xzf "$OPENSSL_ARCHIVE" -C "$SCRIPT_DIR"
	cd "$OPENSSL_SOURCE_DIR"
	REALGCC="${CROSS_COMPILE}gcc" \
	CROSS_COMPILE= CC="$MUSL_CC" AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" \
	./Configure linux-aarch64 \
		--prefix="$OPENSSL_PREFIX" \
		--libdir=lib \
		no-shared no-tests no-docs no-apps no-module no-dso \
		no-secure-memory no-afalgeng no-ktls >/dev/null
	REALGCC="${CROSS_COMPILE}gcc" CROSS_COMPILE= make -j"$(nproc)" >/dev/null
	REALGCC="${CROSS_COMPILE}gcc" CROSS_COMPILE= make install_sw >/dev/null
	touch "$OPENSSL_STAMP"
fi

rm -rf "$SOURCE_DIR"
tar -xJf "$ARCHIVE" -C "$SCRIPT_DIR"

cd "$SOURCE_DIR"
# NextUI's HTTP client does not request compressed transfer encoding, so zlib
# is deliberately omitted instead of linking the glibc SDK's archive into musl.
REALGCC="${CROSS_COMPILE}gcc" \
CC="$MUSL_CC" \
AR="${CROSS_COMPILE}ar" \
RANLIB="${CROSS_COMPILE}ranlib" \
STRIP="${CROSS_COMPILE}strip" \
PKG_CONFIG=false \
PKG_CONFIG_SYSROOT_DIR= \
CPPFLAGS="-I$OPENSSL_PREFIX/include" \
LDFLAGS="-L$OPENSSL_PREFIX/lib" \
./configure \
	--host=aarch64-linux-musl \
	--disable-shared \
	--enable-static \
	--with-openssl="$OPENSSL_PREFIX" \
	--without-zlib \
	--with-ca-bundle=/mnt/SDCARD/.system/h700/etc/ssl/certs/ca-certificates.crt \
	--without-ca-path \
	--without-libpsl \
	--without-brotli \
	--without-zstd \
	--without-libidn2 \
	--without-nghttp2 \
	--disable-ldap \
	--disable-ldaps \
	--disable-rtsp \
	--disable-dict \
	--disable-telnet \
	--disable-tftp \
	--disable-pop3 \
	--disable-imap \
	--disable-smb \
	--disable-smtp \
	--disable-gopher \
	--disable-mqtt \
	--disable-manual >/dev/null
REALGCC="${CROSS_COMPILE}gcc" \
make -j"$(nproc)" LDFLAGS="-all-static -L$OPENSSL_PREFIX/lib" >/dev/null

"${CROSS_COMPILE}strip" src/curl
cp src/curl "$OUTPUT_DIR/curl"
cp /etc/ssl/certs/ca-certificates.crt "$OUTPUT_DIR/ca-certificates.crt"
chmod 755 "$OUTPUT_DIR/curl"

file "$OUTPUT_DIR/curl" | grep -q 'ARM aarch64'
file "$OUTPUT_DIR/curl" | grep -q 'statically linked'
test -s "$OUTPUT_DIR/ca-certificates.crt"
rm -f "$OUTPUT_DIR"/.curl-*
touch "$STAMP"

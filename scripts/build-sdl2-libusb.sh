#!/bin/bash
# Build an x86_64 SDL2 (matching the GPTK Wine bundle's SDL 2.32.10) with the libusb HIDAPI backend
# statically linked in, so SDL's Xbox One (GIP) driver can see vendor-class USB controllers on macOS.
# Output: build/x86_64/lib/libSDL2-2.0.0.dylib
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
PREFIX="$ROOT/build/x86_64"
ARCH=x86_64
MINVER=11.0
JOBS=$(sysctl -n hw.ncpu)
SDL_SRC="$TP/SDL2-2.32.10"
USB_SRC="$TP/libusb-1.0.29"
export MACOSX_DEPLOYMENT_TARGET=$MINVER
mkdir -p "$PREFIX" "$TP"

# Fetch pristine sources if missing (they are not committed).
[ -f "$TP/SDL2-2.32.10.tar.gz" ] || curl -sL -o "$TP/SDL2-2.32.10.tar.gz" https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
[ -f "$TP/libusb-1.0.29.tar.bz2" ] || curl -sL -o "$TP/libusb-1.0.29.tar.bz2" https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2
[ -d "$SDL_SRC" ] || tar xzf "$TP/SDL2-2.32.10.tar.gz" -C "$TP"
[ -d "$USB_SRC" ] || tar xjf "$TP/libusb-1.0.29.tar.bz2" -C "$TP"

echo "== libusb (static, $ARCH)"
mkdir -p "$ROOT/build/libusb-$ARCH" && cd "$ROOT/build/libusb-$ARCH"
if [ ! -f Makefile ]; then
  "$USB_SRC/configure" --host=$ARCH-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static --disable-udev \
    CC="clang -arch $ARCH" CFLAGS="-O2 -mmacosx-version-min=$MINVER" LDFLAGS="-arch $ARCH" >configure.log 2>&1
fi
make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1
file "$PREFIX/lib/libusb-1.0.a" | head -1

echo "== SDL2 (shared, $ARCH, hidapi+libusb)"
# Apply local patches (if any) to a pristine copy of the SDL source.
SDL_WORK="$ROOT/build/SDL2-src-$ARCH"
if [ ! -d "$SDL_WORK" ]; then cp -R "$SDL_SRC" "$SDL_WORK"; fi
for p in "$ROOT"/patches/sdl2-*.patch; do
  [ -f "$p" ] || continue
  if ! patch -d "$SDL_WORK" -p1 -N --dry-run < "$p" >/dev/null 2>&1; then echo "  (patch $(basename "$p") already applied or failed dry-run)"; else patch -d "$SDL_WORK" -p1 -N < "$p"; fi
done
cd "$ROOT/build" && mkdir -p "sdl2-$ARCH" && cd "sdl2-$ARCH"
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" cmake "$SDL_WORK" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_OSX_DEPLOYMENT_TARGET=$MINVER -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
  -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=ON -DSDL_HIDAPI_LIBUSB_SHARED=OFF \
  -DLibUSB_INCLUDE_DIR="$PREFIX/include/libusb-1.0" -DLibUSB_LIBRARY="$PREFIX/lib/libusb-1.0.a" \
  -DCMAKE_MACOSX_RPATH=ON -DCMAKE_INSTALL_NAME_DIR=@rpath >cmake.log 2>&1
cmake --build . -j"$JOBS" >build.log 2>&1
cmake --install . >install.log 2>&1
DYLIB="$PREFIX/lib/libSDL2-2.0.0.dylib"
echo "== result: $DYLIB"; file "$DYLIB"; otool -L "$DYLIB" | head -4
echo -n "libusb symbols inside: "; nm -gU "$DYLIB" | grep -c 'libusb_' || true
echo -n "hidapi libusb backend strings: "; strings "$DYLIB" | grep -c 'SDL_HIDAPI_LIBUSB' || true

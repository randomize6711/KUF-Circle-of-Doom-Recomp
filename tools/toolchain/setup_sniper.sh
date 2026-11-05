#!/usr/bin/env bash
# Prepares the portable Linux build (docs/STEAM_DECK.md "Portable build"; cmake/steamrt3-sniper.cmake), in
# build/toolchains only (nothing is installed on this machine):
#   1. Valve's Steam Runtime 3 "sniper" SDK sysroot (Debian 11, glibc 2.31), pinned version and SHA-256;
#   2. GNU GCC 14.2.0's source (SHA-256), and in the sysroot (bwrap, no network, the sysroot's own GCC 14) a
#      non-bootstrap GCC built only to make a shared libstdc++.so.6 with the configuration of the SDK's GCC 14;
#   3. build/toolchains/libstdcxx-14.2.0/{include,lib}: those headers and libraries only, checked: symbol versioned,
#      no glibc above the baseline.
# Idempotent: finished steps are skipped. About 1.3 GB downloaded, 6 GB on disk, 15-30 minutes for step 2.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TC="$ROOT/build/toolchains"
val() { sed -n "s/^set($1 \(.*\))$/\1/p" "$ROOT/cmake/steamrt3-sniper-version.cmake"; }
SNIPER="$(val KUFCOD_SNIPER_VERSION)"; SNIPER_SHA="$(val KUFCOD_SNIPER_SDK_SHA256)"
GCCV="$(val KUFCOD_LIBSTDCXX_VERSION)"; GCC_SHA="$(val KUFCOD_GCC_SOURCE_SHA256)"; BASE="$(val KUFCOD_GLIBC_BASELINE)"
JOBS="${JOBS:-8}"
SR_DIR="$TC/sniper-$SNIPER"; SR="$SR_DIR/sysroot"; OUT="$TC/libstdcxx-$GCCV"
command -v bwrap >/dev/null || { echo "bwrap (bubblewrap) is needed to build in the sysroot" >&2; exit 1; }
mkdir -p "$SR_DIR"

fetch() {  # url dest sha256
  if [[ -f "$2" ]] && echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
  curl -fL --progress-bar -o "$2.part" "$1"
  echo "$3  $2.part" | sha256sum -c --quiet - || { rm -f "$2.part"; echo "checksum mismatch: $1" >&2; exit 1; }
  mv "$2.part" "$2"
}

echo "== Steam Runtime 3 (sniper) SDK $SNIPER"
if [[ ! -f "$SR/.kuf-extracted" ]]; then
  fetch "https://repo.steampowered.com/steamrt3/images/$SNIPER/com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz" \
    "$SR_DIR/sdk-sysroot.tar.gz" "$SNIPER_SHA"
  rm -rf "$SR" && mkdir -p "$SR"
  tar -xzf "$SR_DIR/sdk-sysroot.tar.gz" -C "$SR" --exclude='./dev/*'
  # Absolute symbolic links (/lib64/ld-linux-x86-64.so.2 -> /lib/...) would resolve to this machine's files when
  # the sysroot is used from outside: make them relative to the sysroot.
  python3 -I - "$SR" <<'PY'
import os, sys
root = sys.argv[1]
for d, dirs, files in os.walk(root):
    for n in dirs + files:
        p = os.path.join(d, n)
        if os.path.islink(p) and os.readlink(p).startswith("/"):
            target = os.path.join(root, os.readlink(p).lstrip("/"))
            os.unlink(p)
            os.symlink(os.path.relpath(target, d), p)
PY
  touch "$SR/.kuf-extracted"
fi
grep -q 'sniper' "$SR/usr/lib/os-release"

echo "== libstdc++ $GCCV"
if [[ ! -f "$OUT/.kuf-built" ]]; then
  fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCCV/gcc-$GCCV.tar.xz" "$TC/gcc-$GCCV.tar.xz" "$GCC_SHA"
  [[ -d "$TC/gcc-$GCCV" ]] || tar -xJf "$TC/gcc-$GCCV.tar.xz" -C "$TC"
  B="$TC/libstdcxx-build"; STAGE="$TC/libstdcxx-stage"
  rm -rf "$B" "$STAGE" "$OUT" && mkdir -p "$B" "$STAGE"
  # The configuration of the SDK's GCC 14 (g++-14 -v in the sysroot) for everything libstdc++ is built with.
  nice bwrap --bind "$SR" / --dev /dev --proc /proc --tmpfs /tmp --ro-bind "$TC/gcc-$GCCV" /src --bind "$B" /build \
    --bind "$STAGE" /opt/libstdcxx --unshare-all --die-with-parent --setenv PATH /usr/lib/gcc-14/bin:/usr/bin:/bin \
    --chdir /build /bin/sh -c "set -e
      CC=gcc CXX=g++ /src/configure --prefix=/opt/libstdcxx --build=x86_64-linux-gnu --host=x86_64-linux-gnu \
        --target=x86_64-linux-gnu --enable-languages=c,c++ --disable-bootstrap --disable-multilib --enable-shared \
        --disable-nls --enable-threads=posix --enable-clocale=gnu --enable-libstdcxx-time=yes \
        --with-default-libstdcxx-abi=new --enable-libstdcxx-backtrace --enable-gnu-unique-object \
        --disable-vtable-verify --disable-cet --disable-libstdcxx-pch --disable-libquadmath --disable-libsanitizer \
        --disable-libgomp --disable-libitm --disable-libatomic --disable-libvtv --disable-libssp --with-system-zlib \
        --enable-linker-build-id --enable-checking=release > configure.log 2>&1
      make -j$JOBS all-gcc > make-gcc.log 2>&1
      make -j$JOBS all-target-libgcc > make-libgcc.log 2>&1
      make -j$JOBS all-target-libstdc++-v3 > make-libstdcxx.log 2>&1
      make install-target-libstdc++-v3 > install.log 2>&1" ||
    { echo "the libstdc++ build failed: logs in $B" >&2; exit 1; }
  rmdir "$SR/build" "$SR/src" "$SR/opt/libstdcxx" 2>/dev/null || true  # bwrap's mount points
  mkdir -p "$OUT/lib"
  cp -a "$STAGE/include" "$OUT/include"
  cp -a "$STAGE"/lib64/libstdc++.so* "$STAGE"/lib64/libstdc++.a "$STAGE"/lib64/libstdc++exp.a \
    "$STAGE"/lib64/libsupc++.a "$OUT/lib/"
  rm -f "$OUT"/lib/*-gdb.py
  rm -rf "$B" "$STAGE"
  touch "$OUT/.kuf-built"
fi
SO="$OUT/lib/libstdc++.so.6"
NEED="$(objdump -T "$SO" | grep -oE 'GLIBC_2\.[0-9]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
HAVE="$(objdump -p "$SO" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sort -uV | tail -1)"
[[ -n "$HAVE" ]] || { echo "$SO has no symbol versions" >&2; exit 1; }
[[ "$(printf '%s\n%s\n' "$NEED" "$BASE" | sort -V | tail -1)" == "$BASE" ]] ||
  { echo "$SO needs glibc $NEED (baseline $BASE)" >&2; exit 1; }
echo "ready: sysroot $SR (glibc $(basename "$(ls "$SR"/lib/x86_64-linux-gnu/libc-2.*.so)" .so | sed 's/libc-//')), libstdc++ $HAVE (needs glibc $NEED)"

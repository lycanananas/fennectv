#!/usr/bin/env bash
# Inside the fennectv-win container (packaging/windows/build runs it):
# configures and builds /src for Windows into /src/build-win, then packs
# the programs with every DLL and Qt plugin they need into
# /src/dist/fennectv-<version>-win64.zip.
set -euo pipefail

# Only quasi-msys2's variables and wrappers: its binfmt and Wine set-up
# (env/all.src) has nothing to do in a container without Wine.
# Its scripts aren't written for `set -u`.
set +u
QUASI_MSYS2_QUIET=1 source /opt/quasi-msys2/env/vars.src
set -u

src=/src
build=/src/build-win
dist=/src/dist
export CCACHE_DIR=$build/.ccache

cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DFENNECTV_TEST_VIDEO=
cmake --build "$build"

# The MSYS2 tree, as a Linux path.
prefix="$MSYSTEM_PREFIX"
version=$(sed -n 's/^project(FennecTV VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
name="fennectv-$version-win64"
out="$dist/$name"
rm -rf "$out" "$out.zip"
mkdir -p "$out/platforms" "$out/styles"

cp "$build/fennectv-tx.exe" "$build/fennectv-rx.exe" "$out/"
cp "$prefix/share/qt6/plugins/platforms/qwindows.dll" "$out/platforms/"
for s in "$prefix"/share/qt6/plugins/styles/*.dll; do [ -e "$s" ] && cp "$s" "$out/styles/"; done

# Every DLL the programs and plugins load that MSYS2 provides, and theirs,
# down to Windows' own (which aren't in the MSYS2 tree and stay out).
declare -A seen=()
deps() {
    llvm-objdump -p "$1" 2>/dev/null | awk '/DLL Name:/ {print $3}'
}
add() {
    local dll lower found
    for dll in $(deps "$1"); do
        lower=${dll,,}
        [ -n "${seen[$lower]:-}" ] && continue
        seen[$lower]=1
        found=$(find "$prefix/bin" -maxdepth 1 -iname "$dll" -print -quit)
        [ -z "$found" ] && continue
        cp "$found" "$out/"
        add "$found"
    done
}
for f in "$out"/*.exe "$out"/platforms/*.dll "$out"/styles/*.dll; do [ -e "$f" ] && add "$f"; done

cp "$src/LICENSE" "$out/LICENSE.txt"
cat >"$out/README.txt" <<EOF
FennecTV $version for Windows (x86-64), built with MSYS2 UCRT64 libraries.
Licence: GPL-3.0-or-later (LICENSE.txt). The DLLs keep their own licences.

  fennectv-tx.exe --help
  fennectv-rx.exe --play
EOF

(cd "$dist" && cmake -E tar cf "$name.zip" --format=zip "$name")
echo
echo "$(find "$out" -name '*.dll' | wc -l) DLLs, $(du -sh "$out" | cut -f1) unpacked"
echo "dist/$name.zip"

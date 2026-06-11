#!/bin/sh
# Build the Windows .exe. The code is plain C stdlib (no POSIX), so the same
# -m32 1.3 layout compiles for win32. Uses a local i686 mingw if present, else a
# throwaway docker image so nothing has to be installed on the host.
#
#   ./build-win.sh        ->  bin/cod2-demotool.exe
#
set -e
cd "$(dirname "$0")"
mkdir -p bin
FLAGS="-g -static -Wno-write-strings -DCOD_VERSION=COD2_1_3"

if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
	echo "using local mingw"
	i686-w64-mingw32-g++ -m32 $FLAGS src/reader.cpp -o bin/cod2-demotool.exe
else
	echo "no local mingw — cross-building in docker (dockcross/windows-static-x86)"
	docker run --rm -v "$(pwd)":/work -w /work dockcross/windows-static-x86 \
		i686-w64-mingw32.static-g++ $FLAGS src/reader.cpp -o bin/cod2-demotool.exe
fi
echo "built bin/cod2-demotool.exe"
file bin/cod2-demotool.exe 2>/dev/null || true

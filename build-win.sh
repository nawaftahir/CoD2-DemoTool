#!/bin/sh
# Build the Windows .exe. The code is plain C stdlib (no POSIX), so the same
# -m32 1.3 layout compiles for win32. Uses a local i686 mingw if present, else a
# throwaway docker image so nothing has to be installed on the host.
#
#   ./build-win.sh        ->  bin/cod2-demotool.exe
#
# The release exe is built -O2 -s (stripped, no debug info) and embeds a version
# resource (src/win/version.rc) — both reduce Windows Defender heuristic false
# positives on an unsigned mingw binary. It is still unsigned, so SmartScreen will
# show "Windows protected your PC" on first download; that clears with "More info ->
# Run anyway" or by shipping/extracting it from a zip. See README "Windows trust".
set -e
cd "$(dirname "$0")"
mkdir -p bin
# -O2 -s for a clean release binary; the version resource is linked alongside.
FLAGS="-O2 -s -Wno-write-strings -DCOD_VERSION=COD2_1_3"
RC="src/win/version.rc"
RES="bin/version.o"

if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
	echo "using local mingw"
	i686-w64-mingw32-windres "$RC" -O coff -o "$RES"
	i686-w64-mingw32-g++ -m32 -static $FLAGS src/reader.cpp "$RES" -o bin/cod2-demotool.exe
else
	echo "no local mingw — cross-building in docker (dockcross/windows-static-x86)"
	docker run --rm -v "$(pwd)":/work -w /work dockcross/windows-static-x86 sh -c \
		"i686-w64-mingw32.static-windres $RC -O coff -o $RES && \
		 i686-w64-mingw32.static-g++ -static $FLAGS src/reader.cpp $RES -o bin/cod2-demotool.exe"
fi
rm -f "$RES"
echo "built bin/cod2-demotool.exe"
file bin/cod2-demotool.exe 2>/dev/null || true

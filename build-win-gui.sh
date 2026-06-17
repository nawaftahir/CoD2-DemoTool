#!/bin/sh
# Build the Windows GUI .exe. Same toolchain and 1.3 layout as build-win.sh, but the
# GUI translation unit (src/gui_win32.cpp, which #includes reader.cpp under GUI_BUILD)
# is compiled as a GUI-subsystem app (-mwindows) so double-clicking opens a window with
# no console flash. Uses a local i686 mingw if present, else the dockcross docker image.
#
#   ./build-win-gui.sh    ->  bin/cod2-demotool-gui.exe
#
# The console CLI (build-win.sh -> cod2-demotool.exe) is unchanged and stays the
# scriptable path. Same unsigned-binary trust caveats apply (see README "Windows trust").
set -e
cd "$(dirname "$0")"
mkdir -p bin
# -mwindows: GUI subsystem (no console). Same release flags as the CLI build.
FLAGS="-O2 -s -mwindows -Wno-write-strings -DCOD_VERSION=COD2_1_3"
# comdlg32 = GetOpenFileName (Browse), shell32 = DragAcceptFiles/DragQueryFile (drag-drop),
# gdi32/user32 = window + controls (usually auto-linked; listed for clarity).
LIBS="-lcomdlg32 -lshell32 -lgdi32 -luser32"
RC="src/win/version.rc"
RES="bin/version-gui.o"

if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
	echo "using local mingw"
	i686-w64-mingw32-windres "$RC" -O coff -o "$RES"
	i686-w64-mingw32-g++ -m32 -static $FLAGS src/gui_win32.cpp "$RES" $LIBS -o bin/cod2-demotool-gui.exe
else
	echo "no local mingw — cross-building in docker (dockcross/windows-static-x86)"
	docker run --rm -v "$(pwd)":/work -w /work dockcross/windows-static-x86 sh -c \
		"i686-w64-mingw32.static-windres $RC -O coff -o $RES && \
		 i686-w64-mingw32.static-g++ -static $FLAGS src/gui_win32.cpp $RES $LIBS -o bin/cod2-demotool-gui.exe"
fi
rm -f "$RES"
echo "built bin/cod2-demotool-gui.exe"
file bin/cod2-demotool-gui.exe 2>/dev/null || true

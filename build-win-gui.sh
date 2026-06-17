#!/bin/sh
# Build the Windows GUI .exe (ImGui, CoD2 menu styled). gui_imgui.cpp #includes
# reader.cpp under GUI_BUILD and drives an ImGui UI on the Win32 + OpenGL3 backend, so
# it cross-compiles with the same dockcross mingw toolchain as the CLI (no DirectX, no
# MSVC, no GLFW). Built -mwindows (GUI subsystem) so double-clicking opens a window with
# no console flash. Uses a local i686 mingw if present, else the dockcross docker image.
#
#   ./build-win-gui.sh    ->  bin/cod2-demotool-gui.exe
#
# The console CLI (build-win.sh -> cod2-demotool.exe) is unchanged. Same unsigned-binary
# trust caveats apply (see README "Windows trust").
set -e
cd "$(dirname "$0")"
mkdir -p bin

FLAGS="-O2 -s -mwindows -Wno-write-strings -DCOD_VERSION=COD2_1_3 -Isrc -Isrc/imgui -Isrc/imgui/backends"
# opengl32 = GL, comdlg32 = file dialog, shell32 = drag-drop, gdi32/user32 = window,
# imm32/dwmapi = used by imgui_impl_win32.
LIBS="-lopengl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -limm32 -ldwmapi"
RC="src/win/version.rc"
RES="bin/version-gui.o"
IMGUI="src/imgui/imgui.cpp src/imgui/imgui_draw.cpp src/imgui/imgui_tables.cpp src/imgui/imgui_widgets.cpp src/imgui/backends/imgui_impl_win32.cpp src/imgui/backends/imgui_impl_opengl3.cpp"

if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
	echo "using local mingw"
	i686-w64-mingw32-windres "$RC" -O coff -o "$RES"
	i686-w64-mingw32-g++ -m32 -static $FLAGS src/gui_imgui.cpp $IMGUI "$RES" $LIBS -o bin/cod2-demotool-gui.exe
else
	echo "no local mingw — cross-building in docker (dockcross/windows-static-x86)"
	docker run --rm -v "$(pwd)":/work -w /work dockcross/windows-static-x86 sh -c \
		"i686-w64-mingw32.static-windres $RC -O coff -o $RES && \
		 i686-w64-mingw32.static-g++ -static $FLAGS src/gui_imgui.cpp $IMGUI $RES $LIBS -o bin/cod2-demotool-gui.exe"
fi
rm -f "$RES"
echo "built bin/cod2-demotool-gui.exe"
file bin/cod2-demotool-gui.exe 2>/dev/null || true

# CoD2-DemoTool — offline CoD2 .dm_1 editor (skip dead-time / cut), all versions.
#
# Built -m32 to match the engine's struct offsets + bit-field encodings exactly
# (the netField tables index playerState_t/entityState_t by 32-bit offset).
# One binary handles protocols 115/117/118/119: the demo wire format is identical
# across versions, so we compile for the 1.3 layout (biggest buffer + all fields)
# and switch the lone CoD2x/p119 stat branch at runtime.

CXX      := g++
CXXFLAGS := -g -m32 -static -Wno-write-strings -DCOD_VERSION=COD2_1_3
SRC      := src/reader.cpp
BIN      := bin/cod2-demotool

# Windows cross-build. The code is plain C stdlib (no POSIX), so the same -m32
# 1.3 layout compiles for win32. Uses a local i686 mingw if present, else a
# throwaway docker image so nothing has to be installed on the host.
WINCXX   := i686-w64-mingw32-g++
WINBIN   := bin/cod2-demotool.exe

.PHONY: all win clean
all: $(BIN)

$(BIN): $(SRC) src/declarations.hpp src/writer.h
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

win: $(SRC) src/declarations.hpp src/writer.h
	@mkdir -p bin
	@if command -v $(WINCXX) >/dev/null 2>&1; then \
		echo "using local mingw"; \
		$(WINCXX) -g -m32 -static -Wno-write-strings -DCOD_VERSION=COD2_1_3 $(SRC) -o $(WINBIN); \
	else \
		echo "no local mingw — cross-building in docker (dockcross/windows-static-x86)"; \
		docker run --rm -v "$$(pwd)":/work -w /work dockcross/windows-static-x86 \
			i686-w64-mingw32.static-g++ -g -static -Wno-write-strings -DCOD_VERSION=COD2_1_3 $(SRC) -o $(WINBIN); \
	fi
	@echo "built $(WINBIN)"

clean:
	rm -f $(BIN) $(WINBIN)

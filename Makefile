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

.PHONY: all clean
all: $(BIN)

$(BIN): $(SRC) src/declarations.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

clean:
	rm -f $(BIN)

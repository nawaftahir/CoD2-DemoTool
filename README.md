# CoD2-DemoTool

An offline editor for Call of Duty 2 `.dm_1` demo files. Reads a demo, lets you
trim it, and writes a new demo that plays in an unmodified CoD2 client.

The headline feature (in progress) is **skip dead-time**: a normal match demo
spends most of its length showing you a respawn timer. This tool removes those
dead stretches by re-timing the demo, so playback is action-only — something the
server physically can't do, because demo playback is driven by timestamps.

Works with demos from **every CoD2 version** — 1.0, 1.2, 1.3, and 1.4/CoD2x
(protocols 115/117/118/119) — from one binary. The version is detected
automatically from the demo itself.

## Status

| Stage | What | State |
|------|------|-------|
| DT0 | Read any demo → friendly `--info` summary | **done** (validated on real 1.0 + 1.3 demos) |
| DT1a | Snapshot/gamestate **encoder** (write valid CoD2 bits) | **done** (compiles) |
| DT1b–c | Wire encoder into a playable `--copy` round-trip | in progress |
| DT2 | `--skip-dead` and `--cut` | planned |
| DT3 | UX polish + Windows build | planned |

## Build

```
g++ -g -m32 -static -Wno-write-strings -DCOD_VERSION=COD2_1_3 src/reader.cpp -o bin/cod2-demotool
```

Built `-m32` so struct offsets and bit-field encodings line up with the engine.
A `Makefile` is included for reference (no `make` required).

## Usage

```
cod2-demotool --info  <demo.dm_1>     show what a demo is (version, map, length, players)
cod2-demotool --dump  <demo.dm_1>     write a verbose per-frame decode to <demo>.log
```

Coming next:

```
cod2-demotool --copy      in.dm_1 out.dm_1     re-encode unchanged (round-trip proof)
cod2-demotool --skip-dead in.dm_1 out.dm_1     remove dead/respawn time
cod2-demotool --cut 60 180 in.dm_1 out.dm_1    keep only seconds 60–180
```

Example:

```
$ cod2-demotool --info match.dm_1
  CoD2 1.3 (protocol 118)
  map mp_matmata   gametype ctf
  length 1:34   frames 1887   players 1
  host Verindra CTF HQ
```

## How it works

- **`src/reader.cpp`** — the decoder. Seeded from the community
  [CoD2-DemoParser](https://github.com/) and fixed to build on a modern
  toolchain (a stale forward-declaration; an unused live-client interpolation
  block removed). Decodes a `.dm_1` into in-memory gamestate + per-frame
  playerstate / entities / clients.
- **`src/writer.h`** — the encoder. The exact inverse of the decoder, ported
  from the reverse-engineered CoD2 server (CoD2rev_Server) and verified
  field-for-field against the reader so a decode→encode round-trip is
  byte-identical. Bit primitives, delta encoders (entity / client / playerstate
  / hud / objective), and (next) the snapshot + gamestate message builders.

Both halves share one translation unit; the version differences across
115/117/118/119 are a buffer size and one CoD2x stat branch, so a single 1.3
build reads them all.

## Why a tool, not a server feature

A `.dm_1` plays back by serverTime. A gap in the recording plays as a *freeze*,
not a skip — so dead-time can only be removed offline, by deleting frames and
re-timing every absolute timestamp after them. That is exactly what CoD4's demo
tool does, and what this does for CoD2.

## Credits

Decoder seeded from CoD2-DemoParser. Encoder ported from CoD2rev_Server. Part of
the Verindra Mod project.

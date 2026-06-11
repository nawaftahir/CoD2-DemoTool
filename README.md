# CoD2-DemoTool

An offline editor for Call of Duty 2 `.dm_1` demo files. Reads a demo, edits it,
and writes a new demo that plays in an unmodified CoD2 client — or renders the
match as a readable timeline.

The headline feature is **skip dead-time**: a normal match demo spends much of
its length showing you a respawn timer. This tool removes those dead stretches
by re-timing the demo, so playback is action-only — something the server
physically can't do, because demo playback is driven by timestamps.

Works with demos from **every CoD2 version** — 1.0, 1.2, 1.3, and 1.4/CoD2x
(protocols 115/117/118/119) — from one binary. The version is detected
automatically from the demo itself.

## Usage

```
cod2-demotool --info      <demo.dm_1>                show what a demo is (version, map, length, players)
cod2-demotool --skip-dead <in.dm_1> <out.dm_1>       remove death/respawn dead-time
cod2-demotool --cut       <in.dm_1> <out.dm_1> <s> <e>   keep only the time range [s, e]
cod2-demotool --overview  <demo.dm_1> [out.html]     match timeline: kills, chat, score (HTML optional)

cod2-demotool --copy      <in.dm_1> <out.dm_1>       re-encode unchanged (round-trip proof)
cod2-demotool --deadscan  <demo.dm_1>                show the dead spans --skip-dead would remove
cod2-demotool --commands  <demo.dm_1>                raw server-command channel by time (debug)
cod2-demotool --dump      <demo.dm_1>                verbose per-frame decode to <demo>.log (debug)
```

`--cut` times are `mm:ss` from the demo start (or plain seconds), or the words
`start` / `end`:

```
cod2-demotool --cut game.dm_1 clip.dm_1 1:30 3:00
cod2-demotool --cut game.dm_1 tail.dm_1 2:00 end
```

`--overview` prints the killfeed with player chat, announcements and team-score
changes interleaved in order, plus a per-player kills/deaths/headshots table.
Give it a second path and it writes the whole thing as one self-contained HTML
match page with names in their CoD colours:

```
$ cod2-demotool --overview match.dm_1
=== mp_toujane  -  ctf ===

  0:00  Rust_              >> B Kombat            [thompson_mp]
  0:00  * Welcome Jjj
  0:01  Scofield           >> Pro100Nik#762       [headshot]
  0:02  (dead) Bugs: nice shot
  ...

$ cod2-demotool --overview match.dm_1 match.html
overview: 260 kills, 21 chat, 15 announcements  ->  match.html
```

## Status

| Stage | What | State |
|------|------|-------|
| DT0 | Read any demo → friendly `--info` summary | **done** (validated on real 1.0 + 1.3 demos) |
| DT1 | Encoder + `--copy` round-trip | **done** (byte-faithful, verified in-game) |
| DT2 | `--skip-dead` and `--cut` | **done** (verified in-game incl. 31-player demos) |
| Overview | `--commands`, `--overview`, HTML match page | **done** |
| DT3 | Windows build + batch UX | next |

## Build

Linux:

```
g++ -g -m32 -static -Wno-write-strings -DCOD_VERSION=COD2_1_3 src/reader.cpp -o bin/cod2-demotool
```

Windows `.exe` (where your demos usually live):

```
./build-win.sh        # uses a local i686 mingw if present, else a throwaway docker image
```

The result is a single static `cod2-demotool.exe` — no DLLs to ship. The code is
plain C stdlib (no POSIX), so the same `-m32` 1.3 layout cross-compiles unchanged.
Built `-m32` so struct offsets and bit-field encodings line up with the engine. A
`Makefile` (`make` / `make win`) is included too.

## How it works

- **`src/reader.cpp`** — the decoder. Seeded from the community CoD2-DemoParser
  and fixed to build on a modern toolchain, then hardened (delta-removal
  sentinel, client parse-ring sizing, snapshot bounds). Decodes a `.dm_1` into
  in-memory gamestate + per-frame playerstate / entities / clients, and hosts
  the CLI commands.
- **`src/writer.h`** — the encoder. The exact inverse of the decoder, ported
  from the reverse-engineered CoD2 server (CoD2rev_Server) and verified
  field-for-field so a decode→encode round-trip is byte-faithful. Editing
  (skip-dead / cut) re-encodes each kept frame as a delta from the previous
  kept frame and re-times every absolute timestamp (snapshot, playerstate,
  entity trajectories, HUD element animations) by the removed duration.
- Kills in `--overview` are read straight from the snapshot entity stream
  (obituary temp-entities carry victim, attacker and weapon); chat,
  announcements and team scores come from the server-command channel.

Both halves share one translation unit; the version differences across
115/117/118/119 are a buffer size and one CoD2x stat branch, so a single 1.3
build reads them all.

## Why a tool, not a server feature

A `.dm_1` plays back by serverTime. A gap in the recording plays as a *freeze*,
not a skip — so dead-time can only be removed offline, by deleting frames and
re-timing every absolute timestamp after them. That is exactly what CoD4's demo
tool does, and what this does for CoD2.

## Credits

Decoder seeded from CoD2-DemoParser (eyza). Encoder ported from CoD2rev_Server.
Inspired by the CoD4-X-Demo-Tool (Caball) and CoD4-DM1 (Iswenzz). Part of the
Verindra Mod project.

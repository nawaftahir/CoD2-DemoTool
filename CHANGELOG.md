# Changelog

Human-friendly summaries of what changed in CoD2-DemoTool.

## [nightly] — unreleased

### Added
- **Windows GUI (`cod2-demotool-gui.exe`)** — a proper window so you don't need the
  command line, with a dark **CoD2-menu look** (amber/gold accent). Double-click to open
  it, **drag a demo in** (or click Browse), pick what to do from a dropdown — Info, Skip
  dead-time, Cut, Remove HUD, Clean text, or Match overview — set the output name (filled
  in for you), and click **Run**. The result shows in the window; the overview opens the
  HTML in your browser. No console window, no flags. The command-line `cod2-demotool.exe`
  is unchanged for scripts and power users. Build it with `make win-gui` (or
  `./build-win-gui.sh`). Built with ImGui on a Win32 + OpenGL backend, so it
  cross-compiles from Linux/WSL with the same toolchain as the CLI.
- **`--info`** — read any CoD2 demo and print a one-glance summary: game version,
  map, gametype, length, frame count, players, and the server's hostname. The game
  version is detected automatically from inside the demo.
- **`--dump`** — write a full per-frame decode to `<demo>.log` for debugging.
- **Encoder** (`src/writer.h`) — all the bit-level write primitives, delta encoders
  (entity, client, playerstate, hud, objective), and the snapshot/gamestate message
  builders, written as the exact inverse of the decoder.
- **`--copy`** — re-encode a demo unchanged, frame by frame, into a new playable
  demo. The round-trip that proves the writer: every frame transcodes, the output
  re-parses identically, and it lands within ~1% of the original size.
- **`--skip-dead`** — the headline feature: removes every death-to-respawn stretch —
  the dead-stare, the **killcam** (the seconds spent watching your killer), and any
  spectating — and re-times the demo so the action plays back-to-back in an unmodified
  CoD2 client. A 15:20 CTF match plays in ~12:24 (~3 minutes of dead-time and killcams
  removed). Add **`keep-killcam`** (`--skip-dead in out keep-killcam`) to keep the kill
  replays and trim only the dead-stare/spectating instead (15:20 → 14:31).
- **`--deadscan`** — diagnostic that lists where the player is dead, so you can see
  exactly what `--skip-dead` will cut before running it.
- **`--commands`** — list every server command in the demo by time (chat, announcements,
  cvar pushes, configstring updates). Player chat is decoded and labelled. This is the
  demo's event channel — the groundwork for a killfeed/chat overview.
- **`--overview`** — the demo's full match timeline: the killfeed (who killed whom,
  when, and with what — real player and weapon names, plus headshot/melee/suicide/
  falling deaths), with **player chat, server announcements, and team-score changes
  interleaved in chronological order**, a per-player kills/deaths/headshots table,
  and the final score. Reads kills straight out of the snapshot stream and events
  from the command channel; works on stock and modded demos.
- **HTML match page** — `--overview demo.dm_1 match.html` writes the whole overview
  as a single self-contained dark-themed HTML page: header with map/score, the player
  summary table, and the full timeline with **names rendered in their CoD colours**
  and headshots highlighted. Share or archive a match as one file.
- **Game-style killfeed icons** — the HTML killfeed shows each kill as
  *killer (weapon icon) victim*, laid out exactly like the in-game feed: the killer is
  right-aligned, the icon centred, the victim left-aligned, so every icon and name lines
  up in clean columns down the feed (no arrows). Dedicated icons for headshot / melee /
  falling / suicide kills. Icons are baked into the page (no external files), and only the
  ones a match actually uses are embedded, so the page stays small. Weapons without an
  icon yet (pistols, launchers, grenades) fall back to their name in brackets. Icons are
  generated from `assets/icons/` by `scripts/gen_icons_header.py`.
- **Console-print types in the overview** — announcement lines now show whether they were
  an `iprintln` (the bottom-left feed) or an `iprintlnbold` (the bold centred print),
  tagged `(println)` / `(printlnbold)`, so you can tell normal notices from the important
  centred ones at a glance.
- **`--cut`** — trim a demo to a time range and play it from the start, e.g.
  `--cut game.dm_1 clip.dm_1 1:30 3:00`. Times are mm:ss from the demo start (or plain
  seconds), or the words `start` / `end`.
- **`--clean`** — strip text from a demo: `chat`, `centertext` (big center prints like
  "airstrike unavailable"), `whitetext` (the bottom-left iprintln feed / connect-leave
  notices), or `all`. e.g. `--clean game.dm_1 clean.dm_1 chat whitetext`. Scores,
  scoreboard, cvars and all gameplay are kept — only the chosen text goes.
- **`--remove-hud`** — strip the server-set HUD elements (custom overlays, kill cards,
  server logos, +N score popups) from a demo. Add `keep <shader-name>` to keep matching
  ones. The ammo/grenade readout (bottom-right) and the compass are **not** HUD elements
  and aren't stored in the demo — the client draws them from the player's own weapon/ammo
  state — so they can't be removed here (hide the compass in-game with `cg_drawcompass 0`).
  The command now prints this so it's clear what was and wasn't touched.
- **`--scale-score`** — multiply the value of the "+N" score-popup HUD elements, e.g.
  `--scale-score game.dm_1 out.dm_1 0.2` to turn +50 into +10.
- **`--split-map`** / **`--split-match`** — break one recording that spans several maps
  or matches into separate playable demos (`<demo>_map1.dm_1`, `_map2.dm_1`, …). Splits
  at each map change (`--split-map`) or each match/round restart (`--split-match`).
- **`--convert`** — re-tag a demo's CoD2 version (`--convert game.dm_1 out.dm_1 118`).
  CoD2's demo format is identical across versions, so this just rewrites the version
  in the demo so it loads on a client of the target version.
- **`--merge`** *(experimental)* — join two demos of the same map and mod into one
  continuous demo with no map reload: `--merge a.dm_1 b.dm_1 ab.dm_1`. Demo B is
  re-timed onto the end of A and its changed game state (scores, etc.) is carried over.
  Caveat (same as the CoD4 tool): brand-new objects in B can flicker, and the camera
  stays on demo A's player.

### Added (UX)
- **Batch / drag-and-drop** — pass several demos at once: with no command you get a
  one-line summary of each; with `--overview` it writes a `<demo>.html` next to each
  demo. Handy for dropping a folder of demos onto the tool on Windows.

### Fixed
- **The killfeed showed the wrong weapon on every kill (off by one).** The weapon list
  is 1-based, but the decoder read it 0-based, so each kill showed the *next* weapon in
  the list - frag-grenade kills appeared as smoke grenades, shotgun kills as binoculars,
  and so on. Fixed; the killfeed now names the exact weapon for every kill. (Caught
  because smoke grenades and binoculars can't actually kill.)
- **"Clean text" no longer strips anything by accident.** Its three options now start
  unticked, and "console prints" is flagged because on some mods the kill feed is printed
  there - so cleaning it would remove the kills. You tick exactly what you want gone.
- **`--info` could crash, and was needlessly slow.** Reading one demo (or dropping a
  single file on the exe) could segfault, and even when it didn't it spent ~14 seconds
  writing a ~28 MB debug log just to print a five-line summary — which is a big part of
  why a double-clicked exe seemed to "do nothing." `--info` now reads in about a second
  and writes no log. (Use `--dump` for the full per-frame trace.)
- Demo length and player count were wrong on demos whose serverTime resets mid-recording
  (e.g. a map restart) — length could even go negative. Length now tracks the peak time,
  and the decoder is fully reset between demos so a batch run can't carry state across.
- Entities and players that disappeared between frames (a "delta removal") were being
  decoded as lingering ghost entities instead of being dropped. Re-encoding those
  ghosts re-triggered their effects — that was the cause of bullet-impact FX and 3D
  sound looping forever after a cut. Removals now decode correctly, so cuts are clean.
- **High-player demos (≈30+ players) came out corrupt** — players had wildly wrong
  animations, and the edited demo could crash on reopen. The client history buffer was
  far too small (only ~4 frames), so on a busy server it overwrote the very data each
  frame is built from. Enlarged it to cover the full delta window; high-player demos
  now edit cleanly. (Also hardened the reader so a malformed demo can never crash it.)
- HUD element animation timings (fades, scales, moves) are now re-timed across a cut,
  so on-screen HUD elements stay in sync after dead-time is removed.
- **`--skip-dead` was leaving the killcam in.** After a death the demo follows your
  killer for a few seconds (the killcam), and during that time the recording shows the
  *killer's* state — alive, full health — so the old detector thought you'd already
  respawned and kept the whole killcam. It now recognises the follow-the-killer state
  and cuts the full dead → killcam → respawn stretch (or keeps just the killcam with
  `keep-killcam`). On a 15:20 match this removed ~2 extra minutes that used to slip
  through. Free-spectating other players is cut too.
- The respawn countdown ("Spawn in N seconds") could show a wrong number after an edit:
  the timer it's drawn from lives in a second HUD array that wasn't being re-timed.
  Both HUD arrays are now shifted, so timers (respawn, round clock, bomb timer) read
  correctly across a cut.
- **The red "you're hurt" screen flash could bleed into the next spawn.** If you took
  damage just before dying, the flash is triggered by a value that the game normally
  clears on respawn — but skip-dead deletes the respawn frame, so the flash carried
  over into your fresh life. Skip-dead now carries that damage state cleanly across
  each cut so the new spawn starts with a clean screen. (Also fixes a stray flash at
  a `--cut` seam that lands in a firefight.)

### Notes on what `--skip-dead` removes
- Kill messages ("You killed X" / "Killed by X") that happened **while you were dead**
  are removed along with the dead-time — that killfeed scrolled by during the seconds
  the tool is cutting out, so it goes with them. Your **own** death messages come back
  with `keep-killcam` (the killcam that follows your killer carries them). This is by
  design: skip-dead removes the time you were dead, and the killfeed from that time
  goes with it.

### Notes
- A single binary reads demos from **every CoD2 version** — 1.0, 1.2, 1.3, and
  1.4/CoD2x (protocols 115/117/118/119).
- Verified on real 1.0 and 1.3 demos, including ones auto-recorded on the Verindra
  server.
- Decoder seeded from CoD2-DemoParser (fixed to build on a modern toolchain);
  encoder ported from the reverse-engineered CoD2 server (CoD2rev_Server).

### Next
- DT3 polish: a Windows build, and a friendlier batch workflow.

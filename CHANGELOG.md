# Changelog

Human-friendly summaries of what changed in CoD2-DemoTool.

## [nightly] — unreleased

### Added
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
- **`--skip-dead`** — the headline feature: removes every death-to-respawn stretch
  (including the killcam) and re-times the demo so the action plays back-to-back in
  an unmodified CoD2 client. A 1:40 match plays in ~1:19; on a 30-minute demo it
  trims ~100 seconds of dead time. Confirmed in-game.
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
- **`--cut`** — trim a demo to a time range and play it from the start, e.g.
  `--cut game.dm_1 clip.dm_1 1:30 3:00`. Times are mm:ss from the demo start (or plain
  seconds), or the words `start` / `end`.
- **`--clean`** — strip text from a demo: `chat`, `centertext` (big center prints like
  "airstrike unavailable"), `whitetext` (the bottom-left iprintln feed / connect-leave
  notices), or `all`. e.g. `--clean game.dm_1 clean.dm_1 chat whitetext`. Scores,
  scoreboard, cvars and all gameplay are kept — only the chosen text goes.
- **`--remove-hud`** — strip the server-set HUD elements (custom overlays, kill cards,
  server logos) from a demo. Add `keep <shader-name>` to keep matching ones. (The
  hitmarker and the ammo/score readouts aren't stored in CoD2 demos, so they're
  unaffected; team-skull head icons live in the entity data and are left alone.)
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

### Notes
- A single binary reads demos from **every CoD2 version** — 1.0, 1.2, 1.3, and
  1.4/CoD2x (protocols 115/117/118/119).
- Verified on real 1.0 and 1.3 demos, including ones auto-recorded on the Verindra
  server.
- Decoder seeded from CoD2-DemoParser (fixed to build on a modern toolchain);
  encoder ported from the reverse-engineered CoD2 server (CoD2rev_Server).

### Next
- DT3 polish: a Windows build, and a friendlier batch workflow.

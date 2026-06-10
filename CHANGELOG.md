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
- **`--overview`** — the demo's killfeed: who killed whom, when, and with what (real
  player and weapon names, plus headshot/melee/suicide/falling deaths). Reads the kill
  events straight out of the snapshot stream and works on stock and modded demos. The
  first step toward a full demo overview / highlight finder.
- **`--cut`** — trim a demo to a time range and play it from the start, e.g.
  `--cut game.dm_1 clip.dm_1 1:30 3:00`. Times are mm:ss from the demo start (or plain
  seconds), or the words `start` / `end`.

### Fixed
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

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

### Notes
- A single binary reads demos from **every CoD2 version** — 1.0, 1.2, 1.3, and
  1.4/CoD2x (protocols 115/117/118/119).
- Verified on real 1.0 and 1.3 demos, including ones auto-recorded on the Verindra
  server.
- Decoder seeded from CoD2-DemoParser (fixed to build on a modern toolchain);
  encoder ported from the reverse-engineered CoD2 server (CoD2rev_Server).

### Next
- Confirm a `--copy` plays identically in an actual CoD2 client (in-game test).
- **`--skip-dead`** / **`--cut`** — the actual editing features.

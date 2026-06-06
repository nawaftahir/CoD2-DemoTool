# Changelog

Human-friendly summaries of what changed in CoD2-DemoTool.

## [nightly] — unreleased

### Added
- **`--info`** — read any CoD2 demo and print a one-glance summary: game version,
  map, gametype, length, frame count, players, and the server's hostname. The game
  version is detected automatically from inside the demo.
- **`--dump`** — write a full per-frame decode to `<demo>.log` for debugging.
- **Encoder** (`src/writer.h`) — all the bit-level write primitives and delta
  encoders (entity, client, playerstate, hud, objective), written as the exact
  inverse of the decoder. This is the groundwork for writing edited demos back out.
  Not yet attached to a command.

### Notes
- A single binary reads demos from **every CoD2 version** — 1.0, 1.2, 1.3, and
  1.4/CoD2x (protocols 115/117/118/119).
- Verified on real 1.0 and 1.3 demos, including ones auto-recorded on the Verindra
  server.
- Decoder seeded from CoD2-DemoParser (fixed to build on a modern toolchain);
  encoder ported from the reverse-engineered CoD2 server (CoD2rev_Server).

### Next
- **`--copy`** — re-encode a demo unchanged and confirm the output plays identically
  in CoD2. This proves the writer before any editing is added.
- **`--skip-dead`** / **`--cut`** — the actual editing features.

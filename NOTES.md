# CoD2-DemoTool — Engineering Notes & Verification Log

Purpose: lock down correctness before adding features. Demo bugs are hard to debug
after the fact, so every `--skip-dead` artifact gets root-caused, fixed, and verified
here before we move on.

---

## Engine gotchas (hard-won, non-obvious — read before touching the writer)

1. **Delta encoding sends ABSOLUTE field values, not increments.** A playerstate/
   entity field, when it changes, carries its full new value; unchanged fields are
   skipped. So any re-timing must shift the absolute time fields on **every emitted
   frame**, not just at cut boundaries. (This caused the slow-mo bug.)

2. **TWO separate event channels — don't confuse them:**
   - **(a) Ring events** — persistent entities + playerState carry `events[4]` /
     `eventParms[4]` / `eventSequence`; the client fires when the sequence advances
     (footsteps, weapon fire on the player). At a cut, zero the ring data (keep the
     counter) so a sequence jump doesn't replay these.
   - **(b) Temp / event entities** — `eType >= ET_EVENTS (0xA)`. One-shot entities for
     **impacts / explosions / sounds**. The event id is encoded **in `eType`**
     (`eType = ET_EVENTS + event`), details in `eventParm` — it does NOT use the ring.
     The client fires the FX on **first-see** of the entity. So re-emitting a temp
     entity from baseline at a full/cut frame = a brand-new first-see → the FX
     replays / sticks. **Fix: OMIT `eType >= ET_EVENTS` entities at full (cut) frames.**
     (This was bug #4. The first attempt zeroed the ring — the *wrong channel* — so it
     did nothing for impacts.)

3. **Client-ring mask bug in the seeded parser.** `cl.parseClients` is sized
   `MAX_PARSE_CLIENTS` (128), but `CL_ParsePacketClients` read old clients with the
   `MAX_PARSE_ENTITIES` mask (2047) → out-of-bounds / wrong slot → corrupt client
   names after ~128 frames. Fixed to the client mask. (Caused wrong/blank names.)

4. **Dangling delta bases.** A kept frame whose delta base was a *dropped* frame is
   un-decodable → the client can't rebuild it (shows as stale/duplicate snapshots).
   Track a kept-seq ring and force a full (non-delta) snapshot whenever a frame's
   delta base wasn't kept. (Caused the scrubbing / 1031 duplicate timestamps.)

5. **Demos are time-driven.** `serverTime` drives playback; a gap plays as a freeze,
   not a skip. Re-time `serverTime` (every frame) + `commandTime` + entity
   `pos/apos.trTime`; leave durations (`trDuration`, etc.) alone.

---

## Bugs found in `--skip-dead` and fixes (chronological)

| # | Symptom | Root cause | Fix | Verified |
|---|---------|-----------|-----|----------|
| 1 | slow-mo after the first cut | `commandTime` re-timed only at boundaries, but it's sent absolute every frame → drifted ahead of `serverTime` | re-time absolute time fields on every frame (copies of old+new) | ✓ cmdTime−serverTime diff flat (−233 start → −234 end) |
| 2 | jerky / "scrubbing a video slider" | dropped frames left kept frames delta-referencing missing bases → 1031 duplicate serverTimes | force full snapshot for any frame whose delta base wasn't kept (kept-seq ring) | ✓ serverTime steps all 50ms, 0 duplicates |
| 3 | wrong / blank player name on respawn | `CL_ParsePacketClients` old-client mask was `MAX_PARSE_ENTITIES` not `MAX_PARSE_CLIENTS` | use the client mask (4 sites) | ✓ name decodes stable across all frames |
| 4a | (first attempt, WRONG) stuck FX | guessed `eventSequence` ring replay | zeroed ring data on cut frames | ✗ did nothing — wrong channel |
| 4b | stuck / replayed bullet impact FX at cuts | impact FX are **temp entities** (`eType>=ET_EVENTS`); client fires on **first-see**; re-emitting them at a full cut frame = a new impact | **omit `eType>=ET_EVENTS` entities at full/cut frames** (12 such frames in the test demo); keep ring-clearing for persistent entities | ⏳ in-game re-test (v3) |

---

## Verification checklist (skip-dead must pass ALL before we proceed)

Automated (run per demo; eventually across protocols 115/117/118/119):
- [x] serverTime steps uniform, match original cadence, no duplicates
- [x] commandTime tracks serverTime (≈constant diff)
- [x] output has 0 dead spans
- [x] client names stable
- [x] output re-parses cleanly
- [x] `--copy` still faithful (event-clear scoped to skip-dead cuts only)

In-game (user must confirm):
- [x] plays at normal speed, smooth (no slow-mo)
- [x] clean instant jump-cut at each death→respawn
- [x] correct player names on every spawn
- [ ] **no replayed / stuck FX (bug #4 fix — awaiting confirmation)**
- [ ] no other-entity jitter at cuts

Cosmetic / known, decide later:
- HUD match-countdown clock jumps back at cuts (match start-time not re-timed) — the
  Caball CoD4 tool exposes a setting for this; revisit if it bothers anyone.

---

## Dev insights / references (from the community)

**People:**
- **eyza / YctN** — author of [CoD2-DemoParser](https://github.com/eyza-cod2/CoD2-DemoParser)
  (the decoder we seeded from). Goal: fully understand the CoD2 demo format; eventually
  multiplayer demos and/or server-side demos.
- **Anomaly = Caball** — author of the CoD4-X-Demo-Tool. Worked **very extensively** on CoD4
  demos; implemented **server-side demos for CoD4 and for CoD Ghosts**. Offered to help with
  demo questions.
- **IzNoGoD** — notes server-side demos are already in CoD4x
  (`callofduty4x/CoD4x_Server`, search "demorecording").

**Key takeaways from their Dec 2025 discussion:**
- CoD2-DemoParser's current state: "decode a demo into the frames/entities, but that's about it."
  Confirms we're extending past where the public tooling stopped.
- Caball's advice (paraphrased): *"unless you have ALL the relevant game functions in C/C++,
  just cut some corners to keep it simple — that makes multiplayer demos tricky, though."*
  → relevant to us: faithful re-encode is hard precisely because we don't run the game's logic;
  we operate purely on decoded state, so edits (skip/cut) must be byte-careful.
- Suggested order if extending the format: **server-side demos first, then multiplayer demos.**
  (We already did server-side recording in zk_libcod; this tool is the offline editor.)

**More from the chat (May 2025):**
- **Anomaly contributed to IWXMVM** (the CoD4 in-game demo/movie viewer with timeline, scrubbing,
  go-backward, demo overview). So IWXMVM is directly relevant prior art — that "good stuff" (seek/
  timeline/overview) is the long-term vision for an in-game CoD2 demo player. IWXMVM had ambitious
  plans to expand to multiple CoD games (likely won't happen, but the CoD4 work is solid).
- **Demo data rate (CoD4):** ~10–12 KB/s for 1 player @ 125 client fps + 20 sv_fps. Anomaly's
  estimate: `125*60 bytes + 20*250 bytes ≈ 12KB/s` (generous). Mostly **client-generated** data,
  not transmitted over the net → that's why client demos are richer than server-side ones.
- eyza on CoD2-DemoParser: "I ended up parsing the snapshot" (i.e. it decodes snapshots, the hard part).
- Vision both share: an **in-game demo player** that supports going backward, moving on a timeline,
  and a demo overview — exactly what IWXMVM does for CoD4.
- Anomaly offered to answer CoD4 demo / netcode questions (muted the server; tag/PM to reach).

> TODO: dump more of the CoD2x devs / Caball chat logs here as we get them — they're the best
> primary source for demo-format edge cases. (Reference tweet from reallyluckyy/IWXMVM noted.)

## Bug #4 (stuck impact FX / looping sound) — still OPEN

Confirmed mechanism (research + decompiled CoD2 1.3 client): a **persistent broadcast FX
entity** (e.g. `ET_LOOP_FX`, or any entity removed during the dropped span) loses its
**removal delta** because skip-dead's full (cut) frame emits entities from baseline with
`old=NULL` → the removal branch never runs → the client keeps the entity (and its 3D
loop sound) forever. Confirmed in-game: bullet impact FX + 3D sound that never stops,
only in skip-dead output, near cuts.

Attempts:
- 4a zero the events[]/eventSequence ring on cut frames — WRONG channel (impacts use
  `eType>=ET_EVENTS`, not the ring). No effect.
- 4b omit `eType>=ET_EVENTS` temp entities on cut frames — handles one-shot impacts, but
  NOT the persistent looping entity. No effect on the stuck sound.
- 4c **delta-from-previous-kept-frame** (renumber output frames contiguously, store the
  last kept frame, delta each frame from it so removals propagate). Architecturally the
  right fix, BUT my implementation produced a malformed entity stream (phantom 0-numbered
  entities accumulating from early frames → 1893 entities by frame 488 → truncated/broken
  output). **Reverted.** The bug was in the array-based delta emit
  (`SV_EmitDeltaEntitiesArr` / `SV_WriteSkipSnapshot` in writer.h, driver in reader.cpp) —
  the emitted delta did not decode back to the source frame, so the decoder's base diverged
  and compounded. Those functions remain in the tree, unused/parked, for a future careful retry.

**Current shipping behaviour = the forceFull (non-delta) cut path.** Everything works
(smooth timing, names, 0 dead spans, clean cuts) EXCEPT this one stuck-FX artifact.

**Repro obtained** (`demos/repro.dm_1`, mp_decoy, 2 deaths) + user-confirmed in-game finding
(2026-06-07): **before the first cut, shooting a surface is fine; AFTER the first cut, every
bullet impact's sound + FX plays in a loop / never stops.** So the trigger is the **non-delta
forceFull cut frame** — once the client receives it, subsequent server-sent impact events get
re-triggered/looped. (Impacts here are server EV_BULLET_HIT event entities, not loopSound/
ET_LOOP_FX; map ambient ET_LOOP_FX 714-721 are NOT the culprit.) Entity analysis: only 2
entities are removed-in-a-dead-span (eType=208 event entities at the death instants), not the
post-cut loopers — consistent with the cause being the cut frame's effect on client event
state, not a single lost removal.

**Conclusion:** the real fix is to STOP emitting a non-delta cut frame — i.e. delta each kept
frame from the previous kept frame (attempt 4c). That attempt is architecturally right but my
array-delta emit had a bug (phantom 0-entities accumulating). Needs a careful retry, debugged
on the small `repro.dm_1` (find the frame where the emitted delta stops decoding back to the
source frame). Until then the forceFull path ships with this known post-cut impact-loop artifact.

## Bug #4 — ROOT CAUSE FOUND (2026-06-07): a DECODER bug, not an encoder bug

After weeks chasing the "phantom 0-entity" accumulation in attempt 4c, the real cause was a
single bug in the **decoder** (`MSG_ReadDeltaStruct`, the seeded CoD2-DemoParser):

```c
// remove branch — BEFORE (broken):
if (MSG_ReadBit(msg) == 1) { return; }            // never marked the entity removed
// AFTER (fixed):
if (MSG_ReadBit(msg) == 1) { *(uint32_t*)to = (1 << indexBits) - 1; return; }  // 1023 ents / 63 clients
```

`CL_DeltaEntity`/`CL_DeltaClient` detect a removal by `state->number == MAX_*-1`. The remove
branch returned **without setting that sentinel**, so every explicit removal was missed: the
removed entity was kept with stale ring data (decoded as a phantom `number 0, eType 0`), and
these accumulated frame after frame.

**Why this was so destructive — it corrupted the encoder, not just the dump:**
the tool is one pass `decode → re-encode`. The phantoms lived in `cl.parseEntities`, so the
**encoder re-emitted dead entities as present**. For temp/event entities (`eType>=ET_EVENTS`)
the client first-sees the re-introduced entity and **fires its FX/3D-sound again** → exactly the
user's "bullet impact + sound loops forever after the first cut." So ONE decoder bug produced:
(1) the phantom 0-entities in `--dump`, (2) the "1893 entities → truncated" that made me revert
the architecturally-correct 4c, and (3) the in-game stuck/looping FX.

**The lesson:** my analysis tool (the decoder) was lying to me. The `--copy` "phantoms" and the
4c "failure" were the same artifact. Attempt 4c (delta each kept frame from the previous kept
frame) was right all along; I reverted it on bad evidence.

### Post-fix verification (automated, mp_matmata `testingdemo` + mp_decoy `repro`)
- decoder fixed → **original** `testingdemo` true max ne = **5** (the old "41" was inflated phantoms).
- `--copy` round-trips **0 / 1887 frames mismatched**, identical max ne — byte-faithful.
- `--skip-dead repro.dm_1`: 628→411 frames, **10.9s dead-time removed**, max ne **19 vs 20**
  (no accumulation), event-entity rows **106 vs 118** (dead-span ones gone, NOT looping),
  serverTime **all 409 steps = 50ms** (no dup/gap).
- Current skip-dead path = delta-from-previous-kept-frame (`SV_WriteSkipSnapshot` +
  `SkipExtractFrame` + `SV_EmitDeltaEntitiesArr`), renumbered output seqs, non-delta only on the
  very first frame. The old forceFull/non-delta-cut path is retired.

### Confirmed
- [x] **in-game (2026-06-07, user)**: `demos/repro_skipdead_v6.dm_1` plays with **NO stuck/looping
  impact FX** after the cuts. The exact symptom is gone — GATE PASSED. Bug #4 closed.
- Running a multi-agent pre-commit review of the diff (decoder sentinel safety across all
  MSG_ReadDeltaStruct callers incl. hud/objective, skip-dead edge cases, cross-protocol --copy
  regression, dead-code hygiene) before committing — findings + resolutions appended below.
- DT3 cleanup: the `forceFull==qtrue` branches in `SV_WriteSnapshot`/`SV_EmitPacketEntities`
  (clearEvents/ClearPlayerstateEvents/omit-eType) are now dead — skip-dead uses `SkipExtractFrame`
  instead. Remove on the next polish pass.

## Pre-commit review (2026-06-07, 7-agent workflow) — findings & resolutions

Ran an adversarial review of the full diff before committing. Outcome: decoder fix validated;
one real latent re-timing bug found & fixed; minor dead code removed; rest deferred to DT3.

**Validated, no action (the decoder fix is correct):**
- The remove-sentinel `*(uint32_t*)to = (1<<indexBits)-1` is UNREACHABLE for hud/objective
  structs (they don't route through MSG_ReadDeltaStruct — own loops, no remove branch). Only
  entities (1023) and clients (63) hit it, exactly matching the caller's MAX_*-1 checks.
- It's the exact inverse of the authoritative engine's removal encode (CoD2rev msg_mp.cpp /
  sv_snapshot_mp.cpp). The real engine reader omits the write because its caller pre-seeds the
  slot's number; this tool doesn't pre-seed, so writing the sentinel here is the correct minimal fix.
- `--copy` round-trips 0-mismatch on 7/8 real demos (protocols 115/118/120; 3.35M ENT lines on
  match_burgundy, 0 diffs). Real entity-0 (eType≠0) preserved; only the ghost entity-0 is dropped.

**FIXED before commit (real latent re-timing gaps — masked because sign-off demos had only static
entities with time/time2 = 0):**
- `RetimeEntity` only shifted `pos.trTime`/`apos.trTime`. The engine shifts FOUR fields, each
  guarded nonzero: + `time` + `time2` (absolute serverTimes: missiles `s.time = level.time + prestep`,
  dropped items `s.time2 = level.time + 60000` despawn). Without it, after a cut dropped weapons/
  missiles linger ~cut-duration too long. **Fixed**: all four shifted, each `if(nonzero)`.
- `RetimePlayerstate` only shifted `commandTime`. Added the other absolute serverTimes the engine's
  archive path shifts: `deltaTime` (always) + guarded `jumpTime`, `foliageSoundTime`,
  `viewHeightLerpTime`, `shellshockTime`, `adsDelayTime`. Left durations/countdowns alone
  (weaponTime, pm_time, *Timer, grenadeTimeLeft, trDuration — these decrement per-frame, relative).
- **Verified on match_burgundy** (31 dead spans, 102s cut, time2 entities): output `time2−serverTime`
  gap set is IDENTICAL to the original (range −92..+60s, 0 output-only values). If time2 weren't
  shifted the output would show gaps up to +162s — it doesn't. time2 tracks serverTime in lockstep.
- Removed dead `skipState_t.keptSeq[]` field (superseded by the g_sf ping-pong; never read).

**Deferred to DT3 (noted, not regressions):**
- The `forceFull`/`timeOffset`/`clearEvents` params + branches in SV_WriteSnapshot/SV_EmitPacketEntities
  are now dead (skip-dead uses SV_WriteSkipSnapshot; --copy passes 0/qfalse) — but they sit on the
  verified --copy path, so strip them when the writer API settles at --cut, not now.
- **Pre-existing writer bug**: `beltot_try_2.dm_1` (41-player, p115) --copy produces a corrupt
  client-section that crashes re-decode. Reproduces on the PRE-fix binary too → not caused by this
  work. High-client-count packet-clients encoder needs a separate look.
- Dead-span-runs-to-EOF (player never respawns) silently drops the tail — add an EOF fallback/warning.
- numEnts/numClients caps in SkipExtractFrame truncate silently (unreachable today) — add a warning.
- Test corpus lacks protocol 117 and 119 demos — add samples.

## Bug #5 — client parse-ring too small (high-player demos): SEGFAULT + weird player animations

User tested on `touj_rus.dm_1` (31-player, **protocol 115 / CoD2 1.0**, mp_toujane ctf): `--cut`
output segfaulted on re-`--info`; skip-dead output played with grossly wrong player animations.
Reproduced with plain `--copy` too -> a PRE-EXISTING writer/ring bug, NOT skip/cut specific (latent
since DT1; `--copy` was only ever in-game-tested on low-player demos).

**Root cause:** `MAX_PARSE_CLIENTS` was `MAX_GCLIENTS*2 = 128`. The client parse ring must hold every
client referenced as a delta base — a snapshot can delta from up to `PACKET_BACKUP` (32) frames back,
each with up to `MAX_GCLIENTS` (64) clients. At 31 clients/frame, 128 slots hold only ~4 frames.
touj_rus deltas frames from up to 4 frames back, so by snapshot 4 the ring head (155) had wrapped past
the delta base (`from.parseClientsNum=0`) -> the old-frame client window was overwritten.
- The DECODER survives: it reads the old frame *just-in-time* during decode (ascending, right before
  each slot is overwritten).
- The WRITER reads the whole `from` window *after* decode completes (ring head fully advanced), so it
  sees the clobbered base -> emits a garbage client delta (spurious `+0 +1 +2 +3 ... -29 -35 -36 -39`)
  -> re-decode inflates/oscillates the client count (31 -> 27/49/71) -> wrong player data in-game and
  `numClients > 64` overflowing `snapshot.clients[64]` on re-decode (the stack-smash segfault).

**Fix (2 parts):**
1. `MAX_PARSE_CLIENTS = MAX_GCLIENTS * PACKET_BACKUP * 2 = 4096` (power-of-2; holds 32 frames of 64
   clients with headroom so the base is never overwritten). `declarations.hpp`.
2. Robustness: `CL_GetSnapshot` clamps `numClients` to `MAX_CLIENTS_IN_SNAPSHOT` (mirrors the existing
   entity clamp) so a corrupt/oversized count can never smash the stack again. `reader.cpp`.

**Verified:** touj_rus `--copy`/`--skip-dead`/`--cut` all exit 0; client section round-trips 0
mismatches (was 27/49/71); villers (1-player) and p118 demos still 0/0. Entities were always fine
(ring 2048). NOTE: the entity ring has the same *theoretical* limit (gap could be `32*256` > 2048) but
no test demo hits it — revisit only if a high-entity far-delta demo ever corrupts.

**Still open (skip-dead refinement, separate bug):** user also saw the **killcam partly present +
killcam HUD weird** after a cut. Dead-span detection itself is correct (`--deadscan touj_rus`: pm_type
0->6->0, the 7.9-10.0s span is dropped), so this is likely either residual from the client corruption
(re-test now it's fixed) or configstring/serverCommand state set during the dropped killcam not being
carried to the next kept frame (the `// FIXME: configstring changes and server commands` gap in
CL_GetSnapshot). Re-test with the ring fix first; tackle HUD/configstring carry if it persists.

## Bug #6 (the "archive") — HUD-element timings not re-timed across cuts (FIXED)
The playerstate carries `hud.archival[31]` HUD elements whose `time`/`fadeStartTime`/`scaleStartTime`/
`moveStartTime` are absolute serverTimes — the engine's archived-snapshot path (SV_GetArchivedClientInfo,
the "archive") re-times exactly these. My re-timing missed them, so HUD animations drifted across a cut.
Added the 4-field shift to `RetimePlayerstate` (archival set only, matching the engine; no future-clamp
since we subtract). **Verified on matm_sk_dead: `hud.time - serverTime` gap set is IDENTICAL orig vs
skip-dead (only the dropped-span gaps absent) -> hud.time shifts in lockstep with serverTime.**

## Status / gate
**COMMITTED `ef649a5`** (client ring fix + clamp + `--cut` + hud archive re-timing). Checkpoint verified
flawless via comprehensive automated tests on the two 31-player matmata demos:
- round-trip `--copy` 0 entity + 0 client mismatches (byte-faithful);
- skip-dead faithfully preserves the source's own frame cadence (40fps=25ms here; 50/150ms steps match
  the original's recording hitches), cut splices continuous (max step = a source 150ms hitch, NOT a
  dead-span gap), 0 duplicate serverTimes, client counts flat 31;
- cut span exact, continuous; hud archive timings re-time in lockstep (verified).
User in-game: cut ~18s OK; skip-dead "most things OK apart from HUDs" (modded-bot server custom HUDs).
**Demos folder is volatile (Windows<->WSL sync rotates it) — always `cp` a demo to /tmp before testing.**
Open/minor: killcam-during-skip-dead refinement (configstring/serverCommand carry) — revisit if a clean
(non-modded) demo shows it; DT3 dead-code strip (forceFull path) + Windows build.

## Overview track — the demo's gameplay layer, fully decoded (commits 583ebcb, a104fe1, 01dbe1c, +HTML)

The reverse-engineering needed for Caball-style match pages and the long-term in-game overview:

**Server-command verbs** (CoD2rev `SV_GameSendServerCommand` call sites, ASCII of the `%c`):
- `h`(104) public chat, `i`(105) team chat — `"\x15<name>^<color><message>"` (G_SayTo). Team chat
  names carry `(GAME_ALLIES)`/`(GAME_AXIS)`, dead chat `(GAME_DEAD)`.
- `e`(101) allClientsPrint, `f`(102) iprintln, `g`(103) iprintlnbold — announcements. Payload after
  the verb: `\x15` prefix = literal text; bare token = localization key (`MP_SCORE_LIMIT_REACHED`);
  `\x14` = localized-key marker inside mixed strings.
- `H`(72) = ALLIES team score, `G`(71) = AXIS team score (`"H <int>"`, GScr_SetTeamScore). Sent on
  every change (TDM: every kill; objective modes: caps/plants) + periodic syncs — dedupe by value.
- `v` set client cvar, `q`(113)/`p`(112) sound fades, `o`(111) menus, `b` scoreboard blob.

**Kills (killfeed)** are NOT commands — they're **EV_OBITUARY temp entities in the snapshot stream**:
`eType = ET_EVENTS(10) + EV_OBITUARY(198) = 208`; `otherEntityNum` = victim, `attackerEntityNum` =
attacker (>= MAX_CLIENTS ⇒ world), `eventParm` = weapon index into the CS_WEAPONS(7) space-separated
list, OR `(MOD | 0x80)` for melee(7)/headshot(8)/crush(9)/falling(11)/suicide(12) (GScr_Obituary).
Obituary entities persist ~8-13 frames → dedup by entity-number-new-this-frame.

**--overview** = killfeed + chat + announcements + score changes interleaved per frame, K/D/HS table,
final score. Optional second arg writes a self-contained dark HTML match page (names rendered in CoD
colours). Score lines suppressed on tdm/dm (killfeed already shows it); final score always printed.

**Gotchas hit & fixed along the way:**
- Names can use **doubled colour codes `^^NN`** → strip/colorize = N carets then up to N digits.
- CoD2 strings are Latin-1-ish; writing them raw into a UTF-8 HTML page → mojibake/binary-looking
  file. Re-encode bytes >= 0x80 as 2-byte UTF-8 (ASCII unchanged).
- A helper buffer smaller than the callee's headroom guard (`esc[8]` vs `j < dstsize - 8`) silently
  drops EVERY character — symptom was "names empty in the summary table only". Watch guard math.
- `--dump`'s OBIT probe printed to stdout while the harness redirected stdout to /dev/null → "0
  obituaries" false negative. Capture the right stream before concluding data is absent.

## Caball CoD4X-tool parity — RE findings (5-agent workflow, all high-confidence)

Caball's tool (readme `dev/cod4xdemotool/`) is PURE OFFLINE EDITING — **no rewind/scrub/playback**
(that's IWXMVM, in-game). Its features map to CoD2 as:

**Command filters (clean, do first — NO round-trip risk):** every server→client text is a
`svc_serverCommand` `"<verb> <payload>"` (verb = 1 ASCII byte). Verbs (CoD2rev `SV_GameSendServerCommand`
call sites): `h`=public chat, `i`=team chat, `c`=Announcement/center-text (`c "<text>" 2`, g_scr_main
4825/4838 + g_active 838 inactivity warn), `e`=allClientsPrint + GAME_* notices (2020/2035), `f`=iprintln
(5458), `g`=iprintlnbold (5448), `H`/`G`=allies/axis score, `b`=scoreboard, `v`=setClientCvar, `I`=health,
`o/p/q`=music/fade, etc. Map: removeChat→drop `h`+`i`; removeCenterText→drop `c`; removeWhiteText→drop
`e`+`f`+`g`. **Sequence-gap is SAFE, no renumber:** CL_ParseCommandString dedupes only seq<=current and
accepts any higher seq; snapshots carry their own serverCommandNum high-water mark; a dropped seq leaves
an inert ring slot, never stalls. Implement in Demo_TranscodeFrame svc_serverCommand case (reader.cpp
~2668): read cseq+string, advance clc.serverCommandSequence, `if(CmdShouldDrop(s)) break;` else re-emit
cseq VERBATIM. NEVER drop b/v/I/G/H/o/p/q/control verbs.

**HUD removal:** hudelems ride in `ps.hud.archival[31]`/`current[31]` (hudelem_t), already decoded
(MSG_ReadDeltaHudElems reader.cpp:1355) + encoded (MSG_WriteDeltaHudElems writer.h:331). Identity =
`type` (he_type_t enum declarations.hpp:911: FREE=0,TEXT=1,VALUE=2,...,MATERIAL=6) + `materialIndex`
(→ CS_SHADERS=1566 configstring = shader name) + `text`/`label` (→ CS_LOCALIZED_STRINGS=1310).
**MUST COMPACT not just zero:** MSG_WriteDeltaHudElems stops at the first HE_TYPE_FREE (writer.h:334-336),
so a hole truncates the rest — shift survivors down + zero tail (mirrors engine HudElem_UpdateClient
packing, g_hudelem_mp.cpp:82-151). **MUST mutate `cl.snap.ps.hud` BEFORE reader.cpp:2056** (`cl.snapshots[
..]=cl.snap`) so the stored delta base matches the emitted frame, else deltas desync. Caball's keep-flags
DON'T port: hitmarker=client cgame (not in demo), +N stackscore popup=CoD4-only, team-skulls=ENTITY
fields `iHeadIcon`/`iHeadIconTeam` (entityStateFields declarations.hpp:3129/3131), separate path. So
expose CoD2-honest flags (`--keep-shader/--keep-type/--keep-text`) not the CoD4 names.

**Stack-score multiplier:** CoD2's `+N` popup DOES exist via mods (IzNoGoD `_scorepopup.gsc`:
newclienthudelem + setvalue → HECmd_SetValue type=HE_TYPE_VALUE(2), float `value`, g_hudelem 649). Scale
= walk `ps.hud.current[]`+`.archival[]`, `if type==HE_TYPE_VALUE: value=round(value*mult)`. Gate to the
popup (label `+`/yellow color) so ammo/timer VALUE elems aren't touched. Mutate before the delta base.

**Split-by-map:** a demo CAN hold multiple maps (each map change = a 2nd+ `svc_gamestate` mid-stream,
SV_SpawnServer→gamestate; CL_ParseGamestate does CL_ClearState). Detect gamestate #2+ in the frame loop →
new out file, emit fresh SV_WriteGameState from cl.gameState, outSeq from 0. Low risk.
**Split-by-match:** fast_restart (sv_ccmds 21 "doesn't send a new gamestate") toggles
**SNAPFLAG_SERVERCOUNT (=4)** in every snapshot's snapFlags (sv_ccmds 64; q_shared 180) — THE reliable
detector (serverTime does NOT reset on fast_restart; `B`/`n` serverCommand is only corroboration). Add
`#define SNAPFLAG_SERVERCOUNT 4`. Medium risk (warmup restarts → many tiny segments; need min-length).
**Merge (EXPERIMENTAL, last):** keep A's gamestate, inject B's differing configstrings as mid-stream
svc_configstring (re-time CS_LEVEL_START_TIME=13), re-time B's snapshots forward, first B snap non-delta.
**Core limitation (why experimental):** svc_baseline is gamestate-only — B entities whose baseline
differs/new can't be re-based mid-stream → ghost/corrupt entities; + different clientNum POV; players are
in snapshot.clients not configstrings so they self-correct. Reuse cut's RetimePlayerstate/Entity +
ClearEntityEvents at the splice.

**Protocol convert:** near-noop for CoD2 — netField tables identical across 115/117/118/119/120; the
protocol# is a TEXT field in configstring[0] (`\protocol\..\shortversion\..`). "Convert" = rewrite those
keys + (for 119/120) gate the one STAT_IDENT_CLIENT_HEALTH branch (writer.h:356 "non-CoD2x path",
reader.cpp:1516). Warn on downgrade if a message exceeds the target MAX_MSGLEN (0x4000 for 1.0/1.2).

**Exe "unsafe" = unsigned + zero-reputation** (NOT infected). Free wins: (1) ship in a ZIP, Extract All
(strips download MOTW → no SmartScreen prompt); (2) embed a **VERSIONINFO resource + icon** via windres
(reduces Defender heuristic FPs on mingw static binaries); (3) win build use `-O2 -s` not `-g`; (4) "More
info → Run anyway"; (5) MS false-positive portal if quarantined. Code signing (~$10/mo Artifact Signing)
= the only permanent fix, not worth it for a free tool. windres on this host = via dockcross
(`i686-w64-mingw32.static-windres -O coff`), needs `sh -c` to chain windres+g++.

## DT3 cleanup (this commit)
- `SV_WriteSnapshot`/`SV_EmitPacketEntities` stripped to the pure --copy re-encode (dropped the dead
  `timeOffset`/`forceFull`/`clearEvents` params + branches — skip-dead/cut use SkipExtractFrame +
  SV_WriteSkipSnapshot, which kept their event-clearing). Re-verified --copy 0/0 after.
- Removed the disabled legacy CoD2-DemoParser `main_legacy()` block.
- README rewritten to match the actual feature set.

## P4 byte-1:1 progress (verify harness)
- **Fixed a --verify-only bug:** the loop never set `clc.serverMessageSequence = seq` (every other
  command loop does). With messageNum stuck at 0, `deltaNum = messageNum - deltaNumByte` went negative,
  so every delta snapshot decoded as non-delta against the wrong `from`. Fix drops false divergences
  ~80% (p115 3999->760, p117 729, p118 1017, cod2x 154).
- **Attribution now by byte offset (cursize), not msg->bit.** `MSG_WriteByte/Short/Long` advance cursize
  but NOT msg->bit, so bit markers go stale right after any byte-aligned write (e.g. a small-int float's
  trailing byte). Byte markers are reliable; use them to map the first divergent byte to entity+field.
- **Remaining divergences are a real non-canonical-encoding class, and the tool is semantically 1:1**
  on them (verified: --copy then --dump both -> every decoded field value identical; only the readcount
  prefixes shift by the few bits the re-encode differs). **Classification: of 760 diverged frames on p115,
  759 are DELTA frames diverging in PLAYERSTATE, 1 is non-delta diverging in entities.** Root cause: our
  encoder recomputes minimal-canonical change-bits by comparing from/to VALUES, but the SERVER marked
  fields changed off FULL-PRECISION state the demo doesn't carry. Proof: the first delta divergence
  (frame12) is at playerstate `viewangles[0]` (bits=-100 = **angle16, quantized**). angle16 round-trips
  perfectly (all 65536 shorts, 0 failures), so it is NOT a value bug — the original marked viewangles[0]
  CHANGED (wrote a short) while we wrote "unchanged" because our from==to (both quantized-equal). The
  server compares real-float viewangles and emits "changed" for sub-quantum moves even when the
  transmitted angle16 short equals the base's; we only have the quantized short, so change decisions can't
  be re-derived from values. The 1 non-delta entity case (trDelta[2] changed->zero) is the same class.
  **FIX = the P4 architectural pivot: the DECODER records the original per-field changed/lc decisions into
  the model and the ENCODER replays them verbatim, instead of recomputing minimal forms from value
  compares.** Substantial (thread "original encoding decisions" through decode->model->encode); design it
  before touching the codec. (An earlier "non-delta uses previous frame" hypothesis was wrong — it
  over-generalized from the single non-delta outlier; corrected by counting deltaNum of every diverged
  frame.)

# Demo corpus (byte-1:1 validation)

Immutable, hash-manifested demos spanning every protocol + recording path, used by
`--verify` as the regression suite for the 1:1 reverse. Kept here (NOT in `demos/`, which
a Windows<->WSL sync rotates). Large demo files are git-lfs / kept off-repo; `SHA256SUMS`
is the manifest of record.

## Current corpus (ALL verify 0-diverged, 176,549 frames total)

| file | protocol | notes |
|---|---|---|
| demo_115_1.0.dm_1 | 115 (1.0) | client demo |
| demo_117_1.2.dm_1 | 117 (1.2) | client demo |
| demo_118_1.3.dm_1 | 118 (1.3) | client demo |
| demo_cod2x.dm_1 | CoD2x client on a 118 server | wire = 118 |
| demo0011_118_server.dm_1 | 118 | long real-server demo (Verindra), map rotation |
| demo0012_118_server.dm_1 | 118 | 26,761 frames |
| demo0014_118_server.dm_1 | 118 | 57,854 frames, mid-stream gamestates |
| demo0015_118_server.dm_1 | 118 | 58,830 frames, mid-stream gamestates |

Protocol notes (from `Refrences/CoD2x/src/shared/shared.h`): **120 = CoD2x, wire format
identical to 118** (CoD2x never patches the delta codec — verified in its patch sources);
**119 = the Microsoft Store 1.3 re-release** (the STAT_IDENT_CLIENT_HEALTH variant) — no
demo available yet, parked.

Matrix (goal): {p115,p117,p118,p119} x {2-4 players, 20+ (bots ok)} x {stock, Verindra},
PATH A (sv_autorecord) + PATH B (mid-game recordDemo) fixtures, one svc_download trigger,
one map-rotation demo (DONE: demo0014/15), one killcam demo, the 41-player p115 corruption case.

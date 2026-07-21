# Demo corpus (byte-1:1 validation)

Immutable, hash-manifested demos spanning every protocol + recording path, used by
`--verify` as the regression suite for the 1:1 reverse. Kept here (NOT in `demos/`, which
a Windows<->WSL sync rotates). Large demo files are git-lfs / kept off-repo; `SHA256SUMS`
is the manifest of record.

Matrix (goal): {p115,p117,p118,p119,p120} x {2-4 players, 20+ (bots ok)} x {stock, Verindra},
PATH A (sv_autorecord) + PATH B (mid-game recordDemo) fixtures, one svc_download trigger,
one map-rotation demo, one killcam demo, the 41-player p115 corruption case.

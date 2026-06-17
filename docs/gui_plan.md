# CoD2-DemoTool — GUI front-end

> **Status: IMPLEMENTED** on branch `gui-win32` as an **ImGui** app (CoD2-menu themed) —
> `src/gui_imgui.cpp` + vendored `src/imgui/` + `build-win-gui.sh`. ImGui builds from WSL via dockcross on
> the **Win32 + OpenGL3** backend (DirectX is the only ImGui backend that can't mingw-cross-compile; we
> avoid it — no DX, no MSVC, no GLFW). The notes below were the original *plain-Win32-controls* plan, kept
> for context: the in-process `Cmd_*` dispatch, freopen stdout capture, per-op state hygiene, and the
> 6-operation design all carried over unchanged — only the rendering face is ImGui now. The CLI stays
> 100% intact.

## Context / why

The tool is a working `.dm_1` editor but **CLI-only**, which makes it unusable for non-CLI users on
Windows: double-clicking the console `.exe` flashes a window that prints usage and closes instantly, and
dragging a demo onto it "gives nothing" (it runs `--info` and prints to a console that immediately
disappears). Anomaly's CoD4 tool is the same kind of CLI with a nicer ASCII banner + a 5-second pause —
still "drag onto exe," not a real app.

**Goal:** a dead-simple **windowed** GUI — double-click opens a real window, drag a demo in (or Browse),
pick an operation from a dropdown, click Run, read the result. No flags, no console flash. The existing
CLI stays 100% intact for scripts/power use.

## Framework decision (and why NOT ImGui)

ImGui — the first instinct, used by the sibling IWXMVM project — runs on **DirectX 9 + the MS DXSDK,
which is MSVC-only and cannot cross-compile with the mingw toolchain** this tool already uses. Going
ImGui+DX would throw away the clean mingw/static-exe build for a few buttons. (ImGui on an OpenGL/GLFW
backend *can* mingw-build, but it adds GLFW + a GL loader + a bigger binary — heavier than needed.)

**Chosen: pure Win32 native controls.** Zero external dependencies, native drag-drop + file dialogs, a
tiny static exe, and it builds with the **exact same dockcross mingw toolchain** the CLI uses (verified:
`docker` + the `dockcross/windows-static-x86` image are present and build the current exe clean — no
install needed; the GUI exe can be compile-checked the same way, only the live window/drag test is done
on Windows).

## Approach

**Two exes from one codebase, same dockcross toolchain:**
- `cod2-demotool.exe` — existing **console** CLI, byte-for-byte unchanged (terminals/scripts).
- `cod2-demotool-gui.exe` — new **`-mwindows`** (GUI subsystem, *no console ever flashes*) double-click
  target.

Splitting is the clean fix for flash-and-close: a console exe always flashes on double-click; a GUI exe
has no stdout for CLI text. Two exes = the noob path (GUI) never shows a console, the CLI stays exactly
as-is. No `argc` sniffing, no `AllocConsole` gymnastics.

**Integration:** new `src/gui_win32.cpp` does `#define GUI_BUILD` then `#include "reader.cpp"`, so the
whole tool (all `Cmd_*`, globals, helpers) compiles into the GUI translation unit and is called
**in-process** — no shelling out, no output parsing. `reader.cpp`'s `main` is guarded out of the GUI build.

**State hygiene (the #1 correctness risk):** the CLI relies on the OS clearing globals per process. A GUI
calling `Cmd_*` repeatedly must reset the ~11 filter globals (`g_quietLog`, `g_dumpCommands`,
`g_removeChat/CenterText/WhiteText`, `g_removeHud`+`g_keepShader`, `g_scaleScore`+`g_scoreMult`,
`g_convertProtocol`, `g_collectEvents`) before each op. Decoder state (`cl`/`clc`/`demo`) already
auto-resets via `CL_ResetState` inside `FS_FOpenFileRead` (reader.cpp:2491). A new `ResetFilters()` owns
all 11 in one place, called unconditionally at the top of every Run.

**Output capture:** the GUI build has no console, so `printf`/result text goes nowhere by default.
Capture with the least-invasive method — `freopen(stdout)` to a temp file around each op, then read the
temp file into the log pane. **Zero edits to the ~133 `printf` sites.** (`Com_Printf` debug still goes to
the per-op `<demo>.log` files, unchanged.)

## Files

**CREATE — `src/gui_win32.cpp`** (the whole GUI, one file): `#define GUI_BUILD` + `#include <windows.h>`
+ `#include "reader.cpp"`; then `WinMain`, `WndProc`, control creation, `WM_DROPFILES`/Browse handlers,
the 6-op dispatch, and the `freopen`-based `RunOp()` capture wrapper.

**CREATE — `build-win-gui.sh`** (mirror of `build-win.sh`, local-mingw-else-dockcross), compiling
`src/gui_win32.cpp` (not `reader.cpp` — the GUI TU includes it) with `-mwindows`, linking
`-lcomdlg32 -lshell32 -lgdi32 -luser32`, reusing `src/win/version.rc` (windres) for metadata; output
`bin/cod2-demotool-gui.exe`. dockcross form:

```
i686-w64-mingw32.static-windres src/win/version.rc -O coff -o bin/version.o
i686-w64-mingw32.static-g++ -static -mwindows -O2 -s -Wno-write-strings \
  -DCOD_VERSION=COD2_1_3 src/gui_win32.cpp bin/version.o \
  -lcomdlg32 -lshell32 -lgdi32 -luser32 -o bin/cod2-demotool-gui.exe
```

`-lcomdlg32` = `GetOpenFileName`; `-lshell32` = `DragAcceptFiles`/`DragQueryFile`; `-lgdi32`/`-luser32`
explicit (usually auto-linked).

**EDIT — `src/reader.cpp` (2 surgical changes; CLI behavior unchanged):**
1. Wrap `main` (reader.cpp:4133+) in `#ifndef GUI_BUILD … #endif`.
2. Add `ResetFilters()` (zeros the 11 globals; `g_scoreMult=1.0f`, `g_keepShader=NULL`) under
   `#ifdef GUI_BUILD`, right after the global block (~line 167).

Reuse/generalize `HtmlPathFor()` (reader.cpp:4121) for default output names (`<name>_skipdead.dm_1`,
`_cut`, `_nohud`, `_clean`; Overview keeps `.html`).

**EDIT (optional) — `Makefile`:** add a `win-gui:` target delegating to `build-win-gui.sh`.

**No edits to:** `writer.h`, `declarations.hpp`, `icons_embed.h`, `build-win.sh`, or any `printf` site.

## The window (Win32 native controls)

Single fixed window (~560×440), `DragAcceptFiles(hwnd,TRUE)`. Controls:

- **Input path** (read-only EDIT) + **Browse** button (`GetOpenFileNameA`, filter `*.dm_1`).
- **Operation** dropdown (COMBOBOX) — curated to the 6 a noob wants:
  1. **Info** → `Cmd_Info(in)` (result to log, no output file)
  2. **Skip dead-time** → `Cmd_SkipDead(in,out,keepKillcam)` — *keep-killcam* checkbox
  3. **Cut to range** → `Cmd_Cut(in,out,startStr,endStr)` — two time fields (default `start`/`end`;
     accept `mm:ss`/secs)
  4. **Remove HUD** → `g_removeHud=1; Cmd_Copy(in,out)`
  5. **Clean text** → `g_removeChat/CenterText/WhiteText` from 3 checkboxes; `Cmd_Copy(in,out)`
  6. **Overview (HTML)** → `Cmd_Overview(in, HtmlPathFor(in))`, then `ShellExecute` to open it
  (Dump/Split/Merge/Scale-score/Convert/DeadScan stay CLI-only — hidden from the GUI.)
- **Per-op panel** shown/hidden on `CBN_SELCHANGE` (keep-killcam checkbox; two time fields; three clean
  checkboxes).
- **Output path** (EDIT) auto-filled beside the input, user-editable; hidden for Info.
- **Run** button (default) + **Result/log** (multiline read-only EDIT, vertical scroll).

`WM_DROPFILES`/Browse → fill input + recompute default output. `IDC_RUN` → validate input exists, gather
fields, `RunOp()` (freopen capture: temp file → `ResetFilters()` → set flags → `Cmd_*` → read temp into
log pane, `\n`→`\r\n` for the EDIT control).

ASCII sketch:

```
+-----------------------------------------------+
|  CoD2 Demo Tool                               |
| +-------------------------------------------+ |
| |   Drag a .dm_1 here   (or [ Browse ])     | |
| +-------------------------------------------+ |
|  Operation: [ Skip dead-time        v ]       |
|  [x] keep killcam                             |
|  Output:  game_skipdead.dm_1     [ Browse ]   |
|                  [    Run    ]                 |
| +-------------------------------------------+ |
| | skip-dead: kept 29739 frames, 12:24 ...   | |
| +-------------------------------------------+ |
+-----------------------------------------------+
```

## Phases / checkpoints

- **P0 — Branch:** `git checkout -b gui-win32` off `nightly`. CLI untouched throughout.
- **P1 — Coexistence scaffold:** guard `main`; add `ResetFilters()`; stub `gui_win32.cpp` (`WinMain` →
  hello MessageBox); `build-win-gui.sh`. *Verify (here):* dockcross compiles+links a PE32 *GUI* exe; the
  CLI exe still builds unchanged.
- **P2 — Window + controls (static):** full `WndProc`, all controls, panel show/hide, drop+Browse,
  default-output; Run echoes chosen args. *Verify build here; user verifies on Windows:* window opens, no
  console flash, drag/Browse fills path, dropdown toggles panels.
- **P3 — In-process dispatch + capture:** `RunOp()` freopen capture, `ResetFilters()` per op, 6-op switch.
  *User verifies on Windows:* Info shows version/map/length; Skip-dead writes output + summary; run two
  ops back-to-back (Clean → Info) to confirm **no filter leakage**.
- **P4 — Polish:** Overview opens HTML via `ShellExecute`; guard Run re-entrancy; friendly error
  MessageBox; window icon (reuse version resource). Propose tool CHANGELOG entry + version bump in
  `version.rc`.

## Risks

- **Filter leakage between in-process ops** (top risk) — `ResetFilters()` is the single owner of all 11
  globals, called unconditionally before each op; P3 explicitly tests back-to-back ops.
- **Console flash** — eliminated by the separate `-mwindows` GUI exe.
- **stdout capture with no console** — `freopen` to a temp file; detach via `freopen("NUL",…)` before
  reading; no `Cmd_*` conflicts (they use named `fopen` log files).
- **Build-but-can't-run-here** — compile+link verifiable via dockcross; all runtime UX is user-tested live
  on Windows. Phases are structured so each "verify here" gate is a clean build and each "user verifies"
  gate is a concrete double-click/drag action.
- **AV/SmartScreen on a 2nd unsigned exe** — same posture as the existing exe (version metadata + `-s`
  strip); inherits the README "Windows trust" note.

## Verification

1. **Here (dockcross):** `./build-win-gui.sh` → clean compile+link; `file bin/cod2-demotool-gui.exe` =
   PE32 GUI; `./build-win.sh` still builds the unchanged CLI exe; `git diff` on `reader.cpp` shows only
   the `main` guard + `ResetFilters`.
2. **User (Windows):** double-click → window, no console flash; drag `.dm_1` → path fills; Info → correct
   version/map/length; Skip-dead (±keep-killcam) → output file + summary; Cut `1:30`/`3:00`; Clean (chat
   only) then Info on the original → no leakage; Overview → HTML opens; Browse as a drag alternative.

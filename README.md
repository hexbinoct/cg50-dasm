# DASM — an SH-4A disassembler that runs on the fx-CG50

DASM is a gint add-in that turns the Casio fx-CG50 into its own reverse-engineering tool.
Pick any add-in (`.g3a`) in storage memory, or the calculator's OS ROM itself, and browse
it as a colour disassembly listing. Follow calls and jumps, then come back. Read the literal
pools, see which OS syscall is being called, and switch to a hex dump or a strings list.

It runs on the real calculator and on our desktop emulator (the parent project,
`casio-cg50`). All the screenshots below come from the emulator running a full dump of a real
fx-CG50, and show DASM disassembling itself.

| | |
|---|---|
| ![File picker](docs/img/picker.png) | ![Disassembly listing](docs/img/listing.png) |
| **File picker:** every `.g3a` in storage memory, with sizes | **Listing:** address · opcode · instruction · comment |
| ![Hex view](docs/img/hex.png) | ![Header pane](docs/img/header.png) |
| **Hex view (F3):** synced to the listing, with ASCII | **Header (F5):** name, version, sizes, checksum, the add-in's icon |

## What it can do

- **Open anything:** every `.g3a` in `\\fls0` (read through the OS's BFile calls, cached in
  4 KB pages), or the **OS ROM** mapped at `0x80000000` (F6).
- **Full SH-4A decoding:** the whole integer, system and FPU set plus the SH-4A additions
  (`movli`/`movco`, `movua`, `icbi`, `prefi`, `synco`, banked registers). The decoder was
  checked against the parent project's Python disassembler over every one of the 6.3 million
  instruction words in OS 3.60, with no unexplained differences.
- **Literal pools resolved:** `mov.l @(disp,pc),rN` shows the value it loads
  (`;=0x08105f14`), and `mov.w` shows it in decimal too.
- **Calls and jumps followed:** EXE on a `bsr`/`bra`/`bt`/`bf` jumps to the target, and EXIT
  comes back (32 levels of history). `jsr @rN`/`jmp @rN`/`braf`/`bsrf` are resolved by
  scanning back for the load of `rN`.
- **Syscalls named:** OS syscall handlers are named from the running OS's own syscall table,
  so it fits whatever OS version is installed (273 names from libfxcg). An add-in's syscall
  trampolines are resolved to the syscall's name.
- **Readable colours:** calls, branches, returns and literals have their own colours, and delay
  slots are indented.
- **Hex view (F3), strings (F4), header (F5), go to address (F2).**
- **Font setting (OPTN):** switches the listing font, described below.

## Font setting: OPTN

DASM has two listing fonts. **OPTN** switches between them at any time, and the status bar
confirms which one is active.

| | Large, smooth (default) | Small |
|---|---|---|
| Font | Anti-aliased DejaVu Sans Mono, 7×11 cells | The original 1-bit 5×7 font, 6×8 cells |
| Screen | 56 columns × 17 rows | 64 columns × 24 rows |
| Hex view | 8 bytes per row | 16 bytes per row |

![Small font](docs/img/small-font.png)

gint fonts are 1-bit, so the large font is drawn by DASM itself. `tools/make_font.py` renders
the 95 printable ASCII glyphs to 8-bit coverage in `src/font_aa.h`, and `aa_text()` in
`src/main.c` blends them into VRAM over whatever is behind them (row highlight, title bar).

## Keys

| Key | In the listing |
|---|---|
| UP / DOWN | previous / next instruction |
| LEFT / RIGHT | previous / next page (SHIFT: ±0x1000) |
| `+` / `-` | shift the listing by one byte |
| EXE | follow the branch, call or literal under the cursor |
| EXIT | go back (history), or to the file picker |
| F1 | open another file (the picker) |
| F2 | go to an address: digits, F1–F6 = A–F, DEL, EXE |
| F3 / F4 / F5 | hex view / strings from here / header |
| F6 | browse the OS ROM |
| OPTN | switch the font (large smooth / small) |
| MENU | back to the calculator's MAIN MENU |

## Building

The add-in uses [gint](https://gitea.planet-casio.com/Lephenixnoir/gint) 2.11 and the fxSDK.

- **Windows (office):** in the parent project's Docker image, from PowerShell:
  `docker run --rm -v "<path-to-dasm>:/work" fxsdk:latest fxsdk build-cg`.
- **macOS (home):** `fxsdk build-cg` with the native toolchain. fxconv needs Pillow in the
  first `python3` on the PATH (see `NOTES.md`).

The output is `DASM.g3a`, which is committed. Helper scripts:

| Script | What it does |
|---|---|
| `python make_icons.py` | the menu icons |
| `python3 tools/make_font.py` | regenerates `src/font_aa.h` and writes a preview |
| `python tools/fetch_syscalls.py` | the syscall names |
| `tools/verify_decoder.py` | the decoder cross-check |
| `python3 tools/readme_shots.py` | these screenshots |

## Testing

1. **Desktop emulator first.** In the parent project, the `dasm_swap_test.go` and
   `dasm_real_test.go` probes (tag `probe`) run a fresh build. The second one uses the full
   32 MB flash dump: DASM starts from its real menu icon and reads the real storage. With
   `DASM_KEYS` you can script any key sequence and get a screenshot per key.
2. **Then the real calculator.** Copy `DASM.g3a` to the calculator in USB mass-storage mode
   and start it from the MAIN MENU.

## Status and next steps

Verified on the real calculator and on the emulator, including reading every add-in's contents.
Next, in order (see `NOTES.md`): function detection, cross-references ("who calls this?"),
bookmarks and comments saved to a side file, and an overview bar of the whole file.

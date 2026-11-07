# Hail to the King — technical notes

Notes on the game and on how this port works. File and RAM addresses are for the USA release (Redump).

## 5. Key facts about the game

- **SDK:** Sega Katana (not Windows CE). `IP.BIN` says `SEGAKATANA`; build stamp "DC build: Dec 11 2000".
- **Compiler (from LNK.TMP):** Hitachi-mode GCC, `-m4-single-only -ml -O2 -ffast-math` → FPU almost always single precision.
- **Libraries linked (LNK.SUB):** Shinobi (system), Kamui2 (graphics), CRI Middleware (Sofdec video, ADX, `sg_mw2k2`, `sg_sd2`), Audio64 (sound), Duck TrueMotion (`.AVI` cutscenes), `sh4nlfzz` math.
- **Game source modules:** cross-platform engine `x*.o`, Dreamcast layer `dc*.o`, script interpreter `J*.o` (`JInterpr`, `JThread`, …), `My*.o` (file/DMA/IRQ).
- **Memory:** loads at `0x8C010000`; entry stub sets CCR then jumps to `0x8C05BEF8`. Code ≈ `0x8C010000–0x8C0BE000`; CRI middleware first, game ≈ `0x8C05D000–0x8C0A5000`, Audio64 ≈ `0x8C0A5E00–0x8C0AE...`.
- **BIOS vectors used:** `0x8C0000B0` sysinfo, `B4` font, `B8` flash, `BC` GD-ROM (stubs at `0x8C0AF20C+`, r7 = function, r6 = super), `E0` system (r4=1 → exit to BIOS menu).
- **Disc layout:** HD area track 3 at LBA 45000 (FAD 45150), track 4 audio, track 5 data from LBA 48912 (index 01 at +225). FAD = ISO LBA + 150. Movies (`*.SFD`, ~1 GB) live on track 5.
- **Boot file order observed:** fs metadata → `AUDIO64.DRV` → `FONT5.PVR` → `DBFONT24.PVR` → `ED.INI` → `AA01.AVI` → `LEVELA.JDC` → `AZ01.DC` (3 MB) → `AA01.DC` → `AA01V.DC`.

## 6. How the recompiler works (and its non-obvious rules)

- Each function → `void f_XXXXXXXX(sh4ctx *c)`. Intra-function branches → `goto`; known calls → direct C calls; indirect calls/jumps → `sh4_dispatch()` (binary search in `fntable.c`; unknown targets logged to `missing_entries.txt`).
- **Delay slots:** branch condition / target captured first, slot executed, then branch.
- **Jump tables:** GCC pattern `cmp/hi|cmp/hs` + `mova` + `mov.b|w @(r0,rI)` + `braf`; bound found path-aware; index bias (`add #k`) handled; `braf` with a `mov.w` constant = long branch.
- **Function discovery:** recursive descent + pointer scan (lenient prologue: `sts.l pr,@-r15` or `mov.l r14,@-r15` in first 8 insns, scheduler-interleaved) + gap pass + leaf pass + `notes/extra_entries.txt`.
- **Entry rule:** if a function owns code below its entry address, emit `goto L_entry;` first (bug fixed 2026-10-18; affected 118 functions).
- **Literals are baked as constants** — except those in `notes/live_literals.txt`, which are read from RAM. The runtime checks every RAM write into the code range against `litmap.c` and logs hits to `live_literals.txt` (Shinobi stores its interrupt-stack pointer in a literal; Kamui relocates code near `0x8C03F1E8–0x8C040308`).
- **Overlays:** code the game writes at runtime (exception trampolines at `VBR+0x100/0x400/0x600 = 0x8C00F500/F800/FA00`) is decoded from `notes/overlays/<addr>.bin` snapshots; overlay literals are always read live. Snapshot was taken with `DC_DUMPRAM=<file>`.
- **Interrupts:** `SH4_POLL` at loop back-edges → every 4096 polls check host clock → raise Holly vblank (IRL 6/4/2 → INTEVT 0x320/0x360/0x3A0) or TMU underflow; runtime performs SH-4 acceptance (SSR/SPC/SGR, MD/RB/BL) and calls `VBR+0x600`; `rte` returns to the C caller (SPC sentinel `0xFFFFFFF0`).

Debug aids: `DC_TRAP_ROM=1` (SIGTRAP on BIOS-ROM reads, run under gdb), `DC_DUMPRAM=<file>` (dump 16 MB RAM on exit), exit report lists most-accessed unmodelled registers, disc sector counts, PVR counters.


## 7. Models and data formats
- **F8 capture** (`render_gl.c capture_frame`): un-projects the TA vertices (z = 1/w; focal 566 px on 640x480, measured from Ash's shadow being flat), then back to world with the camera at 0x8C72D388 (view = world*R + t; view X right, Y up, Z depth, left-handed -> OBJ Z negated). Writes captures/capture_NNN.obj/.mtl/_texNN.png. Headless test: `DC_CAPTURE_FIELD=N`.
- **.DC container**: 0x14: data size, 0x14: count at +20, chunk table at 0x18 of {u32 type, u32 flag (1 = VRAM texture block), u32 offset, u32 size}. type = (kind<<23) | (object<<12) | extra. Ash = object 0x24 in AZ00/AZ01/BZ01: kind 0 = mesh, 1 = skeleton, 2 = ?, 3 = animations (58).
- **Mesh chunk**: u32 nparts, nparts u32 offsets; part = {0, nv, nv, ntri} + nv*(x,y,z,u,v f32) + nv*normal(3 f32) + nv*u16 bone + ntri*3 u16 indices. Vertices are bone-local. Part 0 = body; Ash's parts 1-13 are alternate hands (bones 6/7, 9/10).
- **Skeleton chunk**: u32 type, u32 nbones, u32 ?, then per bone 4x4 local matrix (16 f32) + translation (3 f32) + i32 parent. Bind world = L (with row 3 = translation) * world[parent]. Rebuilds Ash in T-pose correctly (tools/models/dcmodel.py).
- Open: which flag-1 texture chunk / TCW each mesh uses; other object kinds (enemies) may use other kind numbers; animation format.
- **Textures** (flag-1 chunks): 16-byte prefix (first word = table type) + PVRT block (pixel fmt byte 8: 0 1555, 1 565, 2 4444, 3 YUV422; data fmt byte 9: 1 twiddled, 9 rectangle, 13 twiddled rectangle). A character's texture = the flag-1 chunk with the same object number. Formats seen: YUV rect (109), YUV twiddled (26), 1555 tw-rect (21), 4444 tw-rect (2).
- **Character rule** (models.c `models_scan`): mesh chunk with kind even and extra = 0, skeleton at type + (1<<23), and a texture with the same object -> 35 characters across the disc (most in AZ00.DC). Kind 22 meshes (0xB...) are small multi-quad things (HUD/effects?), not characters.
- **Build 14**: launcher Extras > Models (models.c + options_ui.c, SDL_RenderGeometry, painter-sorted, UVs shifted into 0..1 per triangle since SDL can't repeat). Names are descriptions by look (mnames table). Alternate parts (Ash's 13 hands) hidden except the first per bone set; Enter shows all.


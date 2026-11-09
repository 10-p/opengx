# opengx `ut99` branch: changes against upstream

Base: devkitPro/opengx `23c12e6` (v0.17.0-1). Every change is a separate commit on `ut99`, written so it could be
sent upstream on its own. Licence unchanged (BSD-3-Clause, see the file headers).

## Texture environment

### `GL_RGB_SCALE` / `GL_ALPHA_SCALE` (1, 2, 4)
- `glTexEnv{if}(GL_TEXTURE_ENV, GL_RGB_SCALE|GL_ALPHA_SCALE, v)` is stored per texture unit; values other than
  1, 2, 4 give `GL_INVALID_VALUE` (as the spec requires).
- GX mechanism: the TEV stage that computes the unit's `GL_COMBINE` result gets the scale as its output scale
  (`GX_SetTevColorOp`/`GX_SetTevAlphaOp` `GX_CS_SCALE_1/2/4`). The TEV scales before clamping, the same order
  as OpenGL. Like the spec, the scale only applies in `GL_COMBINE` mode.
- Bug fix found on the way: `setup_texture_stage()` tested `if (!tu->mode != GL_COMBINE)`, which is always true,
  so the generic `GX_TEV_ADD`/`GX_TB_ZERO` op overwrote what `setup_combine_operation()` had set. That broke
  `GL_SUBTRACT` (ran as ADD) and `GL_ADD_SIGNED` (lost its -0.5 bias), and would have dropped the new scale.

## Textures

### `GL_EXT_paletted_texture` → `GX_TF_CI8` + TLUT
- New: `glColorTableEXT`, `glColorSubTableEXT`, `glGetColorTableParameterivEXT` (WIDTH/FORMAT) on
  `GL_TEXTURE_2D` (per texture object; `GL_SHARED_TEXTURE_PALETTE_EXT` is not supported).
  `glTexImage2D(..., GL_COLOR_INDEX8_EXT, w, h, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, indices)` (any level,
  `glTexSubImage2D` too) stores the indices as `GX_TF_CI8` (8x4-texel tiles, a byte copy). The extension is in
  `GL_EXTENSIONS`.
- Palette storage: main-memory GX TLUT, 32-byte aligned, padded to a multiple of 16 entries, 1..256 entries
  (power of two, as the extension requires). The TLUT format follows the palette's internal format:
  - anything with alpha (`GL_RGBA`, `GL_RGBA8`, `GL_RGB5_A1`, …) → `GX_TL_RGB5A3`: entries whose alpha rounds
    to 7/7 are stored opaque as RGB555, the others as A3 RGB444. So 1-bit masks (UT's index 0 = transparent)
    are exact, opaque colours lose 3 bits per channel, translucent entries lose 4 bits of colour and keep 3 bits
    of alpha.
  - `GL_RGB*` → `GX_TL_RGB565` (opaque, 5/6/5 bits).
  - `GL_LUMINANCE*`, `GL_LUMINANCE_ALPHA*`, `GL_INTENSITY*`, `GL_ALPHA*` → `GX_TL_IA8` (8-bit intensity and
    alpha, lossless for those).
  Source formats: `GL_RGBA`, `GL_BGRA`, `GL_RGB`, `GL_BGR`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA`,
  `GL_INTENSITY`, `GL_ALPHA` with `GL_UNSIGNED_BYTE`.
- TMEM management: libogc's default TMEM layout has 16 TLUT regions of 256 entries (`GX_TLUT0-15`). When a
  CI8 texture is bound to a TEV stage, `_ogx_tlut_bind()` looks the palette up in a 16-entry LRU table of
  resident palettes (keyed by a serial number that changes on every palette update); on a miss it
  `GX_LoadTlut()`s it into the least recently used region. The textures of the current draw are the most
  recently used, so their regions are never evicted by one another.
- Palette updates call `GX_DrawDone()` first, because draws still in the FIFO may have to load the old
  palette from RAM.
- GX cannot interpolate between mip levels of colour-index textures, so `GL_*_MIPMAP_LINEAR` min filters
  fall back to `*_MIPMAP_NEAREST` for CI8.
- Bugs fixed on the way:
  - libogc 3.2.1's `GX_GetTexBufferSize()` sizes `GX_TF_CI8` like a 16-bit format: 2x the memory and wrong
    mip offsets. opengx now computes CI8 sizes as I8 (same tile layout).
  - `glTexImage2D` reused the old texel buffer when only the format changed (same size), overflowing it when
    the new format is bigger (e.g. RGB565 → RGBA8). It now reallocates when the buffer size changes, and it
    resets the "alpha texture" flag on re-specification.
  - `update_texture()` flushed only `width*height` bytes from the start of the level, which misses
    sub-images not at the origin (the tiled layout spreads them over the whole level). It now flushes the
    whole level. (Invisible in Dolphin, which does not emulate the data cache.)
  - `ogx_get_proc_address()` now also strips an `EXT` suffix, and first looks the name up unchanged (for
    functions that only exist with the suffix, like `glColorTableEXT`).

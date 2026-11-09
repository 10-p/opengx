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

### `GL_EXT_texture_compression_s3tc` (DXT1 only) → `GX_TF_CMPR`
- New: `glCompressedTexImage2D` / `glCompressedTexSubImage2D` (also reachable as `...ARB`) for
  `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` and `GL_COMPRESSED_RGBA_S3TC_DXT1_EXT`, any level (levels smaller than
  8x8 go into one padded 32-byte tile). `imageSize` must be exactly `ceil(w/4)*ceil(h/4)*8`
  (else `GL_INVALID_VALUE`). Sub-images must be 4-aligned.
- **DXT3 and DXT5 → `GL_INVALID_ENUM`**: GX has no explicit/interpolated alpha block format (CMPR is DXT1
  only). The extension string is advertised anyway because DXT1 is what matters; applications must not assume
  DXT3/5 work.
- Repacking (CPU, ~a memcpy): the 8-byte blocks are reordered from raster order into 32-byte CMPR tiles of 2x2
  blocks (top-left, top-right, bottom-left, bottom-right; tiles in raster order); the two RGB565 endpoint
  colours are byte-swapped to big-endian; in each of the four index bytes (one per texel row) the four 2-bit
  indices are mirrored, because DXT1 keeps the leftmost texel in the low bits and CMPR in the high bits. Index
  meanings and the `c0 <= c1` three-colour + transparent mode are the same. Verified texel by texel
  (16x16 texture with a distinct index pattern per block, and a black/white block).
- Decode differences GX vs the S3TC spec (measured in Dolphin, to confirm on hardware): the 4-colour mode
  interpolates with 5/8 + 3/8 weights instead of 2/3 + 1/3 (white/black → 159 and 95 instead of 170 and 85),
  and in 3-colour mode the transparent texel keeps the `(c0+c1)/2` colour instead of black.
- `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` must not have transparent texels: the texture is flagged `is_opaque` and
  the TEV setup reads that unit's texture alpha as a konst 1.0 (`GX_TEV_KASEL_1`) in every env mode, including
  `GL_COMBINE` sources. The colour of such texels is still `(c0+c1)/2`, not black (GX limitation).

### `GL_OGX_native_texture`: pre-tiled GX texture data, zero conversion
Header: `#include <GL/gxext.h>` (installed next to `GL/gl.h`). The enums are above 0xFFFF, a range Khronos
never assigns; the low 4 bits are the `GX_TF_*` value.

| Internal format (`glCompressedTexImage2D`) | value | GX format |
|---|---|---|
| `GL_TEXTURE_GX_I8_OGX` | `0x0F6E0001` | `GX_TF_I8` |
| `GL_TEXTURE_GX_IA8_OGX` | `0x0F6E0003` | `GX_TF_IA8` |
| `GL_TEXTURE_GX_RGB565_OGX` | `0x0F6E0004` | `GX_TF_RGB565` |
| `GL_TEXTURE_GX_RGB5A3_OGX` | `0x0F6E0005` | `GX_TF_RGB5A3` |
| `GL_TEXTURE_GX_RGBA8_OGX` | `0x0F6E0006` | `GX_TF_RGBA8` |
| `GL_TEXTURE_GX_CI8_OGX` | `0x0F6E0009` | `GX_TF_CI8` (palette via `glColorTableEXT`) |
| `GL_TEXTURE_GX_CMPR_OGX` | `0x0F6E000E` | `GX_TF_CMPR` |

| `glColorTableEXT` type | value | table contents |
|---|---|---|
| `GL_TLUT_GX_IA8_OGX` | `0x0F6E0100` | big-endian 16-bit entries, `A<<8 \| I` |
| `GL_TLUT_GX_RGB565_OGX` | `0x0F6E0101` | big-endian 16-bit RGB565 entries |
| `GL_TLUT_GX_RGB5A3_OGX` | `0x0F6E0102` | big-endian 16-bit RGB5A3 entries |

With a `GL_TLUT_GX_*_OGX` type the `format` argument is ignored and the TLUT format is the type's; the
internal format is only recorded. The entries are copied as they are.

**Upload:** `glCompressedTexImage2D(GL_TEXTURE_2D, level, GL_TEXTURE_GX_xxx_OGX, w, h, 0, imageSize, data)`:
opengx allocates (or reuses) the 32-byte-aligned GX buffer, `memcpy`s, `DCFlushRange`s, and sets up the
`GXTexObj`. No conversion at all. `imageSize` must be either
- exactly the size of that level (below), or
- for `level == 0` (and w, h ≥ 2), the size of a whole chain of n ≥ 2 levels: the whole chain is uploaded in
  one call and the texture gets levels 0..n-1.

Any other size → `GL_INVALID_VALUE`. Mipmapped textures must have power-of-two sizes (GX rule).

**Byte layout** (what the PC cooker must produce; reference tilers are in
`tests/opengx-features/main.cpp`: `gx_tile`, `gx_tile_rgba8`, `gx_tile_cmpr`):
- The image is cut into tiles of 32 bytes (64 for RGBA8). Tiles are stored left to right, then top to bottom
  (row 0 = top row of the image = first row of the GL data, i.e. the same row order as `glTexImage2D` data).
  Partial tiles at the right/bottom edge are stored whole (pad with anything, zeros recommended).
- Inside a tile, texels are in raster order (left to right, then top to bottom), except CMPR and RGBA8.
- 16-bit values are big-endian.

| Format | Tile (texels) | Texel encoding |
|---|---|---|
| I8 | 8x4, 1 byte/texel | intensity |
| CI8 | 8x4, 1 byte/texel | palette index |
| IA8 | 4x4, 2 bytes/texel | byte 0 = alpha, byte 1 = intensity |
| RGB565 | 4x4, 2 bytes/texel | `RRRRRGGG GGGBBBBB` big-endian |
| RGB5A3 | 4x4, 2 bytes/texel | bit 15 set: `1RRRRRGG GGGBBBBB` (opaque, 5:5:5); bit 15 clear: `0AAARRRR GGGGBBBB` (3-bit alpha, 4:4:4). GX expands a 3-bit alpha `a` to `a<<5 \| a<<2 \| a>>1` |
| RGBA8 | 4x4, **64 bytes**: bytes 0-31 = 16 (A, R) pairs, bytes 32-63 = 16 (G, B) pairs, both in raster order in the tile | |
| CMPR | 8x8 = 2x2 DXT1-style blocks of 8 bytes: top-left, top-right, bottom-left, bottom-right | per block: colour0 (big-endian RGB565), colour1 (big-endian RGB565), then 4 bytes, one per texel row top to bottom, each holding the 4 texels' 2-bit indices with the **leftmost texel in bits 7-6**. `colour0 > colour1`: indices 0,1 = the colours, 2 = 5/8 c0 + 3/8 c1, 3 = 3/8 c0 + 5/8 c1; otherwise 2 = (c0+c1)/2, 3 = transparent |

Level sizes: `ceil(w/tw) * ceil(h/th) * 32` bytes (`*64` for RGBA8), with `tw x th` the tile size above
(`GX_GetTexBufferSize()`, except for CI8 where libogc is wrong: CI8 is sized like I8). In a chain, level k
(`max(w>>k,1) x max(h>>k,1)`) starts right after level k-1.

## Framebuffer

### `glReadPixels`: GL orientation and a fast path
- Bug fixed: the EFB copy rectangle used the GL `y` (bottom-left origin) as the EFB top coordinate, so
  `glReadPixels(x, y, ...)` read the rows mirrored around the window's middle (the rows inside the buffer were
  already written bottom-up correctly). The source rectangle is now `top = window_height - y - height`, with the
  viewport height as the window height (the same convention as the scissor code). Applies to every format.
- Fast path for `GL_RGBA`/`GL_RGB` + `GL_UNSIGNED_BYTE` (no skip pixels/rows, no FBO read target): one
  `GX_CopyTex` of the rectangle into an RGBA8 buffer, then a de-tiling loop that reads the AR and GB halves of
  each 4x4 tile two texels per 32-bit word and writes rows bottom-up; honours `GL_PACK_ALIGNMENT` and
  `GL_PACK_ROW_LENGTH`. 640x480 RGBA: 69 ms → 6.8 ms (emulated).
- `OPENGX_READPIXELS` env var: `copy` (EFB copy only), `peek` (`GX_PeekARGB` per pixel), `auto` (default):
  copy, and if the copy did not reach RAM fall back to peek. "Did not reach RAM" is detected by pre-zeroing the
  first tile's alpha bytes: a copy from a `GX_PF_RGB8_Z24` EFB always writes alpha 0xff. That case only
  happens in emulators (Dolphin's "Store EFB copies to texture only").

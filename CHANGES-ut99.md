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

### Combiner extensions in `GL_EXTENSIONS`
- `GL_ARB_texture_env_combine`, `GL_EXT_texture_env_combine`, `GL_ARB_texture_env_add`, `GL_EXT_texture_env_add`
  are now advertised (the functionality was there; with `GL_RGB_SCALE` it is now complete enough for the usual
  lightmap paths). All combine functions of the ARB extension (REPLACE, MODULATE, ADD, ADD_SIGNED, INTERPOLATE,
  SUBTRACT), all sources and operands, and the scales work. Known limits, all due to one TEV stage per unit:
  `ONE_MINUS_*` on both arguments of MODULATE/ADD, on arg0/arg1 of INTERPOLATE or on arg0 of SUBTRACT, and
  more than one `GL_CONSTANT` per stage, are not supported (warning). `GL_DOT3_*` is not implemented, so
  `GL_ARB_texture_env_dot3` is not advertised.

### `GL_ATI_texture_env_combine3` (`GL_MODULATE_ADD_ATI`, `GL_MODULATE_SIGNED_ADD_ATI`)
- New `GL_COMBINE_RGB`/`GL_COMBINE_ALPHA` functions `arg0 * arg2 + arg1` (and `- 0.5`), advertised as
  `GL_ATI_texture_env_combine3`. D3D's three-operand stage ops (`MULTIPLYADD`, `MODULATEALPHA_ADDCOLOR`, ...)
  map onto it.
- GX mechanism: one TEV stage, `d + (a(1-c) + bc)` with `a = 0, b = arg0, c = arg2, d = arg1` (bias
  `GX_TB_SUBHALF` for the signed form); a complemented `arg2` or `arg0` is taken by swapping the interpolated
  pair (`arg0 * (1 - arg2)` = `a = arg0, b = 0, c = arg2`).
- Limits (warning): `arg1` cannot be complemented (it is `d`), nor both factors at once; `GL_MODULATE_SUBTRACT_ATI`
  (`arg0 * arg2 - arg1`) has no one-stage form (the TEV subtracts from `d`) and is drawn as `arg0 * arg2`.
- Check: `j.combine3` (the three operand forms, the signed form and the alpha half, within 1-2 of exact).

## Buffer objects

### Rewriting a buffer drawn before the last swap no longer hangs; 4096 buffer names
- **Upstream bug (hang):** `glBufferData`/`glBufferSubData` with data into a buffer that had been drawn waited
  with `while (GX_GetDrawSync() < last_sync_token_sent)`. Draw sync tokens restart from 0 at every
  `ogx_prepare_swap_buffers()`, so a buffer drawn late in one frame (token 200, say) and rewritten in the next,
  whose draws had sent fewer tokens, waited forever. UC2GLDrv's dynamic vertex pool hung in its second frame.
  The buffer now keeps the swap count of its token and the wait is for that FIFO point
  (`_ogx_fifo_point_passed()`, shared with the deferred texture frees); the FIFO is flushed before waiting so
  the token reaches the GPU. The "can this storage be freed now" checks use the same test.
- **`MAX_VBOS` 256 → 4096** (`VboType` is 16 bits): a UE2 level has a vertex buffer per static mesh, BSP section
  and skeletal mesh, far more than 256. The name table is 16 KB.
- Check: `k.vbo` (1500 buffers drawn at once, each its own colour; a buffer drawn after 200 tokens, rewritten
  after a swap: no hang, the new data, 0.03 ms).

## Vertex submission

Numbers: `tests/opengx-bench` section (k) (Dolphin 2512, Null backend, emulated time; a 71x72-vertex grid, 9940
triangles, 5112 vertices of 44 bytes: position, normal, RGBA8 colour, two texture coordinate sets; lit with
`GL_COLOR_MATERIAL`, textured; "submit" is the time spent in the GL calls, FIFO the bytes the CPU wrote for one
99-triangle draw).

### Indexed GX arrays for `glDrawElements` / `glDrawRangeElements`
- The vertex loader fetches the attributes itself: `GX_SetArray()` per attribute, `GX_INDEX8` (an index range of
  at most 255 vertices) or `GX_INDEX16` in the vertex descriptor, and the FIFO receives one index per attribute
  and vertex (written as 32-bit stores of repeated indices) instead of the attribute values read and written one
  by one through a virtual call per attribute.
- Buffer object data is used in place. Client arrays may be changed as soon as the call returns, so the index
  range the draw uses is copied into a 1 MB ring buffer (16 parts, each fenced by a draw sync token when the
  copies move past it, reused once the GPU passed it; `DCStoreRange` on the copy); the attributes of an
  interleaved vertex share one copy. Float colours (`glColorPointer(4, GL_FLOAT)`, UT99's NOpenGLDrv) are
  converted to RGB8/RGBA8 while copying, with the same conversion as the direct path.
- Indices are rebased to the smallest index drawn, so the GX limits apply to a draw's range only (index 0xffff,
  or 0xff with `GX_INDEX8`, means "skip this vertex"; at most 65534 vertices in a range); 8-, 16- and 32-bit
  index arrays all work this way.
- What stays on the direct path, per attribute (the others of the draw stay indexed: GX mixes direct and indexed
  attributes in one vertex): positions with four components (the w divide), byte/int/double data, colours of
  one or two components or of non-float, non-byte types, constant attributes (point sprite coordinates) and
  software-generated texture coordinates (sphere map). The whole draw stays direct with GLSL programs, in
  selection mode, for the flat pass of the stencil emulation, when the range is more than twice the vertex count
  plus 64 (copying would cost more than it saves), and above 65534 vertices.
- Strides stay those of the arrays (the GX limit is 255 bytes, which opengx's array state already has).
- Bench (before -> after, ms submit per frame): client arrays, one draw 12.21 -> 1.93; 100 draws 12.31 -> 2.65;
  static buffer objects, one draw 6.60 -> 1.65 (the buffer path already used `GX_INDEX16`, but through the
  per-vertex reader calls); 100 draws 6.77 -> 1.97; UT99-like batches (200 x 50 triangles from one refilled
  float-colour buffer, two texture units) 12.52 -> 5.55 (1.1 of it the refill). FIFO bytes per 99 triangles:
  11904 -> 1600 (client), 3104 -> 1600 (buffer objects).
- Check: `o.indexed` (a 20x20-quad mesh with vertex colours and two texture coordinate sets, unlit and lit:
  16-bit/32-bit/8-bit indices, `glDrawRangeElements`, one draw per row (`GX_INDEX8`), triangle strips, separate
  arrays with a 3-component texture coordinate, float colours, 4-component positions (mixed direct + indexed),
  buffer objects, glDrawArrays, immediate mode, line loops: every path gives the same pixels). The whole suite's
  checked pixels and read-back regions (an FNV checksum of each) are identical to the build before.

### Indexed GX arrays for `glDrawArrays`; large draws in chunks
- `glDrawArrays` goes through the same path with the ascending indices `first..first+count-1` (rebased to 0):
  GX has no non-indexed array draw, and the 16-bit indices cost less than the attribute values. It beats the
  direct path down to a single 4-vertex fan (1000 one-quad draws: 3.06 -> 2.92 ms; the classification of the
  arrays into indexed/direct/copy groups is cached until the arrays change). Immediate mode (`glBegin`/`glEnd`)
  stays direct: its vertices are sent once anyway.
- A draw whose copies exceed 256 KB is split into chunks of whole primitives (points, lines, triangles, quads),
  each copied into the ring and drawn on its own (`GX_SetArray` per chunk). A chunk whose indices span much more
  than its vertex count is sent directly (vertex descriptor switched for that chunk only). Strips and fans that
  large stay on the direct path.
- **Upstream bug (hang):** a draw of more than 65535 vertices wrapped `GX_Begin()`'s 16-bit vertex count and the
  GP desynchronised (the feature test hung). List primitives are now always drawn in chunks of at most 65534.
- Bench (ms submit per frame, before -> after): de-indexed mesh (29820 vertices), client arrays one draw 11.86 ->
  3.11, 100 draws 11.95 -> 3.20; static buffer objects one draw 6.25 -> 1.32, 100 draws 6.41 -> 1.57; the older
  "10000 tris" lines: glDrawArrays 9.05 -> 2.32, glDrawElements 9.40 -> 2.98, 50 calls 8.62 -> 2.22.
- Check: `o.indexed` "large" checks: 86400 indices in one draw (chunked) and with rows from both ends in turn
  (first chunks drawn directly), and glDrawArrays of 86400 vertices, each the same pixels as one draw per row.

### Buffer objects: aligned storage, index streams as GX display lists
- Buffer data starts 32-byte aligned (the header is padded); it is flushed once at `glBufferData`/
  `glBufferSubData`/`glUnmapBuffer` (as before) and the indexed arrays point at it in place: no copy per draw.
- **Upstream bug:** `glBufferData` rounded the size up to 32 before copying, so it read up to 31 bytes past the
  caller's data.
- Index streams that repeat are compiled into GX display lists and called: the indices of an element buffer
  range (until the buffer's data changes; not for `GL_STREAM_DRAW` buffers) and the ascending indices of
  `glDrawArrays` (any buffer or client array: the list holds indices rebased to the range's first vertex, the
  arrays are pointed at that vertex). A stream is compiled the second time it is drawn (`GX_BeginDispList`,
  `GX_Begin`, the indices, `GX_EndDispList`), then each draw is `GX_SetArray` per attribute and
  `GX_CallDispList`. Up to three lists per range (passes with other attributes or primitives), 2 MB of lists in
  all, 1024 ranges per buffer; an element buffer range also keeps its smallest/largest index, so the index
  scan is done once. A rewrite, `glMapBuffer` or deletion drops the buffer's lists (freed once the GPU passed a
  draw sync token, as texture storage is).
- Only draws whose attributes are all indexed (no direct attribute in the vertex) and not chunked use lists.
- Bench (ms submit per frame, before -> after this step / against the base): static buffer objects, one
  glDrawElements 1.65 -> 0.06 (base 6.60); 100 glDrawElements 1.96 -> 0.44 (6.77); one glDrawArrays 1.32 -> 0.03
  (6.25); 100 glDrawArrays 1.57 -> 0.26 (6.41); client arrays, 100 glDrawArrays 3.20 -> 1.90 (11.95, the copies
  remain). FIFO bytes per 99-triangle draw from buffer objects: 3104 (base) -> 1632 -> 160.
- Check: `o.indexed`: element buffer drawn four times (streamed, compiled, called), then rewritten with other
  indices (the new ones are drawn), then restored; glDrawArrays lists through the per-row draws.

### Display lists: draws compiled as indexed data
- A `glDrawElements`/`glDrawArrays` (or `glBegin`/`glEnd`) inside `glNewList` is compiled into a copy of the
  vertex range it uses, packed per attribute (buffer object data copied too: a list keeps the data of when it
  was compiled), plus a GX display list of 8- or 16-bit indices. `glCallList` sets the vertex descriptors,
  formats and `GX_SetArray` for the list's copies and calls the GX list; state commands in the list are replayed
  by opengx as before (the TEV, matrix and texture code is unchanged). The current normal/colour stand-ins for
  disabled arrays keep working (one-element indexed arrays, as before).
- Draws with an attribute that cannot be indexed (4-component positions, conversions, generated texture
  coordinates) keep the direct-data list, now sized from the vertex count instead of a fixed 1 MB buffer.
- **Upstream bugs:** a list draw with a colour array and lighting off sent the colour twice per vertex while the
  vertex descriptor had one colour (every vertex after the first misread: garbage); a draw with more than 1 MB
  of direct data overflowed the list buffer and drew nothing (the bench's single-draw 9940-triangle list);
  compiling printed "Created draw list" to stderr; `glDeleteLists` freed list memory the GPU could still read.
- Bench: the 9940-triangle mesh as one list draw now draws (0.50 ms draw+finish; before: nothing drawn), as 100
  list draws 0.03 -> 0.15 ms submit (`GX_SetArray` per draw); compile 12.05 -> 5.89 ms (one draw), 45.24 -> 9.24
  ms (100 draws); list memory for the one-draw mesh about 0.5 MB (copies 0.2 MB + indices 0.3 MB) instead of
  1.2 MB of direct data.
- Check: `o.indexed` display lists (one draw, one per row, arrays changed after compiling, from a buffer object,
  with direct data, of `glBegin`/`glEnd`), unlit and lit: the same pixels as the arrays drawn directly (the
  unlit ones failed before).

### In the engines (Dolphin, emulated time; the same engine builds with only the fork changed)
- UC2 (UC2GLDrv fixed-function, static streams in buffer objects, dynamic ones as client arrays): DM-Decay
  pre-match camera 19.1 -> 54.0 fps, the device's frame time 55.9 -> 18.5 ms (194 draws, 127.8k primitives per
  frame in both); a 60 s bot match 21.3 -> 50.4 fps (frame time 44-59 -> 18-22 ms). The pre-match frame is the
  same picture (mean difference 0.7/255, the random bot names and portraits).
- UT99 (NOpenGLDrv, float-colour client arrays and glDrawElements per batch): at the 60 fps cap already; DM-Morbias
  60 s 3469 -> 3506 frames, CityIntro (volumetric lighting, CPU-bound in Render) 1001 -> 1015 frames in its 10-30 s
  window. Frames the same (the first 300 identical; later ones differ by animation timing).
- Memory: the ring is 1 MB (allocated at the first client-array draw), index display lists at most 2 MB (UC2
  DM-Decay: heap +0.3-1.4 MB at the same points).

## Lighting

### `GL_COLOR_MATERIAL` with `GL_EMISSION` and a colour array
- **Upstream bug:** with a colour array enabled, the lit path took every material colour from the vertices
  (`GX_SRC_VTX` on both channels) and the emission from the *current* colour (`glColor`), so a per-vertex
  emission (D3D's `EMISSIVEMATERIALSOURCE = COLOR1`, which UC2GLDrv uses for static lighting baked into vertex
  colours) drew `vertex colour * (ambient + lights) + glColor`: white walls.
- Now in that mode channel 0 passes the vertex colour through unlit as the emission, and channel 1 is
  `matdiffuse * (ambient' + lights)` from registers, `ambient' = matambient * globalambient / matdiffuse`
  (exact when the two material colours are equal, as D3D's default material is). Not drawn in this mode: the
  lights' own ambient and the specular term (GX has two colour channels).
- Check: `l.emission` (emission + global ambient + one directional light, and with the light off; within 2).

### Light numbers beyond the four opengx has
- **Upstream bug (memory):** `glLightf`/`glLightfv` indexed the light table with `light - GL_LIGHT0` unchecked;
  opengx has 4 lights (two GX lights each), so `GL_LIGHT4..7` (valid enums, which a D3D-style device with 8
  lights sets at start-up) wrote past the table into the GX light objects. They now give `GL_INVALID_ENUM`, and
  `glGetIntegerv(GL_MAX_LIGHTS)` answers 4 (it answered nothing).
- Check: `l.emission` (GL_MAX_LIGHTS, GL_LIGHT5/GL_LIGHT7 rejected).

## Texture coordinates

### Linear texture generation of r, through the texture matrix
- **Upstream gap:** `GL_EYE_LINEAR`/`GL_OBJECT_LINEAR` generated (s, t) with a 2x4 matrix (the R plane was not
  even stored), so the texture matrix's third column met a constant 1 instead of r. A D3D-style camera-space
  texture coordinate (`D3DTSS_TCI_CAMERASPACEPOSITION` + a texture transform mapping eye x, y, z to u, v: UE2's
  terrain layers and projectors) lost the eye depth: radial streaks on terrain.
- Now the generation is a 3x4 matrix of the S, T and R planes (R only with `GL_TEXTURE_GEN_R`, else 0), and the
  texture matrix is the post-transform with its third row (0, 0, 0, 1), so the projective divide is by 1.
  `glTexGen(GL_R, GL_EYE_PLANE/GL_OBJECT_PLANE)` is stored. Not done: the Q plane and a projective texture
  matrix (opengx keeps 3x4 texture matrices).
- Check: `m.texgen` (eye depth as s through the texture matrix, two depths, two texels).

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

### `GL_EXT_texture_compression_s3tc`: DXT1 → `GX_TF_CMPR`, DXT3/DXT5 decoded
- New: `glCompressedTexImage2D` / `glCompressedTexSubImage2D` (also reachable as `...ARB`) for
  `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` and `GL_COMPRESSED_RGBA_S3TC_DXT1_EXT`, any level (levels smaller than
  8x8 go into one padded 32-byte tile). `imageSize` must be exactly `ceil(w/4)*ceil(h/4)*8`
  (else `GL_INVALID_VALUE`). Sub-images must be 4-aligned.
- **DXT3 and DXT5** have no GX block format (CMPR is DXT1 only): `glCompressedTexImage2D` /
  `glCompressedTexSubImage2D` decode them on the CPU (standard S3TC: the colour half always in four-colour
  mode, DXT3's 4-bit explicit alpha, DXT5's two alpha endpoints with 3-bit indices in the 8- and 6-alpha modes)
  into RGBA bytes and store them through the `glTexImage2D` path:
  - default (`GL_TEXTURE_COMPRESSION_HINT` `GL_DONT_CARE`/`GL_FASTEST`): `GX_TF_RGB5A3`, 2 bytes per texel,
    **4x the DXT3/5 size** (256x256: 64 KB of DXT5 → 131 KB). Opaque texels keep RGB555, translucent ones
    become A3 RGB444 (alpha in eighths: DXT5 alpha gradients band).
  - `glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST)`: `GX_TF_RGBA8`, 4 bytes per texel, **8x the size**
    (256x256: 262 KB), lossless against the S3TC decode.
  The hint is read at upload time (it was a no-op before; `glHint` now rejects unknown modes with
  `GL_INVALID_ENUM`). `imageSize` must be `ceil(w/4)*ceil(h/4)*16`; sub-images must be 4-aligned and go into the
  texture's decoded storage. The application's unpack state (alignment, row length, skips) is ignored for the
  decoded rows, as it is for compressed data. Cost (Dolphin, emulated): 256x256 DXT5 → RGB5A3 7.5 ms, → RGBA8
  6.0 ms per upload (the decode plus the RGBA tiling).
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

### Internal formats: `GL_RGB5_A1`, `GL_RGBA4`, luminance/alpha/intensity
- `GL_RGB5_A1`, `GL_RGBA4`, `GL_RGBA2` → `GX_TF_RGB5A3` (new `TexelRGB5A3` converter, plus a registered fast
  path from `GL_RGBA` bytes). Upstream sent them to the default branch, i.e. `GX_TF_CMPR`, then ran the RGB DXT
  compressor on RGBA data: corrupt texture. 1-bit alpha is exact; 4-bit alpha keeps 3 bits; texels whose alpha
  rounds to 7/7 get 5-bit colour, the others 4-bit. (So `GL_RGB5_A1` with a source alpha of e.g. 200 is stored
  translucent 5/7 instead of opaque.)
- Sized luminance / luminance-alpha / intensity / alpha formats (`GL_LUMINANCE8`, `GL_LUMINANCE8_ALPHA8`,
  `GL_INTENSITY8`, `GL_ALPHA8`, … and the component counts 1 and 2) map to I8 / IA8 / I8 / A8 instead of CMPR.
- `GL_UNSIGNED_SHORT_5_5_5_1` (and every packed type with components narrower than 4 bits) expanded the
  components with a single bit replication: 1-bit alpha 1 became 0xc0. Now replicated to the full byte.
- Base-format semantics (GL 1.5, table 3.22): GX I8 returns the intensity as alpha too, and every GX format has
  some colour. Textures whose GL base format has no alpha (`GL_RGB*`, `GL_LUMINANCE*`, RGB DXT1, native RGB565)
  are flagged `is_opaque`; `GL_ALPHA*` ones `is_alpha`. In `GL_REPLACE/MODULATE/ADD/BLEND` the missing
  channels now pass the fragment value through (upstream: `GL_LUMINANCE` modulated alpha by L, `GL_ALPHA`
  replaced/modulated the colour with A, `GL_RGB` + `GL_REPLACE` replaced the fragment alpha with 1); in
  `GL_COMBINE` such a texture's alpha source reads 1.0 and an alpha texture's colour source reads 0.

### Upload fast paths
- Direct tilers in `_ogx_bytes_to_texture()` (before the registered/generic converters) for:
  `GL_RGBA`/`GL_UNSIGNED_BYTE` → RGBA8 (two texels per pair of 32-bit stores), `GL_RGB`/
  `GL_UNSIGNED_SHORT_5_6_5` → RGB565 (a tiled copy, two 32-bit words per tile line; upstream had no fast path
  at all for packed types), `GL_RGB` bytes → RGB565, `GL_RGBA` bytes → RGB5A3 (row encode + tiled copy),
  `GL_LUMINANCE`/`GL_INTENSITY` bytes → I8 (byte tiling). They honour `GL_UNPACK_ALIGNMENT`,
  `GL_UNPACK_ROW_LENGTH`, `GL_UNPACK_SKIP_PIXELS/ROWS` and arbitrary sub-image offsets (unlike the upstream
  readers, which ignore the alignment: with them, rows of e.g. 7 16-bit texels were read tightly packed).
- After a (sub-)image update only the touched rows of tiles are flushed from the data cache.
- 256x256 emulated: RGBA8 3.15 → 1.00 ms, 5_6_5 → RGB565 14.73 → 0.20 ms, RGB → RGB565 3.11 → 1.21 ms,
  RGBA → RGB5_A1 19.9 (corrupt) → 2.47 ms, L → I8 2.28 → 0.10 ms; 64x64 sub-image into 512x512 RGBA8 0.10 ms.

### Mip levels of non-square textures
- A level's size is `max(1, base >> level)` per axis. `glTexImage2D` derived the base size as `size << level`,
  which is wrong once an axis has reached 1: the 1x2 tail of a 128x256 chain gave "128x512", so opengx threw the
  texture away and rebuilt it around that tiny level (flat-coloured surfaces in UT). Now a level whose size
  matches the existing texture's level keeps the texture; `size << level` is only used for a new base size.

### Texture uploads without a GPU sync (buffer orphaning)
- Upstream `glTexImage2D` (and the compressed/native uploads) started with `GX_DrawDone()`: the CPU waited until
  the GPU had drawn everything queued, on every upload. `glTexSubImage2D` did not wait at all and wrote into
  texels that queued draws were still going to read (in Dolphin 19 of 20 draws interleaved with sub-image
  updates showed the last upload).
- Now a texture is marked `in_use` when it is bound to a TEV stage. An upload into an `in_use` texture first
  gives it a fresh texel buffer (a copy of the old one unless the upload replaces all of a single-level
  texture), sends a draw-sync token, and frees the old buffer once the GPU has passed that token
  (`_ogx_free_after_gpu`). A texture not drawn since its last upload is written in place. No upload waits for
  the GPU any more (only if the 128-entry deferred-free list fills up, or a copy can't be allocated).
- opengx's draw-sync tokens restart from 0 at every `ogx_prepare_swap_buffers()` (which sends token 0); FIFO
  points are therefore tracked as (swap markers sent before, token), the sync callback counting the zeros.
- `glDeleteTextures` uses the same deferred free instead of a `GX_DrawDone()`, for texels and palettes.

## Framebuffer

### `glClear` colour writes
- `glClear` drew its clearing quad with `GX_SetColorUpdate(GX_TRUE)` in both branches, so
  `glClear(GL_DEPTH_BUFFER_BIT)` also cleared the colour buffer to the clear colour (UT's sky zone vanished).
  Colour writes are now enabled only for `GL_COLOR_BUFFER_BIT`, and only if `glColorMask` allows them; a clear
  with neither colour nor depth to do draws nothing.

### `glDepthRange`
- The range was stored (and used by the raster/2D paths) but the viewport always had Z range 0..1, so 3D
  drawing ignored it. `update_viewport()` now passes it to `GX_SetViewport()` as near/far, and `glDepthRange`
  clamps to [0,1] and marks the viewport dirty. The 2D projection already used the range as its near/far
  planes, which with the new viewport keeps raster positions at their window Z; an empty range (e.g. 1,1)
  gets a dummy far plane there to avoid a division by zero.

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

## Small GL 1.2-1.4 entry points
- `glGetIntegerv` framebuffer sizes from the EFB pixel format: `GL_DEPTH_BITS` 24 (16 with
  `GX_PF_RGB565_Z16`), `GL_RED/GREEN/BLUE_BITS` 8 (6 for RGBA6, 5/6/5 for RGB565), `GL_ALPHA_BITS` 0 (6 for
  RGBA6). `GL_STENCIL_BITS` is 0 unless the emulated stencil was created with `ogx_stencil_create()` (then 4 or
  8; upstream always answered 4).
- `glDrawRangeElements` (the range is a hint; forwards to `glDrawElements`).
- `glBlendFuncSeparate`: `GX_SetBlendMode()` has one factor pair for colour and alpha. With the default
  `GX_PF_RGB8_Z24` EFB there is no destination alpha, so the RGB pair is exact; with an alpha EFB the alpha
  factors are ignored (warning).
- `glGetTexLevelParameter{i,f}v`: `GL_TEXTURE_WIDTH/HEIGHT/DEPTH/BORDER/INTERNAL_FORMAT`, component sizes,
  `GL_TEXTURE_COMPRESSED(_IMAGE_SIZE)` for `GL_TEXTURE_2D` (0 for levels that don't exist), and
  `GL_PROXY_TEXTURE_2D` (the last proxy `glTexImage2D`, accepted up to 1024x1024). The internal format is
  derived from the GX format (`GL_RGBA8`, `GL_RGB`, `GL_RGB5_A1`, `GL_LUMINANCE(_ALPHA)`, `GL_INTENSITY`,
  `GL_ALPHA`, `GL_COLOR_INDEX8_EXT`, DXT1 RGB/RGBA).

/*****************************************************************************
Copyright (c) 2026  The UT99 Wii port contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of copyright holders nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/



/* glCompressedTexImage2D(): S3TC DXT1 data is repacked into the GX CMPR
 * format, which uses the same 8-byte blocks with a different byte and bit
 * order and a different arrangement in memory:
 *
 * - DXT1 blocks are stored in raster order, one per 4x4 texels. CMPR groups
 *   them by 2x2 into 32-byte tiles covering 8x8 texels (top-left, top-right,
 *   bottom-left, bottom-right block), and the tiles are in raster order.
 * - The two RGB565 colours are little-endian 16-bit words in DXT1 and
 *   big-endian in CMPR.
 * - The four index bytes (one per texel row) hold four 2-bit indices; DXT1
 *   puts the leftmost texel in the lowest two bits, CMPR in the highest two.
 *   The meaning of the indices (and the "c0 <= c1 means three colours plus
 *   transparent" rule) is the same.
 * DXT3 and DXT5 have no GX equivalent: they are decoded on the CPU into
 * RGBA bytes and stored through glTexImage2D() as GX_TF_RGB5A3 (2 bytes per
 * texel, 4x the DXT5 size: opaque texels RGB555, translucent ones A3 RGB444),
 * or as GX_TF_RGBA8 (4 bytes per texel) when GL_TEXTURE_COMPRESSION_HINT is
 * GL_NICEST.
 *
 * GL_OGX_native_texture (GL/gxext.h): the GL_TEXTURE_GX_*_OGX internal
 * formats take data which is already in the GX layout; it's copied as is.
 * The low 4 bits of those enums are the GX_TF_* format. */

#define GL_GLEXT_PROTOTYPES 1
#include "debug.h"
#include "pixels.h"
#include "state.h"
#include "texture.h"
#include "utils.h"

#include <GL/gxext.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* Convert one DXT1 block (as two 32-bit words loaded from little-endian
 * data on our big-endian CPU) into a CMPR block, in place. */
static inline void dxt1_block_to_cmpr(uint32_t *d, const uint8_t *s)
{
    uint32_t colors, indices;
    memcpy(&colors, s, 4);
    memcpy(&indices, s + 4, 4);
    /* Byte-swap each 16-bit colour */
    colors = ((colors & 0x00ff00ff) << 8) | ((colors >> 8) & 0x00ff00ff);
    /* Reverse the order of the four 2-bit fields in every byte */
    indices = ((indices & 0x33333333) << 2) | ((indices >> 2) & 0x33333333);
    indices = ((indices & 0x0f0f0f0f) << 4) | ((indices >> 4) & 0x0f0f0f0f);
    d[0] = colors;
    d[1] = indices;
}

/* Copy a rectangle of DXT1 blocks into a CMPR level. "bx0, by0" is the
 * destination position in blocks, "bw x bh" the size of the source in
 * blocks, "level_bw" the width of the destination level in blocks. */
static void dxt1_to_cmpr(const uint8_t *src, int bw, int bh,
                         uint8_t *dst, int bx0, int by0, int level_bw)
{
    int tiles_per_row = (level_bw + 1) / 2;
    for (int by = 0; by < bh; by++) {
        int ty = by + by0;
        uint8_t *tile_row = dst + (ty >> 1) * tiles_per_row * 32 + (ty & 1) * 16;
        for (int bx = 0; bx < bw; bx++) {
            int tx = bx + bx0;
            uint32_t *d = (uint32_t *)(tile_row + (tx >> 1) * 32 + (tx & 1) * 8);
            dxt1_block_to_cmpr(d, src);
            src += 8;
        }
    }
}

static int dxt1_size(int width, int height)
{
    return ((width + 3) / 4) * ((height + 3) / 4) * 8;
}

/* GL_TEXTURE_COMPRESSION_HINT (glHint): GL_NICEST stores decoded DXT3/DXT5
 * images as RGBA8, anything else as RGB5A3. */
GLenum _ogx_texture_compression_hint = GL_DONT_CARE;

static bool is_dxt35(GLenum internal_format)
{
    return internal_format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
        internal_format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

static int dxt35_size(int width, int height)
{
    return ((width + 3) / 4) * ((height + 3) / 4) * 16;
}

static void rgb565_to_rgb8(uint16_t c, int *rgb)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

/* The 16 alpha values of a DXT3 or DXT5 block (texel t = row * 4 + column)
 * in 8 bits: DXT3 has 4-bit explicit alpha (low nibble first), DXT5 two
 * endpoints and 3-bit indices (the 8- or the 6-alpha mode). */
static void dxt35_block_alpha(const uint8_t *src, bool dxt5, uint8_t alpha[16])
{
    if (dxt5) {
        int a[8];
        a[0] = src[0];
        a[1] = src[1];
        if (a[0] > a[1]) {
            for (int i = 1; i < 7; i++)
                a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
        } else {
            for (int i = 1; i < 5; i++)
                a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++)
            bits |= (uint64_t)src[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++)
            alpha[i] = a[(bits >> (3 * i)) & 7];
    } else {
        for (int i = 0; i < 16; i++) {
            int nibble = (src[i / 2] >> ((i & 1) * 4)) & 15;
            alpha[i] = nibble * 17;
        }
    }
}

/* Decode a DXT3 or DXT5 image (blocks in raster order, little-endian fields)
 * into width x height RGBA bytes. The colour half of these blocks is always
 * the four-colour mode, whatever the order of the endpoints. */
static void decode_dxt35(const uint8_t *src, bool dxt5, int width, int height,
                         uint8_t *rgba)
{
    int bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++, src += 16) {
            uint8_t alpha[16];
            dxt35_block_alpha(src, dxt5, alpha);
            const uint8_t *cb = src + 8;
            int colors[4][3];
            rgb565_to_rgb8(cb[0] | (cb[1] << 8), colors[0]);
            rgb565_to_rgb8(cb[2] | (cb[3] << 8), colors[1]);
            for (int k = 0; k < 3; k++) {
                colors[2][k] = (2 * colors[0][k] + colors[1][k] + 1) / 3;
                colors[3][k] = (colors[0][k] + 2 * colors[1][k] + 1) / 3;
            }
            uint32_t indices = cb[4] | (cb[5] << 8) | (cb[6] << 16) |
                ((uint32_t)cb[7] << 24);
            for (int ty = 0; ty < 4; ty++) {
                int y = by * 4 + ty;
                if (y >= height) break;
                for (int tx = 0; tx < 4; tx++) {
                    int x = bx * 4 + tx;
                    if (x >= width) continue;
                    int t = ty * 4 + tx;
                    const int *c = colors[(indices >> (2 * t)) & 3];
                    uint8_t *out = rgba + (y * width + x) * 4;
                    out[0] = c[0];
                    out[1] = c[1];
                    out[2] = c[2];
                    out[3] = alpha[t];
                }
            }
        }
    }
}

/* A DXT3/DXT5 image or sub-image as RGBA bytes (NULL when out of memory). */
static uint8_t *decode_dxt35_image(GLenum format, int width, int height,
                                   const void *data)
{
    uint8_t *rgba = malloc(width * height * 4);
    if (!rgba) {
        set_error(GL_OUT_OF_MEMORY);
        return NULL;
    }
    decode_dxt35(data, format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width,
                 height, rgba);
    return rgba;
}

/* The pixel-store state a decoded image needs (tightly packed rows), and the
 * application's, put back afterwards. */
typedef struct {
    uint8_t skip_pixels, skip_rows, alignment;
    uint16_t row_length;
} UnpackState;

static UnpackState tight_unpack(void)
{
    UnpackState saved = {
        glparamstate.unpack_skip_pixels, glparamstate.unpack_skip_rows,
        glparamstate.unpack_alignment, glparamstate.unpack_row_length,
    };
    glparamstate.unpack_skip_pixels = 0;
    glparamstate.unpack_skip_rows = 0;
    glparamstate.unpack_alignment = 4;
    glparamstate.unpack_row_length = 0;
    return saved;
}

static void restore_unpack(const UnpackState *saved)
{
    glparamstate.unpack_skip_pixels = saved->skip_pixels;
    glparamstate.unpack_skip_rows = saved->skip_rows;
    glparamstate.unpack_alignment = saved->alignment;
    glparamstate.unpack_row_length = saved->row_length;
}

/* ---------------------------------------------------------------------
 * DXT3/DXT5 kept compressed (GL_TEXTURE_COMPRESSION_HINT GL_FASTEST): the
 * colour halves become a CMPR texture, the alpha a GX_TF_I4 plane (or
 * GX_TF_I8, OPENGX_DXT5_ALPHA below) of the same size and mipmap layout,
 * which texture_unit.c samples in a TEV stage of its own. 1 byte per texel
 * (the DXT3/DXT5 size) instead of 2 (RGB5A3) or 4 (RGBA8); DXT3's 4-bit
 * alpha is exact in I4, DXT5's interpolated alpha is rounded to 4 bits
 * (RGB5A3 keeps 3 bits of a translucent texel, and 4 of its colour).
 *
 * CMPR decodes a block with colour0 <= colour1 as three colours plus a
 * transparent texel, DXT3/DXT5 colour halves are always four colours: such
 * blocks get their endpoints swapped and their indices 0<->1, 2<->3 (the
 * thirds then fall on the same colours), equal endpoints become one colour
 * (every index 0). No texel turns transparent.
 * --------------------------------------------------------------------- */

uint32_t _ogx_texture_demotions = 0;

static inline void dxt35_color_to_cmpr(uint32_t *d, const uint8_t *s)
{
    uint32_t c0 = s[0] | (s[1] << 8), c1 = s[2] | (s[3] << 8);
    uint32_t indices;
    memcpy(&indices, s + 4, 4); /* row 0 in the first byte, as CMPR */
    if (c0 < c1) {
        uint32_t t = c0;
        c0 = c1;
        c1 = t;
        indices ^= 0x55555555;
    } else if (c0 == c1) {
        indices = 0;
    }
    /* Reverse the order of the four 2-bit fields in every byte */
    indices = ((indices & 0x33333333) << 2) | ((indices >> 2) & 0x33333333);
    indices = ((indices & 0x0f0f0f0f) << 4) | ((indices >> 4) & 0x0f0f0f0f);
    d[0] = (c0 << 16) | c1;
    d[1] = indices;
}

/* 8-bit alpha rounded to 4 bits */
static uint8_t s_alpha_to_4[256];
static inline uint8_t alpha_to_4(int a) { return s_alpha_to_4[a]; }
static void alpha_to_4_init(void)
{
    if (s_alpha_to_4[255]) return;
    for (int a = 0; a < 256; a++) s_alpha_to_4[a] = (a * 15 + 127) / 255;
}

/* Byte offset of the 4x4 block (bx, by) in a GX_TF_I4 (8x8 tiles, rows of 4
 * bytes, the left texel in the high nibble) or GX_TF_I8 (8x4 tiles, rows of
 * 8 bytes) level whose width is level_bw blocks: both have 32-byte tiles 8
 * texels wide. */
static inline uint32_t alpha_block_offset(bool i8, int bx, int by, int level_bw)
{
    int tiles_per_row = (level_bw + 1) / 2;
    if (i8)
        return (by * tiles_per_row + (bx >> 1)) * 32 + (bx & 1) * 4;
    return ((by >> 1) * tiles_per_row + (bx >> 1)) * 32 + (by & 1) * 16 +
        (bx & 1) * 2;
}

static void put_alpha_block(uint8_t *plane, bool i8, const uint8_t a[16])
{
    for (int r = 0; r < 4; r++) {
        const uint8_t *row = a + r * 4;
        if (i8) {
            memcpy(plane + r * 8, row, 4);
        } else {
            plane[r * 4] = (alpha_to_4(row[0]) << 4) | alpha_to_4(row[1]);
            plane[r * 4 + 1] = (alpha_to_4(row[2]) << 4) | alpha_to_4(row[3]);
        }
    }
}

/* Alpha precision of the DXT5 uploads (OPENGX_DEBUG=texture): the error of
 * the 4-bit plane and of RGB5A3 (the decoded storage: 3 bits for a
 * translucent texel) against the 8-bit DXT5 alpha. */
typedef struct {
    uint32_t texels, translucent, i4_sum, a3_sum;
    uint8_t i4_max, a3_max;
    uint32_t blocks, gradient_blocks;
} AlphaStats;

static void alpha_stats_block(AlphaStats *st, const uint8_t a[16])
{
    int mn = 255, mx = 0, distinct = 0;
    uint8_t seen[256 / 8] = { 0 };
    for (int i = 0; i < 16; i++) {
        int v = a[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (!(seen[v >> 3] & (1 << (v & 7)))) {
            seen[v >> 3] |= 1 << (v & 7);
            distinct++;
        }
        int e4 = abs(v - alpha_to_4(v) * 17);
        int a3 = (v * 7 + 127) / 255;
        int e3 = abs(v - (a3 == 7 ? 255 : (a3 << 5) | (a3 << 2) | (a3 >> 1)));
        st->texels++;
        if (v != 0 && v != 255) st->translucent++;
        st->i4_sum += e4;
        st->a3_sum += e3;
        if (e4 > st->i4_max) st->i4_max = e4;
        if (e3 > st->a3_max) st->a3_max = e3;
    }
    st->blocks++;
    /* A smooth ramp: several levels within a few 4-bit steps, where
     * rounding to 4 bits shows as bands */
    if (distinct >= 4 && mx - mn <= 51) st->gradient_blocks++;
}

/* OPENGX_DXT5_ALPHA: "i4" (the default) or "i8" (DXT5 alpha planes in 8
 * bits: 1.5 bytes per texel, exact) */
static bool dxt5_alpha_i8(void)
{
    /* Read for every new texture (a few environment entries on the Wii) */
    const char *env = getenv("OPENGX_DXT5_ALPHA");
    return env && strcmp(env, "i8") == 0;
}

/* Convert a rectangle of bw x bh DXT3/DXT5 blocks into the two planes of a
 * level whose width is level_bw blocks, at block (bx0, by0). */
static void dxt35_to_planes(const uint8_t *src, bool dxt5, bool i8,
                            int bw, int bh, uint8_t *color, uint8_t *alpha,
                            int bx0, int by0, int level_bw, AlphaStats *st)
{
    int tiles_per_row = (level_bw + 1) / 2;
    alpha_to_4_init();
    for (int by = 0; by < bh; by++) {
        int ty = by + by0;
        uint8_t *tile_row = color + (ty >> 1) * tiles_per_row * 32 +
            (ty & 1) * 16;
        for (int bx = 0; bx < bw; bx++, src += 16) {
            int tx = bx + bx0;
            uint8_t a[16];
            dxt35_block_alpha(src, dxt5, a);
            put_alpha_block(alpha + alpha_block_offset(i8, tx, ty, level_bw),
                            i8, a);
            if (st) alpha_stats_block(st, a);
            dxt35_color_to_cmpr((uint32_t *)(tile_row + (tx >> 1) * 32 +
                                             (tx & 1) * 8), src + 8);
        }
    }
}

static void log_alpha_stats(const AlphaStats *st, int width, int height,
                            bool i8)
{
    if (!st->texels) return;
    /* Hundredths, to keep floats out of the report */
    debug(OGX_LOG_TEXTURE, "DXT5 %dx%d alpha as %s: translucent %u of %u, "
          "I4 err mean %u.%02u max %u, RGB5A3 err mean %u.%02u max %u, "
          "gradient blocks %u of %u", width, height, i8 ? "I8" : "I4",
          st->translucent, st->texels,
          st->i4_sum / st->texels, st->i4_sum * 100 / st->texels % 100,
          st->i4_max, st->a3_sum / st->texels,
          st->a3_sum * 100 / st->texels % 100, st->a3_max,
          st->gradient_blocks, st->blocks);
}

/* glCompressedTexImage2D() of a DXT3/DXT5 level into the split storage */
static void split_tex_image(GLuint tex_id, GLint level, bool dxt5,
                            int width, int height, const void *data)
{
    gltexture_ *currtex = &texture_list[tex_id];
    GXTexObj *texobj = &currtex->texobj;
    {
        float minlod, maxlod;
        GX_GetTexObjLOD(texobj, &minlod, &maxlod);
        _ogx_texture_make_writable(currtex,
                                   !(level == 0 && minlod == 0 && maxlod == 0));
    }

    /* The alpha plane the texture has, if the colour keeps its geometry
     * (the other levels of a chain uploaded level by level) */
    OgxTextureInfo old;
    _ogx_texture_get_info(tex_id, &old);
    bool had_alpha = (currtex->alpha_plane & OGX_ALPHA_PLANE) &&
        old.texels && old.format == GX_TF_CMPR;
    bool old_chain = !(old.minlevel == 0 && old.maxlevel == 0);
    /* The alpha format is chosen once per texture */
    bool i8 = had_alpha ? (currtex->alpha_plane & OGX_ALPHA_PLANE_I8) != 0 :
        dxt5 && dxt5_alpha_i8();
    uint8_t alpha_format = i8 ? GX_TF_I8 : GX_TF_I4;

    OgxTextureInfo ti;
    if (!_ogx_texture_prepare_storage(texobj, level, width, height,
                                      GX_TF_CMPR, &ti)) {
        _ogx_texture_drop_alpha(currtex);
        return;
    }
    bool chain = !(ti.minlevel == 0 && ti.maxlevel == 0);
    uint32_t alpha_size = _ogx_texture_buffer_size(ti.width, ti.height,
                                                   alpha_format, chain);
    bool same_geometry = had_alpha && old.width == ti.width &&
        old.height == ti.height;
    if (!(same_geometry && old_chain == chain)) {
        uint8_t *alpha = memalign(32, alpha_size);
        if (!alpha) {
            warning("Failed to allocate %u bytes for an alpha plane",
                    alpha_size);
            set_error(GL_OUT_OF_MEMORY);
            free(ti.texels);
            GX_InitTexObj(texobj, NULL, 0, 0, 0, GX_REPEAT, GX_REPEAT, 0);
            GX_InitTexObjUserData(texobj, ti.ud.ptr);
            _ogx_texture_drop_alpha(currtex);
            return;
        }
        if (same_geometry) {
            /* level 0 alone -> a whole chain: keep level 0 */
            uint32_t size0 = _ogx_texture_level_size(ti.width, ti.height,
                                                     alpha_format);
            memcpy(alpha, currtex->alpha_texels, size0);
            DCFlushRange(alpha, size0);
        }
        _ogx_texture_drop_alpha(currtex);
        currtex->alpha_texels = alpha;
    }
    currtex->alpha_plane = OGX_ALPHA_PLANE |
        (i8 ? OGX_ALPHA_PLANE_I8 : 0) | (dxt5 ? OGX_ALPHA_PLANE_DXT5 : 0);
    ti.ud.d.is_opaque = 0;

    if (data) {
        uint8_t *dst = (uint8_t *)ti.texels +
            _ogx_texture_level_offset(level, ti.width, ti.height, GX_TF_CMPR);
        uint8_t *adst = (uint8_t *)currtex->alpha_texels +
            _ogx_texture_level_offset(level, ti.width, ti.height,
                                      alpha_format);
        uint32_t size = _ogx_texture_level_size(width, height, GX_TF_CMPR);
        uint32_t asize = _ogx_texture_level_size(width, height, alpha_format);
        int bw = (width + 3) / 4, bh = (height + 3) / 4;
        /* Blocks outside the image (when the size isn't a multiple of 8) are
         * not sampled, but keep them deterministic */
        if ((bw | bh) & 1) {
            memset(dst, 0, size);
            memset(adst, 0, asize);
        }
        AlphaStats st = { 0 };
        bool stats = dxt5 && level == 0 && (_ogx_log_mask & OGX_LOG_TEXTURE);
        dxt35_to_planes(data, dxt5, i8, bw, bh, dst, adst, 0, 0, bw,
                        stats ? &st : NULL);
        if (stats) log_alpha_stats(&st, width, height, i8);
        DCFlushRange(dst, size);
        DCFlushRange(adst, asize);
        GX_InvalidateTexAll();
    }
    _ogx_texture_commit(texobj, &ti);
    glparamstate.dirty.bits.dirty_tev = 1;
}

/* glCompressedTexSubImage2D() of DXT3/DXT5 blocks into the split storage */
static void split_tex_sub_image(GLuint tex_id, GLint level,
                                GLint xoffset, GLint yoffset,
                                GLsizei width, GLsizei height, bool dxt5,
                                const void *data)
{
    gltexture_ *currtex = &texture_list[tex_id];
    OgxTextureInfo ti;
    _ogx_texture_get_info(tex_id, &ti);
    if (!ti.texels || level < ti.minlevel || level > ti.maxlevel) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    int level_w = ti.width >> level, level_h = ti.height >> level;
    if (level_w < 1) level_w = 1;
    if (level_h < 1) level_h = 1;
    if (xoffset < 0 || yoffset < 0 ||
        xoffset + width > level_w || yoffset + height > level_h ||
        ((width & 3) && xoffset + width != level_w) ||
        ((height & 3) && yoffset + height != level_h)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    _ogx_texture_make_writable(currtex, true);
    _ogx_texture_get_info(tex_id, &ti);
    bool i8 = (currtex->alpha_plane & OGX_ALPHA_PLANE_I8) != 0;
    uint8_t alpha_format = i8 ? GX_TF_I8 : GX_TF_I4;
    uint8_t *dst = (uint8_t *)ti.texels +
        _ogx_texture_level_offset(level, ti.width, ti.height, GX_TF_CMPR);
    uint8_t *adst = (uint8_t *)currtex->alpha_texels +
        _ogx_texture_level_offset(level, ti.width, ti.height, alpha_format);
    dxt35_to_planes(data, dxt5, i8, (width + 3) / 4, (height + 3) / 4,
                    dst, adst, xoffset / 4, yoffset / 4, (level_w + 3) / 4,
                    NULL);
    DCFlushRange(dst, _ogx_texture_level_size(level_w, level_h, GX_TF_CMPR));
    DCFlushRange(adst, _ogx_texture_level_size(level_w, level_h,
                                               alpha_format));
    GX_InvalidateTexAll();
    glparamstate.dirty.bits.dirty_tev = 1;
}

/* One texel of the split storage as RGBA, the colour decoded as the S3TC
 * four-colour block it came from (thirds), as the decoded storage has it */
static void split_texel(const uint8_t *color, const uint8_t *alpha, bool i8,
                        int level_w, int x, int y, uint8_t *out)
{
    int level_bw = (level_w + 3) / 4, tiles_per_row = (level_bw + 1) / 2;
    int bx = x / 4, by = y / 4;
    const uint8_t *b = color + (by >> 1) * tiles_per_row * 32 +
        (by & 1) * 16 + (bx >> 1) * 32 + (bx & 1) * 8;
    int c[2][3];
    rgb565_to_rgb8((b[0] << 8) | b[1], c[0]);
    rgb565_to_rgb8((b[2] << 8) | b[3], c[1]);
    int index = (b[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3;
    for (int k = 0; k < 3; k++) {
        int v;
        switch (index) {
        case 0: v = c[0][k]; break;
        case 1: v = c[1][k]; break;
        case 2: v = (2 * c[0][k] + c[1][k] + 1) / 3; break;
        default: v = (c[0][k] + 2 * c[1][k] + 1) / 3; break;
        }
        out[k] = v;
    }
    const uint8_t *a = alpha + alpha_block_offset(i8, bx, by, level_bw);
    if (i8) {
        out[3] = a[(y & 3) * 8 + (x & 3)];
    } else {
        uint8_t v = a[(y & 3) * 4 + (x & 3) / 2];
        out[3] = ((x & 1) ? v & 15 : v >> 4) * 17;
    }
}

bool _ogx_texture_split_demote(GLuint tex_id)
{
    gltexture_ *texture = &texture_list[tex_id];
    if (!(texture->alpha_plane & OGX_ALPHA_PLANE)) return true;

    OgxTextureInfo ti;
    _ogx_texture_get_info(tex_id, &ti);
    bool chain = !(ti.minlevel == 0 && ti.maxlevel == 0);
    bool i8 = (texture->alpha_plane & OGX_ALPHA_PLANE_I8) != 0;
    uint8_t alpha_format = i8 ? GX_TF_I8 : GX_TF_I4;
    uint32_t size = _ogx_texture_buffer_size(ti.width, ti.height,
                                             GX_TF_RGB5A3, chain);
    int w0 = ti.width >> ti.minlevel ? ti.width >> ti.minlevel : 1;
    int h0 = ti.height >> ti.minlevel ? ti.height >> ti.minlevel : 1;
    uint8_t *texels = memalign(32, size);
    uint8_t *rgba = malloc(w0 * h0 * 4);
    if (!texels || !rgba) {
        free(texels);
        free(rgba);
        return false;
    }
    UnpackState saved = tight_unpack();
    for (int level = ti.minlevel; level <= ti.maxlevel; level++) {
        int lw = ti.width >> level ? ti.width >> level : 1;
        int lh = ti.height >> level ? ti.height >> level : 1;
        const uint8_t *color = (const uint8_t *)ti.texels +
            _ogx_texture_level_offset(level, ti.width, ti.height, GX_TF_CMPR);
        const uint8_t *alpha = (const uint8_t *)texture->alpha_texels +
            _ogx_texture_level_offset(level, ti.width, ti.height,
                                      alpha_format);
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++)
                split_texel(color, alpha, i8, lw, x, y,
                            rgba + (y * lw + x) * 4);
        uint8_t *dst = texels +
            _ogx_texture_level_offset(level, ti.width, ti.height,
                                      GX_TF_RGB5A3);
        _ogx_bytes_to_texture(rgba, GL_RGBA, GL_UNSIGNED_BYTE, lw, lh, dst,
                              GX_TF_RGB5A3, 0, 0,
                              _ogx_pitch_for_width(GX_TF_RGB5A3, lw));
    }
    restore_unpack(&saved);
    free(rgba);
    DCFlushRange(texels, size);

    if (texture->in_use) _ogx_free_after_gpu(ti.texels);
    else free(ti.texels);
    _ogx_texture_drop_alpha(texture);
    ti.texels = texels;
    ti.format = GX_TF_RGB5A3;
    ti.ud.d.is_opaque = 0;
    _ogx_texture_commit(&texture->texobj, &ti);
    GX_InvalidateTexAll();
    glparamstate.dirty.bits.dirty_tev = 1;
    _ogx_texture_demotions++;
    return true;
}

static bool is_dxt1(GLenum internal_format)
{
    return internal_format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
        internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
}

static int native_gx_format(GLenum internal_format)
{
    switch (internal_format) {
    case GL_TEXTURE_GX_I8_OGX:
    case GL_TEXTURE_GX_IA8_OGX:
    case GL_TEXTURE_GX_RGB565_OGX:
    case GL_TEXTURE_GX_RGB5A3_OGX:
    case GL_TEXTURE_GX_RGBA8_OGX:
    case GL_TEXTURE_GX_CI8_OGX:
    case GL_TEXTURE_GX_CMPR_OGX:
        return internal_format & 0xf;
    default:
        return -1;
    }
}

/* Upload pre-tiled GX data. "image_size" is either the size of the given
 * level, or (for level 0 only) the size of a whole chain of 2 or more mipmap
 * levels laid out as GX expects them (each level right after the previous
 * one), which is uploaded in one go. */
static void native_tex_image(gltexture_ *currtex, GLint level, uint8_t gx_format,
                             int width, int height, GLsizei image_size,
                             const void *data)
{
    GXTexObj *texobj = &currtex->texobj;
    int num_levels = 0;
    if (image_size == (GLsizei)_ogx_texture_level_size(width, height,
                                                      gx_format)) {
        num_levels = 1;
    } else if (level == 0 && width >= 2 && height >= 2) {
        for (int n = 2; n <= 11; n++) {
            if ((width >> (n - 1)) == 0 && (height >> (n - 1)) == 0) break;
            if (image_size == (GLsizei)_ogx_texture_level_offset(n, width, height,
                                                                 gx_format)) {
                num_levels = n;
                break;
            }
        }
    }
    if (num_levels == 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    /* No GX_DrawDone(): a texture queued draws may still read gets a new
     * buffer. Only a single-level replacement can drop the old contents. */
    {
        float minlod, maxlod;
        GX_GetTexObjLOD(texobj, &minlod, &maxlod);
        _ogx_texture_make_writable(currtex, !(level == 0 &&
                                              (num_levels > 1 ||
                                               (minlod == 0 && maxlod == 0))));
    }

    OgxTextureInfo ti;
    bool ok;
    if (num_levels > 1) {
        /* Preparing the storage for level 1 allocates a full chain */
        ok = _ogx_texture_prepare_storage(texobj, 1, width / 2, height / 2,
                                          gx_format, &ti);
        ti.minlevel = 0;
        ti.maxlevel = num_levels - 1;
    } else {
        ok = _ogx_texture_prepare_storage(texobj, level, width, height,
                                          gx_format, &ti);
    }
    if (!ok) return;
    /* RGB565 has the GL_RGB base format (no alpha); the others have alpha
     * (I8 is an intensity format: alpha = I) */
    ti.ud.d.is_opaque = gx_format == GX_TF_RGB565;

    uint8_t *dst = (uint8_t *)ti.texels +
        _ogx_texture_level_offset(level, ti.width, ti.height, ti.format);
    if (data) {
        memcpy(dst, data, image_size);
        DCFlushRange(dst, image_size);
        GX_InvalidateTexAll();
    }
    _ogx_texture_commit(texobj, &ti);
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internal_format,
                            GLsizei width, GLsizei height, GLint border,
                            GLsizei image_size, const void *data)
{
    if (target != GL_TEXTURE_2D) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
        level < 0 || border != 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    int unit = glparamstate.active_texture;
    int tex_id = glparamstate.texture_unit[unit].glcurtex;
    gltexture_ *currtex = &texture_list[tex_id];
    if (!TEXTURE_IS_RESERVED(texture_list[tex_id])) return;
    GXTexObj *texobj = &currtex->texobj;

    int native = native_gx_format(internal_format);
    if (native >= 0) {
        _ogx_texture_drop_alpha(currtex);
        native_tex_image(currtex, level, native, width, height, image_size,
                         data);
        return;
    }

    if (is_dxt35(internal_format)) {
        if (image_size != dxt35_size(width, height)) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        if (_ogx_texture_compression_hint == GL_FASTEST) {
            split_tex_image(tex_id, level,
                            internal_format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                            width, height, data);
            return;
        }
        GLint stored = _ogx_texture_compression_hint == GL_NICEST ?
            GL_RGBA8 : GL_RGB5_A1;
        uint8_t *rgba = NULL;
        if (data) {
            rgba = decode_dxt35_image(internal_format, width, height, data);
            if (!rgba) return;
        }
        UnpackState saved = tight_unpack();
        glTexImage2D(target, level, stored, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        restore_unpack(&saved);
        free(rgba);
        return;
    }
    if (!is_dxt1(internal_format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    if (image_size != dxt1_size(width, height)) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    _ogx_texture_drop_alpha(currtex);

    {
        float minlod, maxlod;
        GX_GetTexObjLOD(texobj, &minlod, &maxlod);
        _ogx_texture_make_writable(currtex,
                                   !(level == 0 && minlod == 0 && maxlod == 0));
    }

    OgxTextureInfo ti;
    if (!_ogx_texture_prepare_storage(texobj, level, width, height,
                                      GX_TF_CMPR, &ti))
        return;
    ti.ud.d.is_opaque = internal_format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT;

    uint8_t *dst = (uint8_t *)ti.texels +
        _ogx_texture_level_offset(level, ti.width, ti.height, ti.format);
    uint32_t size = _ogx_texture_level_size(width, height, ti.format);
    if (data) {
        int bw = (width + 3) / 4, bh = (height + 3) / 4;
        /* Blocks outside the image (when the size isn't a multiple of 8) are
         * not sampled, but keep them deterministic */
        if ((bw | bh) & 1) memset(dst, 0, size);
        dxt1_to_cmpr(data, bw, bh, dst, 0, 0, bw);
        DCFlushRange(dst, size);
        GX_InvalidateTexAll();
    }
    _ogx_texture_commit(texobj, &ti);
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glCompressedTexSubImage2D(GLenum target, GLint level,
                               GLint xoffset, GLint yoffset,
                               GLsizei width, GLsizei height, GLenum format,
                               GLsizei image_size, const void *data)
{
    if (target != GL_TEXTURE_2D) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    int unit = glparamstate.active_texture;
    gltexture_ *currtex =
        &texture_list[glparamstate.texture_unit[unit].glcurtex];
    OgxTextureInfo ti;
    if (is_dxt35(format)) {
        if ((xoffset | yoffset) & 3 || image_size != dxt35_size(width, height)) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        if (currtex->alpha_plane & OGX_ALPHA_PLANE) {
            split_tex_sub_image(glparamstate.texture_unit[unit].glcurtex,
                                level, xoffset, yoffset, width, height,
                                format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                                data);
            return;
        }
        /* Decoded into the texture's RGB5A3/RGBA8 storage */
        uint8_t *rgba = decode_dxt35_image(format, width, height, data);
        if (!rgba) return;
        UnpackState saved = tight_unpack();
        glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        restore_unpack(&saved);
        free(rgba);
        return;
    }
    if (!_ogx_texture_get_info(glparamstate.texture_unit[unit].glcurtex, &ti) ||
        !ti.texels || ti.format != GX_TF_CMPR || level > ti.maxlevel) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (!is_dxt1(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    if (currtex->alpha_plane & OGX_ALPHA_PLANE) {
        /* DXT1 blocks into a DXT3/DXT5 texture */
        set_error(GL_INVALID_OPERATION);
        return;
    }
    int level_w = ti.width >> level, level_h = ti.height >> level;
    if (level_w < 1) level_w = 1;
    if (level_h < 1) level_h = 1;
    if ((xoffset | yoffset) & 3 || xoffset < 0 || yoffset < 0 ||
        xoffset + width > level_w || yoffset + height > level_h ||
        ((width & 3) && xoffset + width != level_w) ||
        ((height & 3) && yoffset + height != level_h) ||
        image_size != dxt1_size(width, height)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    _ogx_texture_make_writable(currtex, true);
    _ogx_texture_get_info(glparamstate.texture_unit[unit].glcurtex, &ti);
    uint8_t *dst = (uint8_t *)ti.texels +
        _ogx_texture_level_offset(level, ti.width, ti.height, ti.format);
    dxt1_to_cmpr(data, (width + 3) / 4, (height + 3) / 4, dst,
                 xoffset / 4, yoffset / 4, (level_w + 3) / 4);
    DCFlushRange(dst, _ogx_texture_level_size(level_w, level_h, ti.format));
    GX_InvalidateTexAll();
    (void)currtex;
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glCompressedTexImage1D(GLenum target, GLint level, GLenum internal_format,
                            GLsizei width, GLint border, GLsizei image_size,
                            const void *data)
{
    set_error(GL_INVALID_ENUM);
}

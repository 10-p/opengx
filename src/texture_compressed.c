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
        native_tex_image(currtex, level, native, width, height, image_size,
                         data);
        return;
    }

    if (is_dxt35(internal_format)) {
        if (image_size != dxt35_size(width, height)) {
            set_error(GL_INVALID_VALUE);
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
        /* Decoded into the texture's RGB5A3/RGBA8 storage */
        if ((xoffset | yoffset) & 3 || image_size != dxt35_size(width, height)) {
            set_error(GL_INVALID_VALUE);
            return;
        }
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

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



/* GL_EXT_paletted_texture: palettes are kept per texture object in the GX
 * TLUT format and loaded into TMEM when the texture is bound to a stage.
 *
 * TMEM has 16 TLUT regions of 256 entries in the default libogc layout
 * (GX_TLUT0-15). We keep a small least-recently-used cache of which palette
 * is resident in which region, so that switching between a handful of
 * paletted textures doesn't reload their palettes on every bind. Since a
 * draw uses at most MAX_TEXTURE_UNITS textures, and those are the most
 * recently used, a region in use by the current draw is never evicted. */

#define GL_GLEXT_PROTOTYPES 1
#include "tlut.h"

#include "debug.h"
#include "state.h"
#include "texture.h"
#include "utils.h"

#include <GL/gxext.h>
#include <malloc.h>
#include <string.h>

#define NUM_TLUT_REGIONS 16
#define MAX_TLUT_ENTRIES 256 /* size of each GX_TLUTn region */

static struct {
    uint32_t serial; /* 0 = empty */
    uint32_t last_used;
} s_regions[NUM_TLUT_REGIONS];
static uint32_t s_use_counter = 0;
static uint32_t s_next_serial = 0;
uint32_t _ogx_tlut_loads = 0; /* statistics, for tests */

void _ogx_tlut_bind(GXTexObj *texobj, OgxTlut *tlut)
{
    int region = -1, oldest = 0;
    for (int i = 0; i < NUM_TLUT_REGIONS; i++) {
        if (s_regions[i].serial == tlut->serial) {
            region = i;
            break;
        }
        if (s_regions[i].last_used < s_regions[oldest].last_used)
            oldest = i;
    }
    if (region < 0) {
        region = oldest;
        GX_LoadTlut(&tlut->tlutobj, GX_TLUT0 + region);
        s_regions[region].serial = tlut->serial;
        _ogx_tlut_loads++;
    }
    s_regions[region].last_used = ++s_use_counter;
    GX_InitTexObjTlut(texobj, GX_TLUT0 + region);
}

void _ogx_tlut_free(OgxTlut *tlut, bool in_use)
{
    for (int i = 0; i < NUM_TLUT_REGIONS; i++) {
        if (s_regions[i].serial == tlut->serial)
            s_regions[i].serial = 0;
    }
    if (in_use) _ogx_free_after_gpu(tlut);
    else free(tlut);
}

static uint8_t tlut_format_for_internal_format(GLenum internal_format)
{
    switch (internal_format) {
    case 3:
    case GL_RGB:
    case GL_RGB4:
    case GL_RGB5:
    case GL_RGB8:
    case GL_R3_G3_B2:
        return GX_TL_RGB565;
    case 1:
    case 2:
    case GL_LUMINANCE:
    case GL_LUMINANCE4:
    case GL_LUMINANCE8:
    case GL_LUMINANCE_ALPHA:
    case GL_LUMINANCE4_ALPHA4:
    case GL_LUMINANCE8_ALPHA8:
    case GL_INTENSITY:
    case GL_INTENSITY4:
    case GL_INTENSITY8:
    case GL_ALPHA:
    case GL_ALPHA4:
    case GL_ALPHA8:
        return GX_TL_IA8;
    default:
        /* Anything with alpha (GL_RGBA, GL_RGBA8, GL_RGB5_A1, ...) */
        return GX_TL_RGB5A3;
    }
}

static int native_tlut_format(GLenum type)
{
    switch (type) {
    case GL_TLUT_GX_IA8_OGX: return GX_TL_IA8;
    case GL_TLUT_GX_RGB565_OGX: return GX_TL_RGB565;
    case GL_TLUT_GX_RGB5A3_OGX: return GX_TL_RGB5A3;
    default: return -1;
    }
}

/* RGB5A3: opaque texels are stored as 1RRRRRGGGGGBBBBB, the others as
 * 0AAARRRRGGGGBBBB. The hardware expands a 3-bit alpha of 7 to 255, so we
 * use the opaque (5-bit colour) form whenever the alpha rounds to 7. */
static inline uint16_t rgba_to_rgb5a3(uint8_t r, uint8_t g, uint8_t b,
                                      uint8_t a)
{
    int a3 = (a * 7 + 127) / 255;
    if (a3 == 7) {
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    }
    return (a3 << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}

static inline uint16_t rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* Convert "count" palette entries from the GL format into GX TLUT entries.
 * Returns false if the format/type combination is not supported. */
static bool convert_entries(uint16_t *dst, uint8_t gx_format,
                            GLenum internal_format, GLenum format,
                            GLenum type, const void *table, int count)
{
    int native = native_tlut_format(type);
    if (native >= 0) {
        if (native != gx_format) return false;
        memcpy(dst, table, count * 2);
        return true;
    }

    if (type != GL_UNSIGNED_BYTE) return false;

    const uint8_t *src = table;
    int ncomp, ri = 0, gi = 1, bi = 2, ai = -1, li = -1;
    switch (format) {
    case GL_RGBA: ncomp = 4; ai = 3; break;
    case GL_BGRA: ncomp = 4; ri = 2; bi = 0; ai = 3; break;
    case GL_RGB: ncomp = 3; break;
    case GL_BGR: ncomp = 3; ri = 2; bi = 0; break;
    case GL_LUMINANCE: ncomp = 1; li = 0; break;
    case GL_LUMINANCE_ALPHA: ncomp = 2; li = 0; ai = 1; break;
    case GL_INTENSITY: ncomp = 1; li = 0; ai = 0; break;
    case GL_ALPHA: ncomp = 1; ai = 0; break;
    default: return false;
    }

    bool intensity_internal = internal_format == GL_INTENSITY ||
        internal_format == GL_INTENSITY4 || internal_format == GL_INTENSITY8;
    for (int i = 0; i < count; i++, src += ncomp) {
        uint8_t r, g, b, a;
        if (li >= 0) {
            r = g = b = src[li];
        } else if (format == GL_ALPHA) {
            r = g = b = 255;
        } else {
            r = src[ri]; g = src[gi]; b = src[bi];
        }
        a = ai >= 0 ? src[ai] : 255;

        switch (gx_format) {
        case GX_TL_RGB565:
            dst[i] = rgb_to_rgb565(r, g, b);
            break;
        case GX_TL_IA8:
            {
                uint8_t l = li >= 0 ? r : (r + g + b) / 3;
                if (intensity_internal) a = l;
                dst[i] = (a << 8) | l;
            }
            break;
        default:
            dst[i] = rgba_to_rgb5a3(r, g, b, a);
            break;
        }
    }
    return true;
}

static gltexture_ *current_texture(GLenum target)
{
    if (target != GL_TEXTURE_2D) {
        set_error(GL_INVALID_ENUM);
        return NULL;
    }
    int unit = glparamstate.active_texture;
    int name = glparamstate.texture_unit[unit].glcurtex;
    if (!TEXTURE_IS_RESERVED(texture_list[name])) {
        TEXTURE_RESERVE(texture_list[name]);
    }
    return &texture_list[name];
}

static void tlut_updated(OgxTlut *tlut)
{
    DCFlushRange(tlut->data, tlut->gx_entries * 2);
    GX_InitTlutObj(&tlut->tlutobj, tlut->data, tlut->gx_format,
                   tlut->gx_entries);
    tlut->serial = ++s_next_serial;
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glColorTableEXT(GLenum target, GLenum internal_format, GLsizei width,
                     GLenum format, GLenum type, const void *table)
{
    if (target == GL_PROXY_TEXTURE_2D) return;

    gltexture_ *t = current_texture(target);
    if (!t) return;

    if (width <= 0 || width > MAX_TLUT_ENTRIES || (width & (width - 1))) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    int native = native_tlut_format(type);
    uint8_t gx_format = native >= 0 ?
        native : tlut_format_for_internal_format(internal_format);
    /* TLUT loads are in units of 16 entries */
    uint16_t gx_entries = (width + 15) & ~15;

    OgxTlut *tlut = t->tlut;
    if (tlut) {
        /* The GPU might still have to load the old palette for draws which
         * are in the FIFO */
        GX_DrawDone();
        if (tlut->gx_entries < gx_entries) {
            _ogx_tlut_free(tlut, false);
            tlut = t->tlut = NULL;
        }
    }
    if (!tlut) {
        tlut = memalign(32, sizeof(OgxTlut) + gx_entries * 2);
        if (!tlut) {
            set_error(GL_OUT_OF_MEMORY);
            return;
        }
        t->tlut = tlut;
    }
    tlut->entries = width;
    tlut->gx_entries = gx_entries;
    tlut->gx_format = gx_format;
    tlut->internal_format = internal_format;
    memset(tlut->data, 0, gx_entries * 2);
    if (table && !convert_entries(tlut->data, gx_format, internal_format,
                                  format, type, table, width)) {
        warning("glColorTableEXT: unsupported format %04x / type %04x",
                format, type);
        set_error(GL_INVALID_ENUM);
    }
    tlut_updated(tlut);
}

void glColorSubTableEXT(GLenum target, GLsizei start, GLsizei count,
                        GLenum format, GLenum type, const void *data)
{
    gltexture_ *t = current_texture(target);
    if (!t) return;

    OgxTlut *tlut = t->tlut;
    if (!tlut || start < 0 || count < 0 || start + count > tlut->entries) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    GX_DrawDone();
    if (!convert_entries(tlut->data + start, tlut->gx_format,
                         tlut->internal_format, format, type, data, count)) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    tlut_updated(tlut);
}

void glGetColorTableParameterivEXT(GLenum target, GLenum pname,
                                   GLint *params)
{
    gltexture_ *t = current_texture(target);
    if (!t) return;
    OgxTlut *tlut = t->tlut;
    switch (pname) {
    case GL_COLOR_TABLE_WIDTH:
        *params = tlut ? tlut->entries : 0; break;
    case GL_COLOR_TABLE_FORMAT:
        *params = tlut ? tlut->internal_format : GL_RGBA; break;
    default:
        set_error(GL_INVALID_ENUM);
    }
}

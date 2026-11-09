/*****************************************************************************
Copyright (c) 2011  David Guillen Fandos (david@davidgf.net)
Copyright (c) 2024  Alberto Mardegan (mardy@users.sourceforge.net)
All rights reserved.

Attention! Contains pieces of code from others such as Mesa and GRRLib

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

#include "texture.h"

#include "call_lists.h"
#include "debug.h"
#include "image_DXT.h"
#include "pixels.h"
#include "tlut.h"
#include "state.h"
#include "utils.h"

#include <malloc.h>

static inline int curr_tex()
{
    int unit = glparamstate.active_texture;
    return glparamstate.texture_unit[unit].glcurtex;
}

/* libogc 3.2.1's GX_GetTexBufferSize() computes GX_TF_CI8 sizes as if it
 * were a 16-bit format (8x4 tiles are right, 4x4 is what it uses): that
 * doubles the buffer and, worse, gives wrong mipmap offsets. CI8 has the same
 * layout as I8, so compute it as such. */
static inline uint32_t size_format(uint32_t format)
{
    return format == GX_TF_CI8 ? GX_TF_I8 : format;
}

static uint32_t calc_memory(int w, int h, uint32_t format)
{
    return GX_GetTexBufferSize(w, h, size_format(format), GX_FALSE, 0);
}

// Returns the number of bytes required to store a texture with all its bitmaps
static uint32_t calc_tex_size(int w, int h, uint32_t format)
{
    return GX_GetTexBufferSize(w, h, size_format(format), GX_TRUE, 20);
}
// Deduce the original texture size given the current size and level
static int calc_original_size(int level, int s)
{
    while (level > 0) {
        s = 2 * s;
        level--;
    }
    return s;
}
// Given w,h,level,and bpp, returns the offset to the mipmap at level "level"
uint32_t _ogx_texture_level_offset(int level, int w, int h, uint32_t format)
{
    return GX_GetTexBufferSize(w, h, size_format(format), GX_TRUE, level);
}

uint32_t _ogx_texture_level_size(int w, int h, uint32_t format)
{
    return calc_memory(w, h, format);
}

static uint32_t calc_mipmap_offset(int level, int w, int h, uint32_t format)
{
    return GX_GetTexBufferSize(w, h, size_format(format), GX_TRUE, level);
}

static u8 gl_filter_to_gx(GLint gl_filter)
{
    switch (gl_filter) {
    case GL_NEAREST: return GX_NEAR;
    case GL_LINEAR: return GX_LINEAR;
    case GL_NEAREST_MIPMAP_NEAREST: return GX_NEAR_MIP_NEAR;
    case GL_LINEAR_MIPMAP_NEAREST: return GX_LIN_MIP_NEAR;
    case GL_NEAREST_MIPMAP_LINEAR: return GX_NEAR_MIP_LIN;
    case GL_LINEAR_MIPMAP_LINEAR: return GX_LIN_MIP_LIN;
    }
    return GX_NEAR;
}

static unsigned char gcgl_texwrap_conv(GLint param)
{
    switch (param) {
    case GL_MIRRORED_REPEAT:
        return GX_MIRROR;
    case GL_CLAMP:
    case GL_CLAMP_TO_EDGE:
    case GL_CLAMP_TO_BORDER:
        return GX_CLAMP;
    case GL_REPEAT:
    default:
        return GX_REPEAT;
    };
}

static void texture_get_info(const GXTexObj *obj, OgxTextureInfo *info)
{
    GX_GetTexObjAll(obj, &info->texels,
                    &info->width, &info->height,
                    &info->format,
                    &info->wraps, &info->wrapt,
                    &info->mipmap);
    if (info->texels) {
        info->texels = MEM_PHYSICAL_TO_K0(info->texels);
    } else {
        /* Ensure size is 0 if there's no texture  */
        info->width = info->height = 0;
    }

    float minlevel, maxlevel;
    GX_GetTexObjLOD(obj, &minlevel, &maxlevel);
    info->minlevel = minlevel;
    info->maxlevel = maxlevel;
    info->ud.ptr = GX_GetTexObjUserData(obj);
    GX_GetTexObjFilterMode(obj, &info->min_filter, &info->mag_filter);

    /* Check if we wanted an alpha channel instead */
    if (info->format == GX_TF_I8 && info->ud.d.is_alpha)
        info->format = GX_TF_A8;
}

/* Deferred frees. opengx's draw sync tokens restart from 0 at every
 * ogx_prepare_swap_buffers(), which sends a token 0 itself; so a FIFO point
 * is identified by (number of swap markers sent before it, token). */
#define MAX_DEFERRED_FREES 128
static struct {
    void *ptr;
    uint32_t swap;
    uint16_t token;
} s_deferred[MAX_DEFERRED_FREES];
static int s_num_deferred = 0;
static uint32_t s_swaps_sent = 0;
static volatile uint32_t s_swaps_received = 0;
uint32_t _ogx_texture_orphans = 0;

void _ogx_texture_swap_marker_sent(void) { s_swaps_sent++; }
/* Called from the draw sync interrupt */
void _ogx_texture_swap_marker_received(void) { s_swaps_received++; }

uint32_t _ogx_swaps_sent(void) { return s_swaps_sent; }

bool _ogx_fifo_point_passed(uint32_t swap, uint16_t token)
{
    uint32_t received = s_swaps_received;
    if (received > swap) return true;
    return received == swap &&
        *(volatile uint16_t *)&_ogx_draw_sync_token_received >= token;
}
#define fifo_point_passed _ogx_fifo_point_passed

static void reap_deferred(void)
{
    int j = 0;
    for (int i = 0; i < s_num_deferred; i++) {
        if (fifo_point_passed(s_deferred[i].swap, s_deferred[i].token)) {
            free(s_deferred[i].ptr);
        } else {
            s_deferred[j++] = s_deferred[i];
        }
    }
    s_num_deferred = j;
}

void _ogx_free_after_gpu(void *ptr)
{
    reap_deferred();
    if (s_num_deferred == MAX_DEFERRED_FREES) {
        GX_DrawDone();
        for (int i = 0; i < s_num_deferred; i++) free(s_deferred[i].ptr);
        s_num_deferred = 0;
    }
    s_deferred[s_num_deferred].ptr = ptr;
    s_deferred[s_num_deferred].swap = s_swaps_sent;
    s_deferred[s_num_deferred].token = send_draw_sync_token();
    s_num_deferred++;
}

void _ogx_texture_make_writable(gltexture_ *texture, bool keep_contents)
{
    reap_deferred();
    if (!texture->in_use) return;
    texture->in_use = 0;

    void *old = GX_GetTexObjData(&texture->texobj);
    if (!old) return;
    old = MEM_PHYSICAL_TO_K0(old);

    OgxTextureInfo ti;
    texture_get_info(&texture->texobj, &ti);
    /* Buffers hold either level 0 only or a full chain */
    uint32_t size = ti.minlevel == 0 && ti.maxlevel == 0 ?
        calc_memory(ti.width, ti.height, ti.format) :
        calc_tex_size(ti.width, ti.height, ti.format);
    void *texels = memalign(32, size);
    if (!texels) {
        /* No memory for a copy: wait for the GPU instead */
        GX_DrawDone();
        return;
    }
    if (keep_contents) {
        memcpy(texels, old, size);
        DCFlushRange(texels, size);
    }
    GX_InitTexObjData(&texture->texobj, texels);
    _ogx_free_after_gpu(old);
    _ogx_texture_orphans++;
}

bool _ogx_texture_get_info(GLuint texture_name, OgxTextureInfo *info)
{
    if (!TEXTURE_IS_RESERVED(texture_list[texture_name]))
        return false;

    texture_get_info(&texture_list[texture_name].texobj, info);
    return true;
}

bool _ogx_texture_get_texobj(GLuint texture_name, GXTexObj *texobj)
{
    if (!TEXTURE_IS_RESERVED(texture_list[texture_name]))
        return false;

    memcpy(texobj, &texture_list[texture_name].texobj, sizeof(*texobj));
    return true;
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    /* For the time being, all the parameters we support take integer values */
    glTexParameteri(target, pname, param);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (target != GL_TEXTURE_2D)
        return;

    gltexture_ *currtex = &texture_list[curr_tex()];
    u8 wraps, wrapt, min_filter, mag_filter;

    switch (pname) {
    case GL_TEXTURE_WRAP_S:
        wrapt = GX_GetTexObjWrapT(&currtex->texobj);
        wraps = gcgl_texwrap_conv(param);
        GX_InitTexObjWrapMode(&currtex->texobj, wraps, wrapt);
        break;
    case GL_TEXTURE_WRAP_T:
        wraps = GX_GetTexObjWrapS(&currtex->texobj);
        wrapt = gcgl_texwrap_conv(param);
        GX_InitTexObjWrapMode(&currtex->texobj, wraps, wrapt);
        break;
    case GL_TEXTURE_MAG_FILTER:
        GX_GetTexObjFilterMode(&currtex->texobj, &min_filter, &mag_filter);
        /* Only GX_NEAR and GX_LINEAR are supported for magnification */
        mag_filter = (param == GL_NEAREST ||
                      param == GL_NEAREST_MIPMAP_NEAREST ||
                      param == GL_NEAREST_MIPMAP_LINEAR) ? GX_NEAR : GX_LINEAR;
        GX_InitTexObjFilterMode(&currtex->texobj, min_filter, mag_filter);
        break;
    case GL_TEXTURE_MIN_FILTER:
        GX_GetTexObjFilterMode(&currtex->texobj, &min_filter, &mag_filter);
        min_filter = gl_filter_to_gx(param);
        GX_InitTexObjFilterMode(&currtex->texobj, min_filter, mag_filter);
        GX_GetTexObjFilterMode(&currtex->texobj, &min_filter, &mag_filter);
        break;
    };
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    /* All supported pname's only take a single param */
    glTexParameterf(target, pname, params[0]);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    /* All supported pname's only take a single param */
    glTexParameteri(target, pname, params[0]);
}

void glTexGend(GLenum coord, GLenum pname, GLdouble param)
{
    glTexGeni(coord, pname, param);
}

void glTexGenf(GLenum coord, GLenum pname, GLfloat param)
{
    glTexGeni(coord, pname, param);
}

void glTexGeni(GLenum coord, GLenum pname, GLint param)
{
    /* Since in GX we cannot set different modes per texture coordinate, we
     * only look at the S coordinate, hoping that the other enabled coordinates
     * will use the same parameters. */
    if (coord != GL_S) return;

    OgxTextureUnit *tu = active_tex_unit();

    switch (pname) {
    case GL_TEXTURE_GEN_MODE:
        tu->gen_mode = param;
        glparamstate.dirty.bits.dirty_attributes = 1;
        break;
    }
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
    GLfloat p[4];
    int num_params = 1;
    switch (pname) {
    case GL_EYE_PLANE:
    case GL_OBJECT_PLANE:
        num_params = 4;
        break;
    }
    for (int i = 0; i < num_params; i++)
        p[i] = params[i];
    glTexGenfv(coord, pname, p);
}

void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
    OgxTextureUnit *tu = active_tex_unit();

    switch (pname) {
    case GL_TEXTURE_GEN_MODE:
        glTexGeni(coord, pname, params[0]);
        return;
    case GL_EYE_PLANE:
        if (coord == GL_S) {
            floatcpy(tu->texture_eye_plane_s, params, 4);
        } else if (coord == GL_T) {
            floatcpy(tu->texture_eye_plane_t, params, 4);
        }
        break;
    case GL_OBJECT_PLANE:
        if (coord == GL_S) {
            floatcpy(tu->texture_object_plane_s, params, 4);
        } else if (coord == GL_T) {
            floatcpy(tu->texture_object_plane_t, params, 4);
        }
        break;
    }
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glTexGeniv(GLenum coord, GLenum pname, const GLint *params)
{
    GLfloat p[4];
    int num_params = 1;
    switch (pname) {
    case GL_EYE_PLANE:
    case GL_OBJECT_PLANE:
        num_params = 4;
        break;
    default:
        glTexGeni(coord, pname, params[0]);
        return;
    }
    for (int i = 0; i < num_params; i++)
        p[i] = params[i];
    glTexGenfv(coord, pname, p);
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    /* For the time being, all the parameters we support take integer values
     * (GL_RGB_SCALE and GL_ALPHA_SCALE only accept 1.0, 2.0 and 4.0) */
    glTexEnvi(target, pname, param);
}

/* Map a GL_RGB_SCALE / GL_ALPHA_SCALE value to the TEV output scale. Returns
 * 0xff for values not allowed by the spec. */
static uint8_t gl_scale_to_gx(GLint scale)
{
    switch (scale) {
    case 1: return GX_CS_SCALE_1;
    case 2: return GX_CS_SCALE_2;
    case 4: return GX_CS_SCALE_4;
    default: return 0xff;
    }
}

/* Last glTexImage2D(GL_PROXY_TEXTURE_2D, ...) request, for the queries */
static struct {
    GLint width, height, internal_format;
} s_proxy;

static GLint gx_format_to_gl(uint8_t gx_format, const OgxTextureUserData *ud)
{
    switch (gx_format) {
    case GX_TF_RGBA8: return GL_RGBA8;
    case GX_TF_RGB565: return GL_RGB;
    case GX_TF_RGB5A3: return GL_RGB5_A1;
    case GX_TF_IA8: return GL_LUMINANCE_ALPHA;
    case GX_TF_I8:
        if (ud->d.is_alpha) return GL_ALPHA;
        return ud->d.is_opaque ? GL_LUMINANCE : GL_INTENSITY;
    case GX_TF_A8: return GL_ALPHA;
    case GX_TF_I4: return GL_LUMINANCE4;
    case GX_TF_CI8: return GL_COLOR_INDEX8_EXT;
    case GX_TF_CMPR:
        return ud->d.is_opaque ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT :
            GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    default: return GL_RGBA;
    }
}

static void component_sizes(GLint internal_format, int sizes[6])
{
    /* r, g, b, a, luminance, intensity */
    for (int i = 0; i < 6; i++) sizes[i] = 0;
    switch (internal_format) {
    case GL_RGBA8: sizes[0] = sizes[1] = sizes[2] = sizes[3] = 8; break;
    case GL_RGB: sizes[0] = sizes[2] = 5; sizes[1] = 6; break;
    case GL_RGB5_A1: sizes[0] = sizes[1] = sizes[2] = 5; sizes[3] = 1; break;
    case GL_LUMINANCE_ALPHA: sizes[3] = sizes[4] = 8; break;
    case GL_LUMINANCE: sizes[4] = 8; break;
    case GL_INTENSITY: sizes[5] = 8; break;
    case GL_ALPHA: sizes[3] = 8; break;
    case GL_LUMINANCE4: sizes[4] = 4; break;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: sizes[3] = 1; /* fall through */
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
        sizes[0] = sizes[2] = 5; sizes[1] = 6; break;
    }
}

void glGetTexLevelParameteriv(GLenum target, GLint level,
                              GLenum pname, GLint *params)
{
    GLint width = 0, height = 0, internal_format = GL_RGBA;
    bool compressed = false;
    GLint compressed_size = 0;

    if (target == GL_PROXY_TEXTURE_2D) {
        if (level == 0) {
            width = s_proxy.width;
            height = s_proxy.height;
            internal_format = s_proxy.internal_format;
        }
    } else if (target == GL_TEXTURE_2D) {
        OgxTextureInfo ti;
        if (level < 0 || level > 10) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        if (_ogx_texture_get_info(curr_tex(), &ti) && ti.texels &&
            level >= ti.minlevel && level <= ti.maxlevel) {
            width = ti.width >> level ? ti.width >> level : 1;
            height = ti.height >> level ? ti.height >> level : 1;
            internal_format = gx_format_to_gl(ti.format, &ti.ud);
            compressed = ti.format == GX_TF_CMPR;
            compressed_size = ((width + 3) / 4) * ((height + 3) / 4) * 8;
        }
    } else {
        set_error(GL_INVALID_ENUM);
        return;
    }

    int sizes[6];
    component_sizes(width ? internal_format : 0, sizes);
    switch (pname) {
    case GL_TEXTURE_WIDTH: *params = width; break;
    case GL_TEXTURE_HEIGHT: *params = height; break;
    case GL_TEXTURE_DEPTH: *params = width ? 1 : 0; break;
    case GL_TEXTURE_BORDER: *params = 0; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = internal_format; break;
    case GL_TEXTURE_RED_SIZE: *params = sizes[0]; break;
    case GL_TEXTURE_GREEN_SIZE: *params = sizes[1]; break;
    case GL_TEXTURE_BLUE_SIZE: *params = sizes[2]; break;
    case GL_TEXTURE_ALPHA_SIZE: *params = sizes[3]; break;
    case GL_TEXTURE_LUMINANCE_SIZE: *params = sizes[4]; break;
    case GL_TEXTURE_INTENSITY_SIZE: *params = sizes[5]; break;
    case GL_TEXTURE_COMPRESSED: *params = compressed; break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
        if (!compressed) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        *params = compressed_size;
        break;
    default:
        set_error(GL_INVALID_ENUM);
    }
}

void glGetTexLevelParameterfv(GLenum target, GLint level,
                              GLenum pname, GLfloat *params)
{
    GLint value = 0;
    glGetTexLevelParameteriv(target, level, pname, &value);
    *params = value;
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    HANDLE_CALL_LIST(TEX_ENV, target, pname, param);

    if (target == GL_POINT_SPRITE) {
        switch (pname) {
        case GL_COORD_REPLACE:
            glparamstate.point_sprites_coord_replace = param;
            glparamstate.dirty.bits.dirty_attributes = 1;
            break;
        }
        return;
    }

    OgxTextureUnit *tu = active_tex_unit();
    switch (pname) {
    case GL_COMBINE_ALPHA:
        tu->combine_alpha = param;
        break;
    case GL_COMBINE_RGB:
        tu->combine_rgb = param;
        break;
    case GL_OPERAND0_ALPHA:
    case GL_OPERAND1_ALPHA:
    case GL_OPERAND2_ALPHA:
        tu->operand_alpha[pname - GL_OPERAND0_ALPHA] = param;
        break;
    case GL_SOURCE0_ALPHA:
    case GL_SOURCE1_ALPHA:
    case GL_SOURCE2_ALPHA:
        tu->source_alpha[pname - GL_SOURCE0_ALPHA] = param;
        break;
    case GL_OPERAND0_RGB:
    case GL_OPERAND1_RGB:
    case GL_OPERAND2_RGB:
        tu->operand_rgb[pname - GL_OPERAND0_RGB] = param;
        break;
    case GL_SOURCE0_RGB:
    case GL_SOURCE1_RGB:
    case GL_SOURCE2_RGB:
        tu->source_rgb[pname - GL_SOURCE0_RGB] = param;
        break;
    case GL_TEXTURE_ENV_MODE:
        tu->mode = param;
        break;
    case GL_RGB_SCALE:
    case GL_ALPHA_SCALE:
        {
            uint8_t scale = gl_scale_to_gx(param);
            if (scale == 0xff) {
                set_error(GL_INVALID_VALUE);
                return;
            }
            if (pname == GL_RGB_SCALE)
                tu->rgb_scale = scale;
            else
                tu->alpha_scale = scale;
        }
        break;
    }
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    OgxTextureUnit *tu = active_tex_unit();
    switch (pname) {
    case GL_TEXTURE_ENV_COLOR:
        tu->color = gxcol_new_fv(params);
        break;
    default:
        glTexEnvf(target, pname, params[0]);
        return;
    }
    glparamstate.dirty.bits.dirty_tev = 1;
}

void glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{
    switch (pname) {
    case GL_TEXTURE_ENV_COLOR:
        GLfloat p[4] = {
            scaled_int(params[0]),
            scaled_int(params[1]),
            scaled_int(params[2]),
            scaled_int(params[3]),
        };
        glTexEnvfv(target, pname, p);
        break;
    default:
        glTexEnvi(target, pname, params[0]);
        break;
    }
}

void glTexImage1D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLint border, GLenum format, GLenum type,
                  const GLvoid *pixels)
{
    glTexImage2D(GL_TEXTURE_2D, level, internalFormat, width, 1,
                 border, format, type, pixels);
}

static void update_texture(const void *data, int level, GLenum format, GLenum type,
                           int width, int height,
                           GXTexObj *obj, OgxTextureInfo *ti, int x, int y)
{
    unsigned char *dst_addr = ti->texels;
    // Inconditionally convert to 565 all inputs without alpha channel
    // Alpha inputs may be stripped if the user specifies an alpha-free internal format
    if (ti->format != GX_TF_CMPR) {
        // Calculate the offset and address of the mipmap
        uint32_t offset = calc_mipmap_offset(level, ti->width, ti->height, ti->format);
        dst_addr += offset;

        int dstpitch = _ogx_pitch_for_width(ti->format, ti->width >> level);

        uint32_t gx_format = ti->format;
        if (gx_format == GX_TF_I8 && ti->ud.d.is_alpha)
            gx_format = GX_TF_A8;
        _ogx_bytes_to_texture(data, format, type, width, height,
                              dst_addr, gx_format, x, y, dstpitch);
    } else {
        // Compressed texture
        if (x != 0 || y != 0 || ti->width != width) {
            warning("Update of compressed textures not implemented!");
            return;
        }

        // Calculate the offset and address of the mipmap
        uint32_t offset = calc_mipmap_offset(level, ti->width, ti->height, ti->format);
        dst_addr += offset;

        // Simplify but keep in mind the swapping
        int needswap = 0;
        if (format == GL_BGR) {
            format = GL_RGB;
            needswap = 1;
        }
        if (format == GL_BGRA) {
            format = GL_RGBA;
            needswap = 1;
        }

        _ogx_convert_rgb_image_to_DXT1((unsigned char *)data, dst_addr,
                                       width, height, needswap);
    }

    /* Flush the rows of tiles touched by the update: a sub-image is spread
     * over whole tile rows in the tiled layout (upstream flushed only
     * width*height bytes from the start of the level, which misses the data
     * of sub-images not at the origin; invisible in Dolphin, which doesn't
     * emulate the data cache). */
    int level_w = ti->width >> level ? ti->width >> level : 1;
    int level_h = ti->height >> level ? ti->height >> level : 1;
    uint32_t level_size = calc_memory(level_w, level_h, ti->format);
    int pitch = _ogx_pitch_for_width(ti->format, level_w);
    if (ti->format != GX_TF_CMPR && pitch > 0) {
        int tile_h = ti->format == GX_TF_I4 ? 8 : 4;
        uint32_t row_bytes = pitch * tile_h;
        uint32_t start = (y / tile_h) * row_bytes;
        uint32_t end = ((y + height + tile_h - 1) / tile_h) * row_bytes;
        if (end > level_size) end = level_size;
        DCFlushRange(dst_addr + start, end - start);
    } else {
        DCFlushRange(dst_addr, level_size);
    }

    // Slow but necessary! The new textures may be in the same region of some old cached textures
    GX_InvalidateTexAll();

    glparamstate.dirty.bits.dirty_tev = 1;
}

/* Make sure the current texture has a texel buffer able to hold the given
 * mipmap level with the given GX format, (re)allocating it if needed, and
 * fill "ti" with the resulting texture information. */
bool _ogx_texture_prepare_storage(GXTexObj *texobj, GLint level,
                                  int width, int height, uint8_t gx_format,
                                  OgxTextureInfo *ti)
{
    // We *may* need to delete and create a new texture, depending if the user wants to add some mipmap levels
    // or wants to create a new texture from scratch
    texture_get_info(texobj, ti);

    /* Level n of a WxH texture is max(1, W >> n) x max(1, H >> n). If the
     * given size is that of the existing texture's level, keep its base
     * size; only otherwise derive the base size as "size << level" (which
     * is ambiguous once an axis has reached 1: upstream turned e.g. the 1x2
     * tail of a 128x256 chain into a new 128x512... texture, losing all the
     * other levels). */
    int wi, he;
    if (level > 0 && ti->texels &&
        width == (ti->width >> level ? ti->width >> level : 1) &&
        height == (ti->height >> level ? ti->height >> level : 1)) {
        wi = ti->width;
        he = ti->height;
    } else {
        wi = calc_original_size(level, width);
        he = calc_original_size(level, height);
    }
    uint8_t old_format = ti->format == GX_TF_A8 ? GX_TF_I8 : ti->format;
    bool is_alpha = false;
    /* GX_TF_A8 is not supported by Dolphin and it's not properly handed by
     * a real Wii either. */
    if (gx_format == GX_TF_A8) {
        gx_format = GX_TF_I8;
        is_alpha = true; /* Remember that we wanted alpha, though */
    }
    ti->format = gx_format;
    ti->ud.d.is_alpha = is_alpha;
    ti->ud.d.is_opaque = 0;
    ti->ud.d.is_reserved = 1;
    char onelevel = ti->minlevel == 0 && ti->maxlevel == 0;

    // Check if the texture has changed its geometry and proceed to delete it
    // If the specified level is zero, create a onelevel texture to save memory.
    // A format change which changes the size of the buffer also needs a new
    // allocation (upstream reused the old buffer, overflowing it).
    if (wi != ti->width || he != ti->height ||
        (ti->texels && calc_memory(wi, he, old_format) !=
         calc_memory(wi, he, gx_format))) {
        if (ti->texels != 0)
            free(ti->texels);
        uint32_t required_size;
        if (level == 0) {
            required_size = calc_memory(width, height, ti->format);
            onelevel = 1;
        } else {
            required_size = calc_tex_size(wi, he, ti->format);
            onelevel = 0;
        }
        ti->texels = memalign(32, required_size);
        if (!ti->texels) {
            warning("Failed to allocate %u bytes for texture", required_size);
            set_error(GL_OUT_OF_MEMORY);
            GX_InitTexObj(texobj, NULL, 0, 0, 0, GX_REPEAT, GX_REPEAT, 0);
            GX_InitTexObjUserData(texobj, ti->ud.ptr);
            return false;
        }
        ti->minlevel = level;
        ti->maxlevel = level;
        ti->width = wi;
        ti->height = he;
    }
    if (ti->maxlevel < level)
        ti->maxlevel = level;
    if (ti->minlevel > level)
        ti->minlevel = level;

    if (onelevel == 1 && level != 0) {
        // We allocated a onelevel texture (base level 0) but now
        // we are uploading a non-zero level, so we need to create a mipmap capable buffer
        // and copy the level zero texture
        uint32_t tsize = calc_memory(wi, he, ti->format);
        unsigned char *oldbuf = ti->texels;

        uint32_t required_size = calc_tex_size(wi, he, ti->format);
        ti->texels = memalign(32, required_size);
        if (!ti->texels) {
            warning("Failed to allocate memory for texture mipmap (%d)", errno);
            set_error(GL_OUT_OF_MEMORY);
            ti->texels = oldbuf;
            ti->maxlevel = ti->minlevel = 0;
            return false;
        }

        memcpy(ti->texels, oldbuf, tsize);
        free(oldbuf);
    }
    return true;
}

/* Write the texture information back into the GX texture object */
void _ogx_texture_commit(GXTexObj *texobj, const OgxTextureInfo *ti)
{
    if (ti->format == GX_TF_CI8) {
        /* The TLUT name is assigned when the texture is bound to a stage,
         * see _ogx_tlut_bind() */
        GX_InitTexObjCI(texobj, ti->texels, ti->width, ti->height, ti->format,
                        ti->wraps, ti->wrapt, GX_TRUE, GX_TLUT0);
    } else {
        GX_InitTexObj(texobj, ti->texels,
                      ti->width, ti->height, ti->format, ti->wraps, ti->wrapt,
                      GX_TRUE);
    }
    GX_InitTexObjLOD(texobj, ti->min_filter, ti->mag_filter,
                     ti->minlevel, ti->maxlevel, 0, GX_ENABLE, GX_ENABLE, GX_ANISO_1);
    GX_InitTexObjUserData(texobj, ti->ud.ptr);
}

void glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *data)
{
    int tex_id = curr_tex();
    if (target == GL_PROXY_TEXTURE_2D) {
        /* Remember the request, for glGetTexLevelParameteriv(): GX takes
         * up to 1024x1024 */
        bool ok = level == 0 && width > 0 && height > 0 &&
            width <= 1024 && height <= 1024 && border == 0;
        s_proxy.width = ok ? width : 0;
        s_proxy.height = ok ? height : 0;
        s_proxy.internal_format = ok ? internalFormat : 0;
        return;
    }
    // Initial checks
    if (!TEXTURE_IS_RESERVED(texture_list[tex_id]))
        return;
    if (target != GL_TEXTURE_2D)
        return; // FIXME Implement non 2D textures

    gltexture_ *currtex = &texture_list[tex_id];
    GXTexObj *texobj = &currtex->texobj;

    /* Instead of waiting for the GPU (upstream did a GX_DrawDone() here),
     * give the texture a new buffer if queued draws may still read it. The
     * old contents only matter for the other levels of a mipmapped one. */
    {
        float minlod, maxlod;
        GX_GetTexObjLOD(texobj, &minlod, &maxlod);
        _ogx_texture_make_writable(currtex,
                                   !(level == 0 && minlod == 0 && maxlod == 0));
    }

    uint8_t gx_format = _ogx_find_best_gx_format(format, internalFormat,
                                                 width, height);
    if (!data) {
        /* This typically happens when setting up a texture for attaching it to
         * a FBO; in this case, make sure that the format is not compressed,
         * since GX does not support copying the EFB into a compressed texture.
         */
        if (gx_format == GX_TF_CMPR) gx_format = GX_TF_RGB565;
    }
    if (gx_format == GX_TF_CI8 && format != GL_COLOR_INDEX) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    OgxTextureInfo ti;
    if (!_ogx_texture_prepare_storage(texobj, level, width, height, gx_format,
                                      &ti))
        return;

    if (data) {
        update_texture(data, level, format, type, width, height,
                       texobj, &ti, 0, 0);
    }

    /* Remember if the GL base format has no alpha */
    ti.ud.d.is_opaque = !_ogx_internal_format_has_alpha(internalFormat);
    _ogx_texture_commit(texobj, &ti);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *data)
{
    int tex_id = curr_tex();
    if (!TEXTURE_IS_USED(texture_list[tex_id])) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (target != GL_TEXTURE_2D) {
        warning("glTexSubImage2D with target 0x%04x not supported", target);
        return;
    }

    gltexture_ *currtex = &texture_list[tex_id];

    /* Upstream wrote into texels that queued draws could still read. The old
     * contents can be dropped only if this replaces all of a single-level
     * texture. */
    {
        float minlod, maxlod;
        GX_GetTexObjLOD(&currtex->texobj, &minlod, &maxlod);
        bool whole = level == 0 && minlod == 0 && maxlod == 0 &&
            xoffset == 0 && yoffset == 0 &&
            width == GX_GetTexObjWidth(&currtex->texobj) &&
            height == GX_GetTexObjHeight(&currtex->texobj);
        _ogx_texture_make_writable(currtex, !whole);
    }

    OgxTextureInfo ti;
    texture_get_info(&currtex->texobj, &ti);
    if (level > ti.maxlevel) {
        /* OpenGL does not specify this as an error, so we should probably
         * handle this by allocating new mipmap levels as needed. but let's
         * leave it as a TODO for now. */
        warning("glTexSubImage2D called with level %d when max is %d",
                level, ti.maxlevel);
        return;
    }

    update_texture(data, level, format, type, width, height,
                   &currtex->texobj, &ti, xoffset, yoffset);
}

void glBindTexture(GLenum target, GLuint texture)
{
    if (texture < 0 || texture >= _MAX_GL_TEX)
        return;

    HANDLE_CALL_LIST(BIND_TEXTURE, target, texture);

    if (!TEXTURE_IS_RESERVED(texture_list[texture])) {
        TEXTURE_RESERVE(texture_list[texture]);
    }

    /* We don't load the texture now, since its texels might not have been
     * defined yet. We do this when setting up the texturing TEV stage. */
    int unit = glparamstate.active_texture;
    glparamstate.texture_unit[unit].glcurtex = texture;

    glparamstate.dirty.bits.dirty_tev = 1;
}

void glTexImage3D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth,
                  GLint border, GLenum format, GLenum type,
                  const GLvoid *pixels)
{
    warning("glTexImage3D not implemented");
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    const GLuint *texlist = textures;
    while (n-- > 0) {
        int i = *texlist++;
        if (i > 0 && i < _MAX_GL_TEX) {
            /* Textures (and palettes) queued draws may still read are freed
             * once the GPU is past them, instead of waiting for it */
            bool in_use = texture_list[i].in_use;
            void *data = GX_GetTexObjData(&texture_list[i].texobj);
            if (data != 0) {
                data = MEM_PHYSICAL_TO_K0(data);
                if (in_use) _ogx_free_after_gpu(data);
                else free(data);
            }
            if (texture_list[i].tlut)
                _ogx_tlut_free(texture_list[i].tlut, in_use);
            memset(&texture_list[i], 0, sizeof(texture_list[i]));
        }
    }
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    GLuint *texlist = textures;
    int i;
    for (i = 0; i < _MAX_GL_TEX && n > 0; i++) {
        if (!TEXTURE_IS_RESERVED(texture_list[i])) {
            TEXTURE_RESERVE(texture_list[i]);
            *texlist++ = i;
            n--;
        }
    }

    if (n > 0) {
        warning("Could not allocate %d textures", n);
        set_error(GL_OUT_OF_MEMORY);
    }
}

void glActiveTexture(GLenum texture)
{
    int index = texture - GL_TEXTURE0;
    if (index < 0 || index >= MAX_TEXTURE_UNITS) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    glparamstate.active_texture = index;
}

void glClientActiveTexture(GLenum texture)
{
    int index = texture - GL_TEXTURE0;
    if (index < 0 || index >= MAX_TEXTURE_UNITS) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    glparamstate.cs.active_texture = index;
}

/*****************************************************************************
Copyright (c) 2025  Alberto Mardegan (mardy@users.sourceforge.net)
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

#ifndef OPENGX_TEXTURE_H
#define OPENGX_TEXTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <GL/gl.h>
#include <ogc/gx.h>
#include <stdbool.h>
#include <stdint.h>

/* The GX API allow storing a void * for user data into the GXTexObj; but for
 * now we don't need a pointer, just some bits of information. Let's store them
 * in the space reserved for the pointer, then. */
typedef union {
    void *ptr;
    struct {
        unsigned is_reserved: 1;
        unsigned is_alpha: 1;
        /* The GL base format has no alpha (GL_RGB*, GL_LUMINANCE*, RGB
         * DXT1): the texture alpha must read as 1.0 (GX I8 returns I as
         * alpha, CMPR may have transparent texels) and the non-combine env
         * modes pass the fragment alpha through. Handled in the TEV setup. */
        unsigned is_opaque: 1;
    } d;
} OgxTextureUserData;

#define TEXTURE_USER_DATA(texobj) \
    ((OgxTextureUserData)GX_GetTexObjUserData(texobj))
#define TEXTURE_IS_USED(texture) \
    (GX_GetTexObjData(&texture.texobj) != NULL)
#define TEXTURE_IS_RESERVED(texture) \
    (TEXTURE_USER_DATA(&texture.texobj).d.is_reserved)
#define TEXTURE_RESERVE(texture) \
    { \
        GX_InitTexObj(&(texture).texobj, NULL, 0, 0, 0, \
                      GX_REPEAT, GX_REPEAT, 0); \
        OgxTextureUserData ud = { .ptr = NULL }; \
        ud.d.is_reserved = 1; \
        GX_InitTexObjUserData(&(texture).texobj, ud.ptr); \
    }

typedef struct {
    void *texels;
    uint16_t width, height;
    uint8_t format, wraps, wrapt, mipmap;
    uint8_t min_filter, mag_filter;
    uint8_t minlevel, maxlevel;
    OgxTextureUserData ud;
} OgxTextureInfo;

bool _ogx_texture_get_info(GLuint texture_name, OgxTextureInfo *info);
bool _ogx_texture_get_texobj(GLuint texture_name, GXTexObj *texobj);

/* Used by glTexImage2D-like functions: make sure the texel buffer of the
 * texture can hold the given level, then commit the info to the GX object. */
bool _ogx_texture_prepare_storage(GXTexObj *texobj, GLint level,
                                  int width, int height, uint8_t gx_format,
                                  OgxTextureInfo *ti);
void _ogx_texture_commit(GXTexObj *texobj, const OgxTextureInfo *ti);

struct gltexture_;
/* Make the texel buffer of a texture safe to write from the CPU without
 * waiting for the GPU: if queued draws may still read it, the texture gets a
 * new buffer (a copy of the old one if "keep_contents") and the old buffer is
 * freed once the GPU has passed the current point of the FIFO. */
void _ogx_texture_make_writable(struct gltexture_ *texture, bool keep_contents);

/* GL_TEXTURE_COMPRESSION_HINT: GL_NICEST stores decoded DXT3/DXT5 as RGBA8,
 * GL_FASTEST keeps them compressed as CMPR + an alpha plane */
extern GLenum _ogx_texture_compression_hint;

/* The LOD parameters every texture object gets (filters and LOD range come
 * from the GL state; bias, clamp, edge LOD and anisotropy are fixed). An
 * alpha plane is set up with the same call, so both planes of a texture
 * sample the same mipmap levels. */
static inline void _ogx_texture_init_lod(GXTexObj *obj, u8 min_filter,
                                         u8 mag_filter, f32 minlod, f32 maxlod)
{
    GX_InitTexObjLOD(obj, min_filter, mag_filter, minlod, maxlod, 0,
                     GX_ENABLE, GX_ENABLE, GX_ANISO_1);
}

/* Size of a texel buffer holding level 0 only ("chain" false) or a whole
 * mipmap chain of a w x h texture in the given format */
uint32_t _ogx_texture_buffer_size(int w, int h, uint32_t format, bool chain);
/* Free the alpha plane of a texture (after the GPU, if queued draws may
 * read it); the texture is a plain CMPR one afterwards */
void _ogx_texture_drop_alpha(struct gltexture_ *texture);
/* Turn a texture with an alpha plane into the decoded storage (RGB5A3), for
 * the draws that have no room for the second TEV stage; false if out of
 * memory (the texture is left as it was). */
bool _ogx_texture_split_demote(GLuint texture_name);
extern uint32_t _ogx_texture_demotions; /* statistics, for tests */

/* Free memory the GPU may still read, once it has passed the current FIFO
 * point */
void _ogx_free_after_gpu(void *ptr);
/* Hooks for the frame boundary (draw sync tokens restart from 0 there) */
void _ogx_texture_swap_marker_sent(void);
/* A point of the FIFO is (swap markers sent before it, draw sync token):
 * tokens restart from 0 at every swap. The current swap count, and whether
 * the GPU has passed a point. */
uint32_t _ogx_swaps_sent(void);
bool _ogx_fifo_point_passed(uint32_t swap, uint16_t token);
void _ogx_texture_swap_marker_received(void);
extern uint32_t _ogx_texture_orphans; /* statistics, for tests */
/* Byte offset of a mipmap level in the texel buffer, and size of a level,
 * correct for GX_TF_CI8 too. "w" and "h" are the size of level 0. */
uint32_t _ogx_texture_level_offset(int level, int w, int h, uint32_t format);
uint32_t _ogx_texture_level_size(int w, int h, uint32_t format);

#ifdef __cplusplus
} // extern C
#endif

#endif /* OPENGX_TEXTURE_H */

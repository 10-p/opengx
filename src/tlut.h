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



#ifndef OPENGX_TLUT_H
#define OPENGX_TLUT_H

#include <GL/gl.h>
#include <ogc/gx.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A texture palette (GL_EXT_paletted_texture), stored in main memory in the
 * GX TLUT format and loaded into one of the TMEM TLUT regions when a texture
 * using it is bound to a TEV stage. */
typedef struct OgxTlut {
    GXTlutObj tlutobj;
    uint32_t serial; /* unique per palette contents; changes on every update */
    uint16_t entries; /* as requested by the application */
    uint16_t gx_entries; /* allocated entries (multiple of 16) */
    uint8_t gx_format; /* GX_TL_* */
    GLenum internal_format;
    _Alignas(32) uint16_t data[0];
} OgxTlut;

/* Select a TMEM TLUT for the texture object (loading the palette there if
 * it's not already resident) and point the texture object at it. */
void _ogx_tlut_bind(GXTexObj *texobj, OgxTlut *tlut);
void _ogx_tlut_free(OgxTlut *tlut);

#ifdef __cplusplus
} // extern C
#endif

#endif /* OPENGX_TLUT_H */

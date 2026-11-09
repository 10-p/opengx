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



/* opengx-specific GL extensions (UT99 Wii port fork).
 *
 * GL_OGX_native_texture: upload texture data that is already in the GX
 * hardware layout (tiled, big-endian), without any conversion. The internal
 * formats below are accepted by glCompressedTexImage2D(); "compressed" just
 * means "opaque bytes" here. The exact byte layout of each format is
 * documented in CHANGES-ut99.md.
 *
 * The values live above 0xFFFF, a range Khronos never assigns, so that they
 * cannot collide with any registered enum.
 */

#ifndef OPENGX_GXEXT_H
#define OPENGX_GXEXT_H

#ifndef GL_OGX_native_texture
#define GL_OGX_native_texture 1

/* Internal formats for glCompressedTexImage2D() */
#define GL_TEXTURE_GX_I8_OGX            0x0F6E0001
#define GL_TEXTURE_GX_IA8_OGX           0x0F6E0003
#define GL_TEXTURE_GX_RGB565_OGX        0x0F6E0004
#define GL_TEXTURE_GX_RGB5A3_OGX        0x0F6E0005
#define GL_TEXTURE_GX_RGBA8_OGX         0x0F6E0006
#define GL_TEXTURE_GX_CI8_OGX           0x0F6E0009
#define GL_TEXTURE_GX_CMPR_OGX          0x0F6E000E

/* "type" values for glColorTableEXT(): the table is an array of big-endian
 * 16-bit TLUT entries in the given GX TLUT format, used as they are. */
#define GL_TLUT_GX_IA8_OGX              0x0F6E0100
#define GL_TLUT_GX_RGB565_OGX           0x0F6E0101
#define GL_TLUT_GX_RGB5A3_OGX           0x0F6E0102

#endif /* GL_OGX_native_texture */

#endif /* OPENGX_GXEXT_H */

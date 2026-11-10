/*****************************************************************************
Copyright (c) 2024  Alberto Mardegan (mardy@users.sourceforge.net)
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

#include "texture_unit.h"

#include "debug.h"
#include "gpu_resources.h"
#include "texture.h"
#include "texture_gen_sw.h"
#include "tlut.h"
#include "utils.h"

static void setup_texture_gen(const OgxTextureUnit *tu, u8 tex_coord,
                              u8 texture_matrix, u8 matrix_input)
{
    Mtx m;

    /* The GX API does not allow setting different inputs and generation modes
     * for the S and T coordinates; so, if one of them is enabled, we assume
     * that both share the same generation mode. */
    u32 input_type = matrix_input;
    u32 matrix_src = GX_IDENTITY;
    switch (tu->gen_mode) {
    case GL_OBJECT_LINEAR:
    case GL_EYE_LINEAR:
        /* (s, t, r) from the planes, r only with GL_TEXTURE_GEN_R (else 0,
         * the texture coordinate's default), then the texture matrix as the
         * post-transform, whose third row keeps the projection divisor at 1:
         * the matrix's r column then applies to r, as in OpenGL (a 2x4
         * generation would hand the texture matrix a constant 1 there). */
        input_type = GX_TG_POS;
        matrix_src = GX_TEXMTX0 + ogx_gpu_resources->texmtx_first++ * 3;
        bool eye = tu->gen_mode == GL_EYE_LINEAR;
        Mtx plane;
        set_gx_mtx_rowv(0, plane, eye ? tu->texture_eye_plane_s :
                        tu->texture_object_plane_s);
        set_gx_mtx_rowv(1, plane, eye ? tu->texture_eye_plane_t :
                        tu->texture_object_plane_t);
        if (tu->gen_enabled & OGX_TEXGEN_R)
            set_gx_mtx_rowv(2, plane, eye ? tu->texture_eye_plane_r :
                            tu->texture_object_plane_r);
        else
            set_gx_mtx_row(2, plane, 0.0f, 0.0f, 0.0f, 0.0f);
        if (eye)
            guMtxConcat(plane, glparamstate.modelview_matrix, m);
        else
            memcpy(m, plane, sizeof(Mtx));
        GX_LoadTexMtxImm(m, matrix_src, GX_MTX3x4);

        Mtx post;
        memcpy(post, tu->matrix[tu->matrix_index], 8 * sizeof(float));
        post[2][0] = post[2][1] = post[2][2] = 0.0f;
        post[2][3] = 1.0f;
        DCStoreRange(post, sizeof(post));
        GX_LoadTexMtxImm(post, texture_matrix, GX_MTX3x4);
        GX_SetTexCoordGen2(tex_coord, GX_TG_MTX3x4, input_type, matrix_src,
                           FALSE, texture_matrix);
        return;
    case GL_REFLECTION_MAP:
    case GL_SPHERE_MAP:
        input_type = GX_TG_NRM;
        matrix_src = GX_TEXMTX0 + ogx_gpu_resources->texmtx_first++ * 3;
        Mtx scale, translate, m;
        guMtxScale(scale, 0.5f, 0.5f, 0.0f);
        guMtxTrans(translate, 0.5f, 0.5f, 1.0f);
        guMtxConcat(scale, glparamstate.modelview_matrix, m);
        guMtxConcat(translate, m, m);
        GX_LoadTexMtxImm(m, matrix_src, GX_MTX2x4);
        break;
    default:
        warning("Unsupported texture coordinate generation mode %x",
                tu->gen_mode);
    }

    GX_SetTexCoordGen2(tex_coord, GX_TG_MTX2x4, input_type, matrix_src,
                       FALSE, texture_matrix);
}

typedef struct {
    u8 source;
    bool must_complement; /* true if we should use "(1 - source)" instead of
                             "source" */
} TevSource;

/* tex_alpha / tex_alpha_a: where the texture alpha is read from (GX_CC_TEXA /
 * GX_CA_TEXA, or the register the alpha plane stage wrote) */
static TevSource gl_rgbsource_to_gx(GLenum source, GLenum operand,
                                    u8 prev_rgb, u8 prev_alpha,
                                    u8 raster_rgb, u8 raster_alpha,
                                    bool tex_opaque, bool tex_alpha_only,
                                    u8 tex_alpha)
{
    TevSource ret = { GX_CC_ZERO, false };
    if (operand == GL_ONE_MINUS_SRC_COLOR) {
        operand = GL_SRC_COLOR;
        ret.must_complement = true;
    } else if (operand == GL_ONE_MINUS_SRC_ALPHA) {
        operand = GL_SRC_ALPHA;
        ret.must_complement = true;
    }

    switch (source) {
    case GL_TEXTURE:
        switch (operand) {
        /* GL_ALPHA textures have colour (0, 0, 0), alpha-less ones alpha 1 */
        case GL_SRC_COLOR:
            ret.source = tex_alpha_only ? GX_CC_ZERO : GX_CC_TEXC;
            break;
        case GL_SRC_ALPHA:
            ret.source = tex_opaque ? GX_CC_ONE : tex_alpha;
            break;
        }
        break;
    case GL_PREVIOUS:
        switch (operand) {
        case GL_SRC_COLOR: ret.source = prev_rgb; break;
        case GL_SRC_ALPHA: ret.source = prev_alpha; break;
        }
        break;
    case GL_CONSTANT: ret.source = GX_CC_KONST; break;
    case GL_PRIMARY_COLOR:
        switch (operand) {
        case GL_SRC_COLOR: ret.source = raster_rgb; break;
        case GL_SRC_ALPHA: ret.source = raster_alpha; break;
        }
        break;
    }
    return ret;
}

/* Sentinel for "constant 1" in the alpha channel; compute_tev_input()
 * turns it into the stage konst alpha set to 1 */
#define CA_ONE 0xa1

static TevSource gl_alphasource_to_gx(GLenum source, GLenum operand,
                                      u8 prev_alpha, u8 raster_alpha,
                                      bool tex_opaque, u8 tex_alpha_a)
{
    TevSource ret = { GX_CA_ZERO, false };
    /* For the alpha channel, operand can only be either GL_SRC_ALPHA or
     * GL_ONE_MINUS_SRC_ALPHA. */
    if (operand == GL_ONE_MINUS_SRC_ALPHA) {
        ret.must_complement = true;
    }

    switch (source) {
    case GL_TEXTURE: ret.source = tex_opaque ? CA_ONE : tex_alpha_a; break;
    case GL_PREVIOUS: ret.source = prev_alpha; break;
    case GL_CONSTANT: ret.source = GX_CA_KONST; break;
    case GL_PRIMARY_COLOR: ret.source = raster_alpha; break;
    }
    return ret;
}

typedef struct {
    u8 reg[4]; /* a, b, c, d */
    u8 bias;
    u8 tevop;
    bool must_complement_constant;
} TevInput;

static TevInput compute_tev_input(GLenum combine_func, u8 stage, GXColor color,
                                  const TevSource *args,
                                  bool is_alpha)
{
    TevInput ret;
    u8 *reg = ret.reg;
    enum {
        A = 0,
        B = 1,
        C = 2,
        D = 3,
        NUM_TEV_REGS,
    };
    int used_args = 0;

    u8 zero_value, one_value, konst_value;
    if (is_alpha) {
        zero_value = GX_CA_ZERO;
        /* The TEV does not provide an equivalent of GX_CC_ONE for the alpha
         * channel; we can workaround this by using GX_CA_KONST and calling
         * GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_1), but we need to be very
         * careful, because this if one of the arg{0,1,2} is set to a constant,
         * we'll need to solve the conflict somehow (we can use only one
         * constant value per TEV stage). */
        one_value = CA_ONE;
        konst_value = GX_CA_KONST;
    } else {
        zero_value = GX_CC_ZERO;
        one_value = GX_CC_ONE;
        konst_value = GX_CC_KONST;
    }

    ret.must_complement_constant = false;
    ret.bias = GX_TB_ZERO;
    ret.tevop = GX_TEV_ADD;

    /* Promemoria: the TEV operation is
     *     (d OP (a * (1 - c) + b * c + bias)) * scale
     */
    switch (combine_func) {
    case GL_REPLACE:
        used_args = 1;
        /* result = arg0
         * In order to support complementing the value (that is, "1 - arg0"),
         * we store arg0 into the C register, and set A and B to 0 and 1 (or
         * viceversa, if complementing). */
        if (is_alpha && args[0].source == GX_CA_KONST) {
            reg[A] = args[0].source;
            reg[B] = zero_value;
            reg[C] = zero_value;
            reg[D] = zero_value;
            ret.must_complement_constant = args[0].must_complement;
            break;
        }
        if (args[0].must_complement) {
            /* Instead of C, we should use "1 - C"; we can achieve this by
             * swapping A and B: */
            reg[A] = one_value;
            reg[B] = zero_value;
        } else {
            reg[A] = zero_value;
            reg[B] = one_value;
        }
        reg[C] = args[0].source;
        reg[D] = zero_value;
        break;
    case GL_MODULATE:
        used_args = 2;
        /* result = arg0 * arg1 */
        if (args[0].must_complement || args[1].must_complement) {
            if (args[0].must_complement) {
                reg[C] = args[0].source;
                reg[A] = args[1].source;
                reg[B] = zero_value;
                if (args[1].must_complement) {
                    /* Note: we cannot support the case when both arguments are
                     * complemented, unless we add more stages. */
                    warning("Cannot complement both modulate args");
                }
            } else { /* only arg1 is to be complemented */
                reg[C] = args[1].source;
                reg[A] = args[0].source;
                reg[B] = zero_value;
            }
        } else {
            reg[A] = zero_value;
            reg[B] = args[0].source;
            reg[C] = args[1].source;
        }
        reg[D] = zero_value;
        break;
    case GL_ADD_SIGNED:
        /* result = arg0 + arg1 - 0.5 */
        ret.bias = GX_TB_SUBHALF;
        /* fall through */
    case GL_ADD:
        used_args = 2;
        /* result = arg0 + arg1 */
        if (args[0].must_complement || args[1].must_complement) {
            if (args[0].must_complement) {
                reg[C] = args[0].source;
                reg[A] = one_value;
                reg[D] = args[1].source;
                if (args[1].must_complement) {
                    /* Note: we cannot support the case when both arguments are
                     * complemented, unless we add more stages. */
                    warning("Cannot complement both args in addition");
                }
            } else { /* only arg1 is to be complemented */
                reg[C] = args[1].source;
                reg[A] = one_value;
                reg[D] = args[0].source;
            }
        } else {
            reg[A] = args[0].source;
            reg[C] = zero_value;
            reg[D] = args[1].source;
        }
        reg[B] = zero_value;
        break;
    case GL_SUBTRACT:
        used_args = 2;
        /* result = arg0 - arg1 */
        ret.tevop = GX_TEV_SUB;
        if (args[0].must_complement) {
            /* We store arg0 into the D register, and there's no way to
             * complement that */
            warning("Cannot complement first arg in subtraction");
        }
        if (args[1].must_complement) {
            reg[C] = args[1].source;
            reg[A] = one_value;
        } else {
            reg[A] = args[1].source;
            reg[C] = zero_value;
        }
        reg[B] = zero_value;
        reg[D] = args[0].source;
        break;
    case GL_MODULATE_SIGNED_ADD_ATI:
        /* result = arg0 * arg2 + arg1 - 0.5 */
        ret.bias = GX_TB_SUBHALF;
        /* fall through */
    case GL_MODULATE_ADD_ATI:
        used_args = 3;
        /* result = arg0 * arg2 + arg1: arg1 goes into D, which cannot be
         * complemented; one of arg0 and arg2 can (by exchanging the roles of
         * the interpolated pair: a * (1 - c) is "(1 - c) * a") */
        if (args[1].must_complement)
            warning("Cannot complement arg1 of a modulate-add");
        if (args[0].must_complement && args[2].must_complement)
            warning("Cannot complement both factors of a modulate-add");
        if (args[2].must_complement) {
            reg[A] = args[0].source; /* arg0 * (1 - arg2) */
            reg[B] = zero_value;
            reg[C] = args[2].source;
        } else if (args[0].must_complement) {
            reg[A] = args[2].source; /* arg2 * (1 - arg0) */
            reg[B] = zero_value;
            reg[C] = args[0].source;
        } else {
            reg[A] = zero_value;
            reg[B] = args[0].source;
            reg[C] = args[2].source;
        }
        reg[D] = args[1].source;
        break;
    case GL_MODULATE_SUBTRACT_ATI:
        used_args = 3;
        /* result = arg0 * arg2 - arg1: the TEV can only subtract from D
         * (arg1 - arg0 * arg2), so this is not supported */
        warning("GL_MODULATE_SUBTRACT_ATI is not supported; drawn as arg0 * arg2");
        reg[A] = zero_value;
        reg[B] = args[0].source;
        reg[C] = args[2].source;
        reg[D] = zero_value;
        break;
    case GL_INTERPOLATE:
        used_args = 3;
        /* result = arg0 * arg2 + arg1 * (1 - arg2) */
        if (args[2].must_complement) {
            /* Instead of C, we should use "1 - C"; we can achieve this by
             * swapping A and B: */
            reg[A] = args[0].source;
            reg[B] = args[1].source;
        } else {
            reg[A] = args[1].source;
            reg[B] = args[0].source;
        }
        if (args[0].must_complement || args[1].must_complement) {
            warning("Cannot complement interpolation arguments 0 and 1");
        }
        reg[C] = args[2].source;
        reg[D] = zero_value;
        break;
    }

    int used_constants = 0;
    bool needs_constant_one = false;
    if (is_alpha) {
        for (int i = 0; i < NUM_TEV_REGS; i++) {
            if (reg[i] == one_value) {
                needs_constant_one = true;
                reg[i] = GX_CA_KONST;
            }
        }
        if (needs_constant_one) {
            /* Set the stage constant to 1, since the TEV does not provide a
             * such a constant for the alpha channel */
            GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_1);
            used_constants++;
        }
    }
    for (int i = 0; i < used_args; i++) {
        if (args[i].source == konst_value) used_constants++;
    }
    if (used_constants > 0) {
        if (used_constants > 1) {
            warning("TEV only supports one constant per stage!");
            /* We could support this by using more stages. TODO */
        }
        /* TODO: dynamically allocate constant register! */
        if (is_alpha && !needs_constant_one) {
            GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
        } else {
            GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K0);
        }
        GX_SetTevKColor(GX_KCOLOR0, color);
    }

    for (int i = 0; i < 4; i++) {
        ret.reg[i] = reg[i];
    }
    return ret;
}

static void setup_combine_operation(const OgxTextureUnit *te, u8 stage,
                                    u8 prev_rgb, u8 prev_alpha,
                                    u8 raster_rgb, u8 raster_alpha,
                                    bool tex_opaque, bool tex_alpha_only,
                                    u8 tex_alpha_c, u8 tex_alpha_a)
{
    TevSource source_rgb[3];
    TevSource source_alpha[3];

    for (int i = 0; i < 3; i++) {
        source_rgb[i] = gl_rgbsource_to_gx(te->source_rgb[i],
                                           te->operand_rgb[i],
                                           prev_rgb, prev_alpha,
                                           raster_rgb, raster_alpha,
                                           tex_opaque, tex_alpha_only,
                                           tex_alpha_c);
        source_alpha[i] = gl_alphasource_to_gx(te->source_alpha[i],
                                               te->operand_alpha[i],
                                               prev_alpha, raster_alpha,
                                               tex_opaque, tex_alpha_a);
    }

    TevInput rgb = compute_tev_input(te->combine_rgb, stage, te->color,
                                     source_rgb, false);
    GX_SetTevColorIn(stage, rgb.reg[0], rgb.reg[1], rgb.reg[2], rgb.reg[3]);
    /* GL_RGB_SCALE maps directly to the TEV stage output scale; the TEV
     * scales before clamping, which is the same order as OpenGL. */
    GX_SetTevColorOp(stage, rgb.tevop, rgb.bias, te->rgb_scale, GX_TRUE,
                     GX_TEVPREV);

    TevInput alpha = compute_tev_input(te->combine_alpha, stage, te->color,
                                       source_alpha, true);
    GX_SetTevAlphaIn(stage, alpha.reg[0], alpha.reg[1],
                     alpha.reg[2], alpha.reg[3]);
    GX_SetTevAlphaOp(stage, alpha.tevop, alpha.bias, te->alpha_scale,
                     GX_TRUE, GX_TEVPREV);
}

/* alpha_reg: for a texture with an alpha plane, the TEV register (0..2)
 * the previous stage wrote its alpha to; 0xff otherwise */
static void setup_texture_stage(const OgxTextureUnit *tu,
                                u8 stage, u8 tex_coord, u8 tex_map,
                                u8 prev_rgb, u8 prev_alpha,
                                u8 raster_rgb, u8 raster_alpha,
                                u8 channel, u8 alpha_reg)
{
    u8 tex_alpha_c = GX_CC_TEXA, tex_alpha_a = GX_CA_TEXA;
    if (alpha_reg != 0xff) {
        /* Multiply by two because there are alpha registers in between */
        tex_alpha_c = GX_CC_A0 + alpha_reg * 2;
        tex_alpha_a = GX_CA_A0 + alpha_reg;
    }
    gltexture_ *texture = &texture_list[tu->glcurtex];
    OgxTextureUserData ud = TEXTURE_USER_DATA(&texture->texobj);
    /* Base formats without alpha (GL_RGB, GL_LUMINANCE) or without colour
     * (GL_ALPHA): the GX texture still returns something in the missing
     * channels (I8 returns I as alpha), so make sure it's ignored */
    bool tex_opaque = ud.d.is_opaque;
    bool tex_alpha_only = ud.d.is_alpha;

    switch (tu->mode) {
    case GL_REPLACE:
        // In data: a: Texture Color
        GX_SetTevColorIn(stage, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GX_SetTevAlphaIn(stage, tex_alpha_a, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        break;
    case GL_ADD:
        /* In data: d: Texture Color a: raster value, Operation: a+d
         * Alpha gets multiplied. */
        GX_SetTevColorIn(stage, prev_rgb, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, prev_alpha, tex_alpha_a, GX_CA_ZERO);
        break;
    case GL_BLEND:
        /* In data: c: Texture Color, a: raster value, b: tex env
         * Operation: a(1-c)+b*c
         * Until we implement GL_TEXTURE_ENV_COLOR, use white (GX_CC_ONE) for
         * the tex env color. */
        GX_SetTevColorIn(stage, prev_rgb, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, prev_alpha, tex_alpha_a, GX_CA_ZERO);
        break;
    case GL_COMBINE:
        setup_combine_operation(tu, stage, prev_rgb, prev_alpha,
                                raster_rgb, raster_alpha, tex_opaque,
                                tex_alpha_only, tex_alpha_c, tex_alpha_a);
        break;
    case GL_MODULATE:
    default:
        // In data: c: Texture Color b: raster value, Operation: b*c
        GX_SetTevColorIn(stage, GX_CC_ZERO, prev_rgb, GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, prev_alpha, tex_alpha_a, GX_CA_ZERO);
        break;
    }
    if (tu->mode != GL_COMBINE) {
        /* In all the fixed env modes (GL_REPLACE, GL_MODULATE, GL_ADD,
         * GL_BLEND) the missing channels of the texture leave the fragment's
         * value unchanged */
        if (tex_opaque)
            GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                             prev_alpha);
        if (tex_alpha_only)
            GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO,
                             prev_rgb);
        /* setup_combine_operation() already sets the TEV ops */
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                         GX_TEVPREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                         GX_TEVPREV);
    }
    GX_SetTevOrder(stage, tex_coord, tex_map, channel);
    bool points_enabled = glparamstate.point_sprites_enabled &&
        glparamstate.point_sprites_coord_replace;
    GX_EnableTexOffsets(tex_coord, GX_DISABLE, points_enabled);
    if (GX_GetTexObjFmt(&texture->texobj) == GX_TF_CI8) {
        /* Colour-index textures cannot interpolate between mipmap levels
         * (GX hardware limitation): fall back to the nearest level. */
        u8 min_filter, mag_filter;
        GX_GetTexObjFilterMode(&texture->texobj, &min_filter, &mag_filter);
        if (min_filter == GX_LIN_MIP_LIN || min_filter == GX_NEAR_MIP_LIN) {
            min_filter = min_filter == GX_LIN_MIP_LIN ?
                GX_LIN_MIP_NEAR : GX_NEAR_MIP_NEAR;
            GX_InitTexObjFilterMode(&texture->texobj, min_filter, mag_filter);
        }
        if (texture->tlut) {
            _ogx_tlut_bind(&texture->texobj, texture->tlut);
        } else {
            warning("Paletted texture %d has no palette", tu->glcurtex);
        }
    }
    texture->in_use = 1;
    GX_LoadTexObj(&texture->texobj, tex_map);
}

static void setup_texture_stage_matrix(const OgxTextureUnit *tu,
                                       u8 dtt_matrix)
{
    Mtx m;
    /* Post-transform matrices are always 4x3, but we don't want any
     * transformation on the third coordinate, hence use an identity-like third
     * row. */
    memcpy(m, tu->matrix[tu->matrix_index], 8 * sizeof(float));
    m[2][0] = m[2][1] = m[2][3] = 0.0f;
    m[2][2] = 1.0f;
    DCStoreRange(m, sizeof(m));
    GX_LoadTexMtxImm(m, dtt_matrix, GX_MTX3x4);
}

/* The stage before a texture with an alpha plane: it samples the plane
 * (GX_TF_I4/I8: alpha = I) with the texture's coordinates and stores it in
 * TEV register "alpha_reg"; PREV is untouched. The plane's texture object
 * mirrors the colour's size, wrap modes, filters and LOD range, so both
 * planes sample the same texels of the same levels (the size is the same,
 * so the shared texture coordinate gets the same scale). */
static void setup_alpha_plane_stage(gltexture_ *texture, u8 stage,
                                    u8 tex_coord, u8 tex_map, u8 alpha_reg)
{
    const GXTexObj *color = &texture->texobj;
    GXTexObj plane;
    u8 min_filter, mag_filter;
    f32 minlod, maxlod;
    GX_InitTexObj(&plane, texture->alpha_texels,
                  GX_GetTexObjWidth(color), GX_GetTexObjHeight(color),
                  OGX_ALPHA_PLANE_FORMAT(texture),
                  GX_GetTexObjWrapS(color), GX_GetTexObjWrapT(color),
                  GX_GetTexObjMipMap(color));
    GX_GetTexObjFilterMode(color, &min_filter, &mag_filter);
    GX_GetTexObjLOD(color, &minlod, &maxlod);
    _ogx_texture_init_lod(&plane, min_filter, mag_filter, minlod, maxlod);
    GX_LoadTexObj(&plane, tex_map);

    u8 reg = GX_TEVREG0 + alpha_reg;
    GX_SetTevOrder(stage, tex_coord, tex_map, GX_COLORNULL);
    GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                     reg);
    GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                     reg);
}

/* Textures with an alpha plane cost a TEV stage and a texture map more each,
 * and one TEV register for all of them; the stencil and clip plane stages,
 * set up after the texture stages, need theirs too. If that does not fit,
 * those textures go back to the decoded storage (never a wrong picture).
 * Returns the register for the alpha planes, or 0xff if none is used. */
static u8 alpha_planes_prepare(void)
{
    int units = 0, planes = 0;
    for (int tex = 0; tex < MAX_TEXTURE_UNITS; tex++) {
        if (!(glparamstate.texture_enabled & (1 << tex))) continue;
        OgxTextureUnit *tu = &glparamstate.texture_unit[tex];
        if (!tu->array_reader && !tu->gen_enabled) continue;
        units++;
        if (texture_list[tu->glcurtex].alpha_plane & OGX_ALPHA_PLANE)
            planes++;
    }
    if (planes == 0) return 0xff;

    int stages = units + planes, maps = units + planes;
    if (glparamstate.stencil.enabled) {
        stages++;
        maps++;
    }
    if (glparamstate.clip_plane_mask != 0) {
        stages += (__builtin_popcount(glparamstate.clip_plane_mask) + 1) / 2;
        maps++;
    }
    const OgxGpuResources *r = ogx_gpu_resources;
    if (r->tevstage_first + stages <= r->tevstage_end &&
        r->texmap_first + maps <= r->texmap_end &&
        r->tevreg_first < r->tevreg_end) {
        return ogx_gpu_resources->tevreg_first++;
    }

    warning("No room for %d alpha plane stages: textures decoded", planes);
    for (int tex = 0; tex < MAX_TEXTURE_UNITS; tex++) {
        if (!(glparamstate.texture_enabled & (1 << tex))) continue;
        GLuint name = glparamstate.texture_unit[tex].glcurtex;
        if (!_ogx_texture_split_demote(name)) {
            /* Out of memory for the decoded copy: the texture is drawn
             * without its alpha plane */
            warning("Texture %u: no memory to decode it", name);
        }
    }
    return 0xff;
}

void _ogx_setup_texture_stages(u8 raster_reg_index, u8 channel)
{
    u8 raster_rgb, raster_alpha;
    if (channel != GX_COLORNULL) {
        raster_rgb = GX_CC_RASC;
        raster_alpha = GX_CA_RASA;
    } else {
        raster_rgb = GX_CC_C0 + raster_reg_index * 2;
        raster_alpha = GX_CA_A0 + raster_reg_index;
    }

    u8 prev_rgb = raster_rgb;
    u8 prev_alpha = raster_alpha;
    u8 alpha_reg = alpha_planes_prepare();

    for (int tex = 0; tex < MAX_TEXTURE_UNITS; tex++) {
        if (!(glparamstate.texture_enabled & (1 << tex))) continue;

        OgxTextureUnit *tu = &glparamstate.texture_unit[tex];

        u8 input_coordinates = 0xff;
        if (tu->array_reader) {
            input_coordinates = _ogx_array_reader_get_tex_coord_source(
                                        tu->array_reader);
        } else if (!tu->gen_enabled) {
            warning("Skipping texture unit, since coordinates are missing.");
            continue;
        }

        u8 tex_coord = GX_TEXCOORD0 + ogx_gpu_resources->texcoord_first++;
        u8 dtt_matrix = GX_DTTMTX0 + ogx_gpu_resources->dttmtx_first++ * 3;
        gltexture_ *texture = &texture_list[tu->glcurtex];
        u8 unit_alpha_reg = 0xff;
        if (alpha_reg != 0xff && (texture->alpha_plane & OGX_ALPHA_PLANE)) {
            u8 plane_stage = GX_TEVSTAGE0 +
                ogx_gpu_resources->tevstage_first++;
            u8 plane_map = GX_TEXMAP0 + ogx_gpu_resources->texmap_first++;
            setup_alpha_plane_stage(texture, plane_stage, tex_coord,
                                    plane_map, alpha_reg);
            unit_alpha_reg = alpha_reg;
        }
        u8 stage = GX_TEVSTAGE0 + ogx_gpu_resources->tevstage_first++;
        u8 tex_map = GX_TEXMAP0 + ogx_gpu_resources->texmap_first++;

        setup_texture_stage(tu, stage, tex_coord, tex_map,
                            prev_rgb, prev_alpha,
                            raster_rgb, raster_alpha, channel,
                            unit_alpha_reg);

        if (input_coordinates == GX_TG_POS || input_coordinates == GX_TG_NRM) {
            u8 matrix_src = GX_TEXMTX0 + ogx_gpu_resources->texmtx_first++ * 3;
            GX_LoadTexMtxImm(tu->matrix[tu->matrix_index], matrix_src, GX_MTX2x4);
            GX_SetTexCoordGen(tex_coord, GX_TG_MTX2x4,
                              input_coordinates, matrix_src);
        } else {
            setup_texture_stage_matrix(tu, dtt_matrix);
            /* Use GPU texture coordinate generation only if the coordinates
             * haven't already been generated in software. */
            if (tu->gen_enabled && !tu->array_reader) {
                setup_texture_gen(tu, tex_coord, dtt_matrix, input_coordinates);
            } else {
                GX_SetTexCoordGen2(tex_coord, GX_TG_MTX2x4, input_coordinates,
                                   GX_IDENTITY, FALSE, dtt_matrix);
            }
        }

        /* All texture stages after the first one get their vertex color from
         * the previous stage */
        prev_rgb = GX_CC_CPREV;
        prev_alpha = GX_CA_APREV;
    }
}

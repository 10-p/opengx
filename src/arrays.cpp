/*****************************************************************************
Copyright (c) 2011  David Guillen Fandos (david@davidgf.net)
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

#include "arrays.h"

#include "debug.h"
#include "state.h"
#include "utils.h"
#include "texture.h"
#include "vbo.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ogc/gx.h>
#include <new>
#include <malloc.h>
#include <type_traits>
#include <ogc/machine/processor.h>

#define MAX_TEXCOORDS 8 /* GX_VA_TEX7 - GX_VA_TEX8 */

OgxArrayReader s_readers[1 /* positions (always)*/
                         + 1 /* normals (if s_has_normals) */
                         + MAX_COLOR_ARRAYS /* as s_num_colors */
                         + MAX_TEXCOORD_ARRAYS] /* as s_num_tex_arrays */;

/* The difference between s_num_tex_arrays and s_num_tex_coords is that the
 * former is used to count the number of active OgxArrayReader elements; but
 * not all of them consume a tex coordinate, because TexCoordProxyVertexReader
 * elements do not emit any new coordinates. So, s_num_tex_coords counts the
 * number of arrays that produce texture coordinates. */
static char s_num_tex_arrays = 0;
static char s_num_tex_coords = 0;

static bool s_has_normals = false;
static uint8_t s_num_colors = 0;
static OgxDrawFlags s_draw_flags = OGX_DRAW_FLAG_NONE;

static inline int count_attributes() {
    return 1 /* pos */ + s_has_normals + s_num_colors + s_num_tex_arrays;
}

struct GxVertexFormat {
    uint8_t attribute;
    char num_components;
    uint8_t type;
    uint8_t size;

    int stride() const {
        int component_size;
        if (attribute == GX_VA_CLR0 || attribute == GX_VA_CLR1) {
            component_size = 1;
        } else {
            switch (size) {
            case GX_S8:
            case GX_U8:
                component_size = 1; break;
            case GX_S16:
            case GX_U16:
                component_size = 2; break;
            case GX_F32:
                component_size = 4; break;
            }
        }
        return component_size * num_components;
    }
};

static int compute_array_stride(const OgxVertexAttribArray *array)
{
    return array->stride > 0 ? array->stride :
        array->size * sizeof_gl_type(array->type);
}

struct TemplateSelectionInfo {
    GxVertexFormat format;
    bool same_type;
};

static uint8_t gl_type_to_gx_size(GLenum type)
{
    switch (type) {
    case GL_SHORT: return GX_S16;
    case GL_FLOAT: return GX_F32;
    }
    return 0xff;
}

static TemplateSelectionInfo select_template(GLenum type,
                                             uint8_t vertex_attribute,
                                             char num_components)
{
    TemplateSelectionInfo info = {
        {vertex_attribute, num_components, 0xff, 0xff},
    };
    switch (vertex_attribute) {
    case GX_VA_POS:
        info.format.type = num_components == 2 ? GX_POS_XY : GX_POS_XYZ;
        info.format.size = gl_type_to_gx_size(type);
        info.same_type = num_components <= 3;
        break;
    case GX_VA_NRM:
        info.format.type = GX_NRM_XYZ;
        info.format.size = gl_type_to_gx_size(type);
        info.same_type = num_components == 3;
        break;
    case GX_VA_TEX0:
    case GX_VA_TEX1:
    case GX_VA_TEX2:
    case GX_VA_TEX3:
    case GX_VA_TEX4:
    case GX_VA_TEX5:
    case GX_VA_TEX6:
    case GX_VA_TEX7:
        info.format.type = num_components == 1 ? GX_TEX_S : GX_TEX_ST;
        info.format.size = gl_type_to_gx_size(type);
        info.same_type = num_components <= 2;
        /* The hardware does not support sending more than 2 texture
         * coordinates */
        if (num_components > 2) info.format.num_components = 2;
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (num_components == 3) {
            info.format.type = GX_CLR_RGB;
            info.format.size = GX_RGB8;
        } else {
            info.format.type = GX_CLR_RGBA;
            info.format.size = GX_RGBA8;
        }
        info.same_type = type == GL_UNSIGNED_BYTE && num_components >= 3;
        break;
    }

    if (info.format.size == 0xff) {
        info.format.size = GX_F32;
        info.same_type = false;
    }
    return info;
}

/* This function assumes that num_components is at least 2 */
template <typename T>
static void to_pos3f(const T *components, int num_components, Pos3f pos) {
    pos[0] = *components++;
    pos[1] = *components++;
    if (num_components >= 3) {
        pos[2] = *components++;
        if (num_components == 4) {
            float w = *components++;
            pos[0] /= w;
            pos[1] /= w;
            pos[2] /= w;
        }
    } else {
        pos[2] = 0.0f;
    }
}

template <typename T>
static void to_norm3f(const T *components, Norm3f norm) {
    norm[0] = *components++;
    norm[1] = *components++;
    norm[2] = *components++;
}

/* This function assumes that num_components is at least 3 */
template <typename T>
static void to_color(const T *components, int num_components, GXColor *color) {
    auto read_color_component = [](const T value) {
        if constexpr (std::numeric_limits<T>::is_integer) {
            return value * 255 / std::numeric_limits<T>::max();
        } else {  // floating-point type
            return value * 255.0f;
        }
    };

    color->r = read_color_component(*components++);
    color->g = read_color_component(*components++);
    color->b = read_color_component(*components++);
    color->a = num_components == 4 ?
        read_color_component(*components++) : 255;
}

template <typename T>
static void to_tex2f(const T *components, int num_components, Tex2f tex) {
    tex[0] = *components++;
    if (num_components >= 2) {
        tex[1] = *components++;
    } else {
        tex[1] = 0.0f;
    }
}

/* How the GX vertex loader can fetch an attribute through an indexed array
 * (GX_SetArray + GX_INDEX8/16) instead of receiving its value in the FIFO. */
enum GxSourceKind : uint8_t {
    GXS_DIRECT = 0, /* only as direct data (constants, generators, conversions) */
    GXS_NONE,       /* the attribute sends no data (texture coordinates that
                       texgen takes from the position or normal) */
    GXS_GPU,        /* buffer object memory, already flushed: used in place */
    GXS_RAW,        /* client memory in a GX format: copied for the draw */
    GXS_COLOR_FLOAT,/* float colour: converted to RGB8/RGBA8 for the draw */
};

struct GxSource {
    const char *data;
    uint16_t stride;
    uint8_t elem_bytes; /* bytes of one element read from data */
    uint8_t num_components;
};

struct AbstractVertexReader {
    AbstractVertexReader(GxVertexFormat format): format(format) {}

    virtual GxSourceKind gx_source(GxSource *src) const { return GXS_DIRECT; }
    const GxVertexFormat &gx_format() const { return format; }

    virtual void setup_draw() {
        GX_SetVtxDesc(format.attribute, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, format.attribute,
                         format.type, format.size, 0);
    }

    virtual void draw_done() {};

    virtual void get_format(uint8_t *attribute, uint8_t *inputmode,
                            uint8_t *type, uint8_t *size) const {
        *attribute = format.attribute;
        *inputmode = GX_DIRECT;
        *type = format.type;
        *size = format.size;
    }

    virtual uint8_t get_tex_coord_source() const {
        switch (format.attribute) {
        case GX_VA_POS:
            return GX_TG_POS;
        case GX_VA_NRM:
            return GX_TG_NRM;
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            return GX_TG_COLOR0 + (format.attribute - GX_VA_CLR0);
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            return GX_TG_TEX0 + (format.attribute - GX_VA_TEX0);
        default:
            return 0xff;
        }
    }

    virtual void process_element(int index) = 0;

    virtual void read_color(int index, GXColor *color) const = 0;
    virtual void read_pos3f(int index, Pos3f pos) const = 0;
    virtual void read_norm3f(int index, Norm3f norm) const = 0;
    virtual void read_tex2f(int index, Tex2f tex) const = 0;

protected:
    GxVertexFormat format;
};

template <typename T>
struct ConstantVertexReader: public AbstractVertexReader {
    ConstantVertexReader(GxVertexFormat format, const T *values):
        AbstractVertexReader(format) {
        for (int i = 0; i < format.num_components; i++) {
            this->values[i] = values[i];
        }
    }

    void process_element(int /*index*/) override {
        const T *ptr = values;
        for (int i = 0; i < format.num_components; i++, ptr++) {
            if constexpr (sizeof(T) == 1) {
                wgPipe->U8 = *ptr;
            } else if constexpr (sizeof(T) == 2) {
                wgPipe->U16 = *ptr;
            } else if constexpr (sizeof(T) == 4) {
                wgPipe->F32 = *ptr;
            }
        }
    }

    void read_pos3f(int index, Pos3f pos) const override {
        to_pos3f(values, format.num_components, pos);
    }
    void read_norm3f(int index, Norm3f norm) const override {
        to_norm3f(values, norm);
    }
    void read_color(int index, GXColor *color) const override {
        to_color(values, format.num_components, color);
    }
    void read_tex2f(int index, Tex2f tex) const override {
        to_tex2f(values, format.num_components, tex);
    }

protected:
    T values[4];
};

struct GeneratorVertexReader: public AbstractVertexReader {
    GeneratorVertexReader(GxVertexFormat format, OgxGenerator_fv generator):
        AbstractVertexReader(format),
        generator(generator) {
    }

    void process_element(int index) override {
        float values[2];
        generator(index, values);
        for (int i = 0; i < format.num_components; i++) {
            GX_TexCoord1f32(values[i]);
        }
    }

    void read_pos3f(int index, Pos3f pos) const override {}
    void read_norm3f(int index, Norm3f norm) const override {}
    void read_color(int index, GXColor *color) const override {}
    void read_tex2f(int index, Tex2f tex) const override {
        generator(index, tex);
    }

protected:
    OgxGenerator_fv generator;
};

struct TexCoordProxyVertexReader: public AbstractVertexReader {
    TexCoordProxyVertexReader(GxVertexFormat format,
                              const AbstractVertexReader *source_reader):
        AbstractVertexReader(format),
        source_reader(source_reader) {}

    /* On these methods we do nothing, since we are just referencing data
     * already sent by another array */
    void process_element(int index) override {}
    void setup_draw() override {}
    GxSourceKind gx_source(GxSource *src) const override { return GXS_NONE; }
    void get_format(uint8_t *attribute, uint8_t *inputmode,
                    uint8_t *type, uint8_t *size) const override {
        *inputmode = GX_NONE;
    }

    /* For the other methods, we proxy the source array */
    uint8_t get_tex_coord_source() const override {
        return source_reader->get_tex_coord_source();
    }
    void read_color(int index, GXColor *color) const override {
        return source_reader->read_color(index, color);
    }
    void read_pos3f(int index, Pos3f pos) const override {
        return source_reader->read_pos3f(index, pos);
    }
    void read_norm3f(int index, Norm3f norm) const override {
        return source_reader->read_norm3f(index, norm);
    }
    void read_tex2f(int index, Tex2f tex) const override {
        return source_reader->read_tex2f(index, tex);
    }

protected:
    const AbstractVertexReader *source_reader;
};

struct VertexReaderBase: public AbstractVertexReader {
    VertexReaderBase(GxVertexFormat format, const void *data,
                     int stride):
        AbstractVertexReader(format),
        data(static_cast<const char *>(data)),
        stride(stride) {}

    void draw_done() override {}

    bool has_same_data(const OgxVertexAttribArray *array) const {
        int array_stride = compute_array_stride(array);
        const void *array_data = array->vbo ?
            _ogx_vbo_get_data(array->vbo, array->pointer) : array->pointer;
        return data == array_data && stride == array_stride;
    }

    GxSourceKind fill_source(GxSource *src, GxSourceKind kind,
                             int elem_bytes) const {
        src->data = data;
        src->stride = stride;
        src->elem_bytes = elem_bytes;
        src->num_components = format.num_components;
        return kind;
    }

    const char *data;
    uint16_t stride;
};

struct DirectVboReader: public VertexReaderBase {
    DirectVboReader(VboType vbo, GxVertexFormat format,
                    const void *data, int stride):
        VertexReaderBase(format, data, stride ? stride : format.stride()),
        vbo(vbo)
    {
    }

    void setup_draw() override {
        GX_SetArray(format.attribute, const_cast<char*>(data), stride);
        GX_SetVtxDesc(format.attribute, GX_INDEX16);
        GX_SetVtxAttrFmt(GX_VTXFMT0, format.attribute,
                         format.type, format.size, 0);
    }

    GxSourceKind gx_source(GxSource *src) const override {
        return fill_source(src, GXS_GPU, format.stride());
    }

    void draw_done() override {
        if (vbo != s_last_used_vbo) {
            _ogx_vbo_set_in_use(vbo);
            s_last_used_vbo = vbo;
        }
    }

    void get_format(uint8_t *attribute, uint8_t *inputmode,
                    uint8_t *type, uint8_t *size) const override {
        VertexReaderBase::get_format(attribute, inputmode, type, size);
        *inputmode = GX_INDEX16;
    }

    void process_element(int index) {
        GX_Position1x16(index);
    }
    template<typename T> const T *elemAt(int index) const {
        return reinterpret_cast<const T*>(data + stride * index);
    }

    template<typename T>
    void read_floats(int index, float *out) const {
        const T *ptr = elemAt<T>(index);
        out[0] = *ptr++;
        out[1] = *ptr++;
        if (format.num_components >= 3) {
            out[2] = *ptr++;
        } else {
            out[2] = 0.0f;
        }
    }

    void read_float_components(int index, float *out) const {
        switch (format.size) {
        case GX_F32: read_floats<float>(index, out); break;
        case GX_S16: read_floats<int16_t>(index, out); break;
        case GX_U16: read_floats<uint16_t>(index, out); break;
        case GX_S8: read_floats<int8_t>(index, out); break;
        case GX_U8: read_floats<uint8_t>(index, out); break;
        }
    }

    void read_pos3f(int index, Pos3f pos) const override {
        read_float_components(index, pos);
    }
    void read_norm3f(int index, Norm3f norm) const override {
        read_float_components(index, norm);
    }

    /* These should never be called on this class, since it doesn't make sense
     * to call glArrayElement() when a VBO is bound, and they are not used for
     * texture coordinate generation. */
    void read_color(int index, GXColor *color) const override {}
    void read_tex2f(int index, Tex2f tex) const override {}

    VboType vbo;
    static VboType s_last_used_vbo;
};

VboType DirectVboReader::s_last_used_vbo = 0;

template <typename T>
struct GenericVertexReader: public VertexReaderBase {
    GenericVertexReader(GxVertexFormat format, const void *data,
                        int stride):
        VertexReaderBase(format, data,
                         stride > 0 ? stride : sizeof(T) * format.num_components)
    {
    }

    const T *elemAt(int index) const {
        return reinterpret_cast<const T*>(data + stride * index);
    }

    static uint8_t read_color_component(const T *ptr) {
        if constexpr (std::numeric_limits<T>::is_integer) {
            return sizeof(T) > 1 ?
                (*ptr * 255 / std::numeric_limits<T>::max()) : *ptr;
        } else {  // floating-point type
            return *ptr * 255.0f;
        }
    }

    /* The following methods are only kept because of glArrayElement() */
    void read_color(int index, GXColor *color) const override {
        const T *ptr = elemAt(index);
        color->r = read_color_component(ptr++);
        color->g = read_color_component(ptr++);
        color->b = read_color_component(ptr++);
        color->a = format.num_components == 4 ?
            read_color_component(ptr++) : 255;
    }

    void read_pos3f(int index, Pos3f pos) const override {
        const T *ptr = elemAt(index);
        pos[0] = *ptr++;
        pos[1] = *ptr++;
        if (format.num_components >= 3) {
            pos[2] = *ptr++;
            if (format.num_components == 4) {
                float w = *ptr++;
                pos[0] /= w;
                pos[1] /= w;
                pos[2] /= w;
            }
        } else {
            pos[2] = 0.0f;
        }
    }

    void read_norm3f(int index, Norm3f norm) const override {
        const T *ptr = elemAt(index);
        norm[0] = *ptr++;
        norm[1] = *ptr++;
        norm[2] = *ptr++;
    }

    void read_tex2f(int index, Tex2f tex) const override {
        const T *ptr = elemAt(index);
        tex[0] = *ptr++;
        if (format.num_components >= 2) {
            tex[1] = *ptr++;
        } else {
            tex[1] = 0.0f;
        }
    }
};

template <typename T>
struct SameTypeVertexReader: public GenericVertexReader<T> {
    using GenericVertexReader<T>::GenericVertexReader;
    using GenericVertexReader<T>::elemAt;
    using GenericVertexReader<T>::format;
    using GenericVertexReader<T>::fill_source;

    GxSourceKind gx_source(GxSource *src) const override {
        return fill_source(src, GXS_RAW, sizeof(T) * format.num_components);
    }

    void process_element(int index) override {
        const T *ptr = elemAt(index);
        /* Directly write to the GX pipe. We don't really care if the values
         * are signed or unsigned, integers or floating point: we already know
         * that the attribute's type matches (or we wouldn't have selected the
         * SameTypeVertexReader subclass), the only thing that matters is the
         * size of the element that we put into the GX pipe. */
        for (int i = 0; i < format.num_components; i++, ptr++) {
            if constexpr (sizeof(T) == 1) {
                wgPipe->U8 = *ptr;
            } else if constexpr (sizeof(T) == 2) {
                wgPipe->U16 = *ptr;
            } else if constexpr (sizeof(T) == 4) {
                wgPipe->F32 = *ptr;
            }
        }
    }
};

template <typename T>
struct ColorVertexReader: public GenericVertexReader<T> {
    using GenericVertexReader<T>::GenericVertexReader;
    using GenericVertexReader<T>::elemAt;
    using GenericVertexReader<T>::format;
    using GenericVertexReader<T>::read_color_component;
    using GenericVertexReader<T>::fill_source;

    GxSourceKind gx_source(GxSource *src) const override {
        if constexpr (std::is_same_v<T, float>) {
            if (format.num_components >= 3)
                return fill_source(src, GXS_COLOR_FLOAT,
                                   sizeof(T) * format.num_components);
        }
        return GXS_DIRECT;
    }

    /* The bytes process_element() sends for one element (num_components 3 or
     * 4), for the converted copy of an indexed draw. */
    static void convert(const T *ptr, int num_components, uint8_t *out) {
        out[0] = read_color_component(ptr++);
        out[1] = read_color_component(ptr++);
        out[2] = read_color_component(ptr++);
        if (num_components == 4) out[3] = read_color_component(ptr);
    }

    void process_element(int index) {
        GXColor color;
        const T *ptr = elemAt(index);
        color.r = read_color_component(ptr++);
        if (format.num_components == 4) [[likely]] {
            color.g = read_color_component(ptr++);
            color.b = read_color_component(ptr++);
            color.a = read_color_component(ptr++);
            GX_Color4u8(color.r, color.g, color.b, color.a);
        } else if (format.num_components == 3) [[likely]] {
            color.g = read_color_component(ptr++);
            color.b = read_color_component(ptr++);
            GX_Color3u8(color.r, color.g, color.b);
        } else if (format.num_components == 2) {
            color.a = read_color_component(ptr++);
            GX_Color4u8(color.r, color.r, color.r, color.a);
        } else {
            GX_Color4u8(color.r, color.r, color.r, color.r);
        }
    }
};

template <typename T>
struct CoordVertexReader: public GenericVertexReader<T> {
    using GenericVertexReader<T>::elemAt;
    using GenericVertexReader<T>::format;
    using GenericVertexReader<T>::GenericVertexReader;
    using GenericVertexReader<T>::fill_source;

    /* Floats are sent as they are, except a position's w division; texture
     * coordinates keep their first two components (GX reads those). */
    GxSourceKind gx_source(GxSource *src) const override {
        if constexpr (std::is_same_v<T, float>) {
            if (format.num_components < 4 && format.size == GX_F32)
                return fill_source(src, GXS_RAW,
                                   sizeof(T) * format.num_components);
        }
        return GXS_DIRECT;
    }

    void process_element(int index) {
        const T *ptr = elemAt(index);
        if (format.num_components == 4) {
            float x, y, z, w;
            x = *ptr++;
            y = *ptr++;
            z = *ptr++;
            w = *ptr++;
            x /= w;
            y /= w;
            z /= w;
            GX_Position3f32(x, y, z);
        } else {
            for (int i = 0; i < format.num_components; i++, ptr++) {
                wgPipe->F32 = float(*ptr);
            }
        }
    }
};

static inline VertexReaderBase *get_reader(OgxArrayReader *reader)
{
    return reinterpret_cast<VertexReaderBase *>(reader);
}

/* ---------------------------------------------------------------------------
 * Indexed GX arrays.
 *
 * A draw whose attributes the GX vertex loader can fetch itself sends only
 * indices through the FIFO: GX_SetArray() points each attribute at its data
 * and the vertex descriptor says GX_INDEX8 or GX_INDEX16. Buffer object data
 * is used in place (flushed when it was uploaded); client arrays may change
 * as soon as the draw call returns, so the index range they use is copied
 * into a ring buffer that the GPU reads (float colours are converted to
 * bytes on the way). Indices are rebased to the smallest one drawn, so the
 * GX limits (index 0xffff, or 0xff with GX_INDEX8, means "skip the vertex")
 * apply to the range of a draw only.
 */

#define MAX_READERS (sizeof(s_readers) / sizeof(s_readers[0]))

/* The ring: SEGMENTS parts, each fenced by a draw sync token once the
 * allocations move past it; a part is reused after the GPU passed its fence. */
#define RING_SIZE (1024 * 1024)
#define RING_SEGMENTS 16
#define RING_SEGMENT_SIZE (RING_SIZE / RING_SEGMENTS)

static uint8_t *s_ring = NULL;
static uint32_t s_ring_pos = 0;
static int s_ring_segment = 0;
static uint32_t s_ring_pending = 0; /* segments used since the last fence */
static struct {
    uint32_t swap;
    uint16_t token;
} s_ring_fence[RING_SEGMENTS];

static uint8_t *ring_alloc(uint32_t size)
{
    size = (size + 31) & ~31u;
    if (size > RING_SIZE / 2) return NULL;
    if (!s_ring) {
        s_ring = static_cast<uint8_t*>(memalign(32, RING_SIZE));
        if (!s_ring) return NULL;
    }

    uint32_t pos = s_ring_pos;
    if (pos + size > RING_SIZE) pos = 0;
    int first = pos / RING_SEGMENT_SIZE;
    int last = (pos + size - 1) / RING_SEGMENT_SIZE;
    if (first != s_ring_segment && s_ring_pending) {
        /* All the draws that used the pending segments are in the FIFO */
        uint32_t swap = _ogx_swaps_sent();
        uint16_t token = send_draw_sync_token();
        for (int i = 0; i < RING_SEGMENTS; i++) {
            if (!(s_ring_pending & (1u << i))) continue;
            s_ring_fence[i].swap = swap;
            s_ring_fence[i].token = token;
        }
        s_ring_pending = 0;
    }
    for (int i = first; i <= last; i++) {
        if (i == s_ring_segment && (s_ring_pending & (1u << i))) continue;
        if (!_ogx_fifo_point_passed(s_ring_fence[i].swap,
                                    s_ring_fence[i].token)) {
            GX_Flush();
            while (!_ogx_fifo_point_passed(s_ring_fence[i].swap,
                                           s_ring_fence[i].token));
        }
    }
    for (int i = first; i <= last; i++)
        s_ring_pending |= 1u << i;
    s_ring_segment = last;
    s_ring_pos = pos + size;
    return s_ring + pos;
}

enum IndexedMode : uint8_t {
    IDX_DIRECT, /* process_element() */
    IDX_NONE,   /* nothing is sent */
    IDX_INDEX,  /* an index into the array set up for this draw */
};

static struct {
    bool active;
    bool all_indexed;
    uint8_t index_size; /* 1: GX_INDEX8, 2: GX_INDEX16 */
    uint8_t num_indexed;
    int min;
    IndexedMode mode[MAX_READERS];
    const void *base[MAX_READERS];
    uint8_t stride[MAX_READERS];
} s_plan;

/* Copies [start, end) of every group of overlapping client arrays (the
 * attributes of an interleaved vertex share one copy). */
struct CopyGroup {
    const char *start, *end;
    uint16_t stride;
    uint8_t *copy;
};

bool _ogx_arrays_plan_indexed(int min, int max, int count)
{
    s_plan.active = false;
    if (count <= 0 || count >= 65535) return false; /* GX_Begin's u16 count */
    int range = max - min + 1;
    if (min < 0 || range > 65535) return false;

    int n = count_attributes();
    GxSource src[MAX_READERS];
    GxSourceKind kind[MAX_READERS];
    int num_indexed = 0, num_direct = 0;
    bool needs_copy = false;
    for (int i = 0; i < n; i++) {
        kind[i] = get_reader(&s_readers[i])->gx_source(&src[i]);
        if (kind[i] == GXS_DIRECT) num_direct++;
        else if (kind[i] != GXS_NONE) num_indexed++;
        if (kind[i] == GXS_RAW || kind[i] == GXS_COLOR_FLOAT) needs_copy = true;
    }
    if (num_indexed == 0) return false;
    /* A copy is worth it while the range is not much larger than the
     * vertices drawn */
    if (needs_copy && range > 2 * count + 64) return false;

    CopyGroup groups[MAX_READERS];
    int num_groups = 0;
    int group_of[MAX_READERS];
    for (int i = 0; i < n; i++) {
        group_of[i] = -1;
        if (kind[i] != GXS_RAW) continue;
        const char *start = src[i].data + min * src[i].stride;
        const char *end = src[i].data + max * src[i].stride + src[i].elem_bytes;
        int g;
        for (g = 0; g < num_groups; g++) {
            if (groups[g].stride == src[i].stride &&
                start < groups[g].end && groups[g].start < end) break;
        }
        if (g == num_groups) {
            groups[g].start = start;
            groups[g].end = end;
            groups[g].stride = src[i].stride;
            num_groups++;
        } else {
            if (start < groups[g].start) groups[g].start = start;
            if (end > groups[g].end) groups[g].end = end;
        }
        group_of[i] = g;
    }
    for (int g = 0; g < num_groups; g++) {
        uint32_t size = groups[g].end - groups[g].start;
        groups[g].copy = ring_alloc(size);
        if (!groups[g].copy) return false;
        memcpy(groups[g].copy, groups[g].start, size);
        DCStoreRangeNoSync(groups[g].copy, size);
    }

    for (int i = 0; i < n; i++) {
        switch (kind[i]) {
        case GXS_DIRECT:
            s_plan.mode[i] = IDX_DIRECT;
            continue;
        case GXS_NONE:
            s_plan.mode[i] = IDX_NONE;
            continue;
        case GXS_GPU:
            s_plan.base[i] = src[i].data + min * src[i].stride;
            s_plan.stride[i] = src[i].stride;
            break;
        case GXS_RAW: {
            const CopyGroup &grp = groups[group_of[i]];
            s_plan.base[i] = grp.copy +
                (src[i].data + min * src[i].stride - grp.start);
            s_plan.stride[i] = src[i].stride;
            break;
        }
        case GXS_COLOR_FLOAT: {
            int nc = src[i].num_components;
            int j;
            /* CLR1 repeats CLR0's array when lighting */
            for (j = 0; j < i; j++) {
                if (kind[j] == GXS_COLOR_FLOAT && src[j].data == src[i].data &&
                    src[j].stride == src[i].stride &&
                    src[j].num_components == nc) break;
            }
            if (j < i) {
                s_plan.base[i] = s_plan.base[j];
            } else {
                uint8_t *out = ring_alloc(range * nc);
                if (!out) return false;
                const char *in = src[i].data + min * src[i].stride;
                for (int v = 0; v < range; v++, in += src[i].stride)
                    ColorVertexReader<float>::convert(
                        reinterpret_cast<const float*>(in), nc, out + v * nc);
                DCStoreRangeNoSync(out, range * nc);
                s_plan.base[i] = out;
            }
            s_plan.stride[i] = nc;
            break;
        }
        }
        s_plan.mode[i] = IDX_INDEX;
    }

    s_plan.min = min;
    s_plan.num_indexed = num_indexed;
    s_plan.all_indexed = num_direct == 0;
    s_plan.index_size = range <= 255 ? 1 : 2;
    s_plan.active = true;
    return true;
}

static void setup_indexed_draw()
{
    int n = count_attributes();
    uint8_t input = s_plan.index_size == 1 ? GX_INDEX8 : GX_INDEX16;
    for (int i = 0; i < n; i++) {
        VertexReaderBase *r = get_reader(&s_readers[i]);
        if (s_plan.mode[i] != IDX_INDEX) {
            r->setup_draw();
            continue;
        }
        const GxVertexFormat &f = r->gx_format();
        GX_SetArray(f.attribute, const_cast<void*>(s_plan.base[i]),
                    s_plan.stride[i]);
        GX_SetVtxDesc(f.attribute, input);
        GX_SetVtxAttrFmt(GX_VTXFMT0, f.attribute, f.type, f.size, 0);
    }
    /* The copies (and buffer object uploads) must reach memory before the
     * GPU reads them */
    ppcsync();
}

/* Sends the vertices count (+ the first again for a loop): index_of(i) is
 * the array index of the i-th vertex. */
template <typename F>
static inline void emit_indexed(int count, bool loop, F index_of)
{
    const int min = s_plan.min;
    const int total = count + loop;
    if (s_plan.all_indexed) {
        const int n = s_plan.num_indexed;
        if (s_plan.index_size == 1) {
            for (int i = 0; i < total; i++) {
                uint32_t v = index_of(i < count ? i : 0) - min;
                uint32_t rep = v * 0x01010101u;
                int k = n;
                for (; k >= 4; k -= 4) wgPipe->U32 = rep;
                if (k >= 2) { wgPipe->U16 = rep; k -= 2; }
                if (k) wgPipe->U8 = v;
            }
        } else {
            for (int i = 0; i < total; i++) {
                uint32_t v = index_of(i < count ? i : 0) - min;
                uint32_t rep = v | (v << 16);
                int k = n;
                for (; k >= 2; k -= 2) wgPipe->U32 = rep;
                if (k) wgPipe->U16 = v;
            }
        }
        return;
    }

    const int n = count_attributes();
    for (int i = 0; i < total; i++) {
        int index = index_of(i < count ? i : 0);
        uint32_t v = index - min;
        for (int a = 0; a < n; a++) {
            switch (s_plan.mode[a]) {
            case IDX_INDEX:
                if (s_plan.index_size == 1) wgPipe->U8 = v;
                else wgPipe->U16 = v;
                break;
            case IDX_DIRECT:
                get_reader(&s_readers[a])->process_element(index);
                break;
            case IDX_NONE:
                break;
            }
        }
    }
}

bool _ogx_arrays_indexed_planned()
{
    return s_plan.active;
}

void _ogx_arrays_draw_indexed_elements(const OgxDrawData *draw_data,
                                       const void *indices)
{
    bool loop = draw_data->gxmode.loop;
    int count = draw_data->count;
    GX_InvVtxCache();
    GX_Begin(draw_data->gxmode.mode, GX_VTXFMT0, count + loop);
    switch (draw_data->type) {
    case GL_UNSIGNED_BYTE: {
        const uint8_t *idx = static_cast<const uint8_t*>(indices);
        emit_indexed(count, loop, [idx](int i) { return int(idx[i]); });
        break;
    }
    case GL_UNSIGNED_SHORT: {
        const uint16_t *idx = static_cast<const uint16_t*>(indices);
        emit_indexed(count, loop, [idx](int i) { return int(idx[i]); });
        break;
    }
    case GL_UNSIGNED_INT: {
        const uint32_t *idx = static_cast<const uint32_t*>(indices);
        emit_indexed(count, loop, [idx](int i) { return int(idx[i]); });
        break;
    }
    }
    GX_End();
}

void _ogx_arrays_draw_indexed_arrays(const OgxDrawData *draw_data)
{
    bool loop = draw_data->gxmode.loop;
    int count = draw_data->count, first = draw_data->first;
    GX_InvVtxCache();
    GX_Begin(draw_data->gxmode.mode, GX_VTXFMT0, count + loop);
    emit_indexed(count, loop, [first](int i) { return first + i; });
    GX_End();
}

void _ogx_arrays_index_range(const void *indices, GLenum type, int count,
                             int *min_out, int *max_out)
{
    unsigned lo = ~0u, hi = 0;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        for (int i = 0; i < count; i++) {
            unsigned v = static_cast<const uint8_t*>(indices)[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        break;
    case GL_UNSIGNED_SHORT:
        for (int i = 0; i < count; i++) {
            unsigned v = static_cast<const uint16_t*>(indices)[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        break;
    case GL_UNSIGNED_INT:
        for (int i = 0; i < count; i++) {
            uint32_t v = static_cast<const uint32_t*>(indices)[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        break;
    }
    /* Ranges that do not fit an int make the plan fail */
    *min_out = lo > 0x7fffffff ? -1 : int(lo);
    *max_out = hi > 0x7fffffff ? -1 : int(hi);
}

void _ogx_arrays_setup_draw(const OgxDrawData *draw_data,
                            OgxDrawFlags flags)
{
    GX_ClearVtxDesc();

    s_draw_flags = flags;

    if (s_plan.active && flags == OGX_DRAW_FLAG_NONE) {
        setup_indexed_draw();
        return;
    }

    int num_arrays = s_draw_flags & OGX_DRAW_FLAG_FLAT ?
        1 : count_attributes();

    for (int i = 0; i < num_arrays; i++) {
        VertexReaderBase *r = get_reader(&s_readers[i]);
        r->setup_draw();
    }
}

void _ogx_arrays_process_element(int index)
{
    int num_arrays = s_draw_flags & OGX_DRAW_FLAG_FLAT ?
        1 : count_attributes();

    for (int i = 0; i < num_arrays; i++) {
        get_reader(&s_readers[i])->process_element(index);
    }
}

void _ogx_arrays_draw_done()
{
    int num_arrays = count_attributes();

    s_plan.active = false;
    DirectVboReader::s_last_used_vbo = 0;
    for (int i = 0; i < num_arrays; i++) {
        get_reader(&s_readers[i])->draw_done();
    }
}

void _ogx_array_reader_process_element(OgxArrayReader *reader, int index)
{
    VertexReaderBase *r = reinterpret_cast<VertexReaderBase *>(reader);
    r->process_element(index);
}

uint8_t _ogx_array_reader_get_tex_coord_source(OgxArrayReader *reader)
{
    return get_reader(reader)->get_tex_coord_source();
}

void _ogx_array_reader_read_pos3f(OgxArrayReader *reader,
                                  int index, float *pos)
{
    VertexReaderBase *r = reinterpret_cast<VertexReaderBase *>(reader);
    r->read_pos3f(index, pos);
}

void _ogx_array_reader_read_norm3f(OgxArrayReader *reader,
                                   int index, float *norm)
{
    VertexReaderBase *r = reinterpret_cast<VertexReaderBase *>(reader);
    r->read_norm3f(index, norm);
}

void _ogx_array_reader_read_tex2f(OgxArrayReader *reader,
                                  int index, float *tex)
{
    VertexReaderBase *r = reinterpret_cast<VertexReaderBase *>(reader);
    r->read_tex2f(index, tex);
}

void _ogx_array_reader_read_color(OgxArrayReader *reader,
                                  int index, GXColor *color)
{
    VertexReaderBase *r = reinterpret_cast<VertexReaderBase *>(reader);
    r->read_color(index, color);
}

static OgxArrayReader *allocate_reader_for_format(GxVertexFormat *format)
{
    int attr;
    switch (format->attribute) {
    case GX_VA_POS:
        return &s_readers[0];
    case GX_VA_NRM:
        s_has_normals = true;
        return &s_readers[1];
    case GX_VA_CLR0:
        if (s_num_colors >= MAX_COLOR_ARRAYS) return NULL;
        format->attribute += s_num_colors;
        attr = 1 + s_has_normals;
        return &s_readers[attr + s_num_colors++];
    case GX_VA_TEX0:
        if (s_num_tex_arrays >= MAX_TEXCOORD_ARRAYS) return NULL;
        format->attribute += s_num_tex_coords;
        attr = 1 + s_has_normals + s_num_colors;
        return &s_readers[attr + s_num_tex_arrays++];
    }
    return NULL;
}

void _ogx_arrays_reset()
{
    s_has_normals = 0;
    s_num_colors = 0;
    s_num_tex_arrays = 0;
    s_num_tex_coords = 0;
}

OgxArrayReader *_ogx_array_add_constant_fv(uint8_t attribute, int size,
                                const float *values)
{
    TemplateSelectionInfo info = select_template(GL_FLOAT, attribute, size);
    OgxArrayReader *reader = allocate_reader_for_format(&info.format);
    if (!reader) return NULL;
    if (attribute == GX_VA_CLR0) {
        /* Colors cannot be passed as floats */
        uint8_t color[4] = {
            uint8_t(values[0] * 255.0f),
            uint8_t(values[1] * 255.0f),
            uint8_t(values[2] * 255.0f),
            uint8_t(values[3] * 255.0f),
        };
        new (reader) ConstantVertexReader(info.format, color);
    } else {
        new (reader) ConstantVertexReader(info.format, values);
    }
    if (attribute == GX_VA_TEX0)
        s_num_tex_coords++;
    return reader;
}

OgxArrayReader *_ogx_array_add(uint8_t attribute, const OgxVertexAttribArray *array)
{
    TemplateSelectionInfo info =
        select_template(array->type, attribute, array->size);
    OgxArrayReader *reader = allocate_reader_for_format(&info.format);
    if (!reader) return NULL;

    if (attribute == GX_VA_TEX0) {
        /* See if the data array is the same as the positional or normal array.
         * This is not just an optimization, it's actually needed because GX
         * only supports up to two input coordinates for GX_VA_TEXx, but the
         * client might provide three (along with an appropriate texture
         * matrix). So, at least in those cases where these arrays coincide, we
         * can support having three texture input coordinates. */
        OgxArrayReader *source_reader;
        if (get_reader(&s_readers[0])->has_same_data(array)) {
            source_reader = &s_readers[0];
        } else if (s_has_normals &&
                   get_reader(&s_readers[1])->has_same_data(array)) {
            source_reader = &s_readers[1];
        } else {
            /* We could go on and check if this array has the same data of
             * another texture array sent earlier in this same loop, but let's
             * leave this optimisation for later. */
            source_reader = NULL;
        }

        if (source_reader) {
            new (reader) TexCoordProxyVertexReader(info.format,
                                                   get_reader(source_reader));
            return reader;
        }

        /* Otherwise, this is an array providing its own texture coordinates;
         * increment the counter */
        s_num_tex_coords++;
    }

    GLenum type = array->type;
    int stride = array->stride;
    const void *data = array->vbo ?
        _ogx_vbo_get_data(array->vbo, array->pointer) : array->pointer;

    if (info.same_type) {
        /* No conversions needed, just dump the data from the array directly
         * into the GX pipe. */
        if (array->vbo) {
            debug(OGX_LOG_VBO, "Using VBO %d for attr %d type %d, n=%d",
                  array->vbo, info.format.attribute, info.format.size, array->size);
            new (reader) DirectVboReader(array->vbo, info.format, data, stride);
            return reader;
        }
        switch (type) {
        case GL_UNSIGNED_BYTE:
            new (reader) SameTypeVertexReader<int8_t>(
                info.format, data, stride);
            return reader;
        case GL_SHORT:
            new (reader) SameTypeVertexReader<int16_t>(
                info.format, data, stride);
            return reader;
        case GL_INT:
            new (reader) SameTypeVertexReader<int32_t>(
                info.format, data, stride);
            return reader;
        case GL_FLOAT:
            new (reader) SameTypeVertexReader<float>(
                info.format, data, stride);
            return reader;
        }
    }

    if (attribute == GX_VA_CLR0 || attribute == GX_VA_CLR1) {
        switch (type) {
        /* The case GL_UNSIGNED_BYTE is handled by the SameTypeVertexReader */
        case GL_BYTE:
            new (reader) ColorVertexReader<char>(info.format, data, stride);
            return reader;
        case GL_SHORT:
            new (reader) ColorVertexReader<int16_t>(info.format, data, stride);
            return reader;
        case GL_INT:
            new (reader) ColorVertexReader<int32_t>(info.format, data, stride);
            return reader;
        case GL_FLOAT:
            new (reader) ColorVertexReader<float>(info.format, data, stride);
            return reader;
        case GL_DOUBLE:
            new (reader) ColorVertexReader<double>(info.format, data, stride);
            return reader;
        }
    }

    /* We can use the CoordVertexReader not only for positional
     * coordinates, but also for normals and texture coordinates because
     * the GX_Position*() functions are just storing floats into the GX
     * pipe (that is, GX_Position2f32() behaves exactly like
     * GX_TexCoord2f32()). */
    switch (type) {
    case GL_BYTE:
        new (reader) CoordVertexReader<char>(info.format, data, stride);
        return reader;
    case GL_SHORT:
        new (reader) CoordVertexReader<int16_t>(info.format, data, stride);
        return reader;
    case GL_INT:
        new (reader) CoordVertexReader<int32_t>(info.format, data, stride);
        return reader;
    case GL_FLOAT:
        new (reader) CoordVertexReader<float>(info.format, data, stride);
        return reader;
    case GL_DOUBLE:
        new (reader) CoordVertexReader<double>(info.format, data, stride);
        return reader;
    }

    warning("Unknown array data type %x for attribute %d\n",
            type, attribute);
    return NULL;
}

OgxArrayReader *_ogx_array_add_generator_fv(uint8_t attribute, int size,
                                            OgxGenerator_fv generator)
{
    assert(attribute == GX_VA_TEX0);
    TemplateSelectionInfo info = select_template(GL_FLOAT, attribute, size);
    OgxArrayReader *reader = allocate_reader_for_format(&info.format);
    if (!reader) return NULL;

    new (reader) GeneratorVertexReader(info.format, generator);
    s_num_tex_coords++;
    return reader;
}

OgxArrayReader *_ogx_array_reader_next(OgxArrayReader *reader)
{
    if (!reader) return &s_readers[0];

    int attr = (reader - s_readers) + 1;
    return attr < count_attributes() ? &s_readers[attr] : NULL;
}

OgxArrayReader *_ogx_array_reader_for_attribute(uint8_t attribute)
{
    int n;
    switch (attribute) {
    case GX_VA_POS: return &s_readers[0];
    case GX_VA_NRM: return s_has_normals ? &s_readers[1] : NULL;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        n = attribute - GX_VA_CLR0;
        return s_num_colors > n ? &s_readers[1 + s_has_normals + n] : NULL;
    default: /* This can only be a GX_VA_TEX* */
        n = attribute - GX_VA_TEX0;
        return s_num_tex_arrays > n ?
            &s_readers[1 + s_has_normals + s_num_colors + n] : NULL;
    }
}

void _ogx_array_reader_get_format(OgxArrayReader *reader,
                                  uint8_t *attribute, uint8_t *inputmode,
                                  uint8_t *type, uint8_t *size)
{
    get_reader(reader)->get_format(attribute, inputmode, type, size);
}

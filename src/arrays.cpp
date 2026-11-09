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

/* A draw whose copies exceed this is split into chunks of list primitives,
 * each copied and drawn on its own */
#define CHUNK_BYTES (256 * 1024)

/* Client arrays copied together: the attributes of one interleaved vertex
 * (same stride, all within one stride of each other) */
struct CopyGroup {
    const char *lo, *hi; /* first and one past the last byte of element 0 */
    uint16_t stride;
};

static struct {
    bool classified; /* the fields up to group[] describe the readers */
    bool active;
    bool all_indexed;
    bool chunked;
    uint8_t index_size; /* 1: GX_INDEX8, 2: GX_INDEX16 */
    uint8_t num_indexed;
    uint8_t num_groups;
    uint8_t prim_vertices; /* vertices per primitive of a chunkable mode */
    int min;               /* the index the current arrays start at */
    uint32_t copy_bytes_per_vertex;
    IndexedMode mode[MAX_READERS];
    GxSourceKind kind[MAX_READERS];
    GxSource src[MAX_READERS];
    int8_t group_of[MAX_READERS];
    CopyGroup group[MAX_READERS];
    const void *base[MAX_READERS];
    uint8_t stride[MAX_READERS];
    struct IndexList *list; /* the draw's cached index stream, if any */
} s_plan;

static int vertices_per_primitive(uint8_t gxmode)
{
    switch (gxmode) {
    case GX_POINTS: return 1;
    case GX_LINES: return 2;
    case GX_TRIANGLES: return 3;
    case GX_QUADS: return 4;
    default: return 0; /* strips, fans: not split */
    }
}

/* Points the indexed attributes at the vertices [min, max], copying the
 * client data */
static bool copy_range(int min, int max)
{
    const int n = count_attributes();
    const int range = max - min + 1;
    uint8_t *copy[MAX_READERS];
    for (int g = 0; g < s_plan.num_groups; g++) {
        const CopyGroup &grp = s_plan.group[g];
        const char *start = grp.lo + min * grp.stride;
        uint32_t size = (grp.hi - grp.lo) + (range - 1) * grp.stride;
        copy[g] = ring_alloc(size);
        if (!copy[g]) return false;
        memcpy(copy[g], start, size);
        DCStoreRangeNoSync(copy[g], size);
    }
    for (int i = 0; i < n; i++) {
        const GxSource &src = s_plan.src[i];
        switch (s_plan.kind[i]) {
        case GXS_GPU:
            s_plan.base[i] = src.data + min * src.stride;
            break;
        case GXS_RAW: {
            const CopyGroup &grp = s_plan.group[s_plan.group_of[i]];
            s_plan.base[i] = copy[s_plan.group_of[i]] + (src.data - grp.lo);
            break;
        }
        case GXS_COLOR_FLOAT: {
            int nc = src.num_components;
            int j;
            /* CLR1 repeats CLR0's array when lighting */
            for (j = 0; j < i; j++) {
                const GxSource &o = s_plan.src[j];
                if (s_plan.kind[j] == GXS_COLOR_FLOAT && o.data == src.data &&
                    o.stride == src.stride && o.num_components == nc) break;
            }
            if (j < i) {
                s_plan.base[i] = s_plan.base[j];
                break;
            }
            uint8_t *out = ring_alloc(range * nc);
            if (!out) return false;
            const char *in = src.data + min * src.stride;
            for (int v = 0; v < range; v++, in += src.stride)
                ColorVertexReader<float>::convert(
                    reinterpret_cast<const float*>(in), nc, out + v * nc);
            DCStoreRangeNoSync(out, range * nc);
            s_plan.base[i] = out;
            break;
        }
        default:
            break;
        }
    }
    /* The copies must reach memory before the GPU reads them */
    ppcsync();
    s_plan.min = min;
    return true;
}

/* Sorts the current readers into indexed and direct attributes and groups
 * the client arrays copied together. The result holds until the readers are
 * rebuilt (_ogx_arrays_reset()). */
static void classify_sources()
{
    int n = count_attributes();
    int num_indexed = 0, num_direct = 0;
    uint32_t bytes_per_vertex = 0;
    s_plan.num_groups = 0;
    for (int i = 0; i < n; i++) {
        GxSource &src = s_plan.src[i];
        GxSourceKind kind = get_reader(&s_readers[i])->gx_source(&src);
        s_plan.kind[i] = kind;
        s_plan.group_of[i] = -1;
        switch (kind) {
        case GXS_DIRECT:
            s_plan.mode[i] = IDX_DIRECT;
            num_direct++;
            continue;
        case GXS_NONE:
            s_plan.mode[i] = IDX_NONE;
            continue;
        case GXS_GPU:
            s_plan.stride[i] = src.stride;
            break;
        case GXS_RAW: {
            const char *lo = src.data, *hi = src.data + src.elem_bytes;
            int g;
            for (g = 0; g < s_plan.num_groups; g++) {
                CopyGroup &grp = s_plan.group[g];
                if (grp.stride != src.stride) continue;
                const char *glo = lo < grp.lo ? lo : grp.lo;
                const char *ghi = hi > grp.hi ? hi : grp.hi;
                if (ghi - glo <= src.stride) {
                    grp.lo = glo;
                    grp.hi = ghi;
                    break;
                }
            }
            if (g == s_plan.num_groups) {
                s_plan.group[g] = { lo, hi, src.stride };
                s_plan.num_groups++;
            }
            s_plan.group_of[i] = g;
            s_plan.stride[i] = src.stride;
            break;
        }
        case GXS_COLOR_FLOAT:
            s_plan.stride[i] = src.num_components;
            bytes_per_vertex += src.num_components;
            break;
        }
        s_plan.mode[i] = IDX_INDEX;
        num_indexed++;
    }
    for (int g = 0; g < s_plan.num_groups; g++)
        bytes_per_vertex += s_plan.group[g].stride;
    s_plan.num_indexed = num_indexed;
    s_plan.all_indexed = num_direct == 0;
    s_plan.copy_bytes_per_vertex = bytes_per_vertex;
    s_plan.classified = true;
}

bool _ogx_arrays_plan_indexed(uint8_t gxmode, int min, int max, int count)
{
    s_plan.active = false;
    s_plan.list = NULL;
    if (count <= 0) return false;
    int range = max - min + 1;
    if (min < 0 || max < min) return false;

    if (!s_plan.classified) classify_sources();
    if (s_plan.num_indexed == 0) return false;
    uint32_t bytes_per_vertex = s_plan.copy_bytes_per_vertex;

    /* A copy is worth it while the range is not much larger than the
     * vertices drawn */
    if (bytes_per_vertex && range > 2 * count + 64) return false;

    s_plan.prim_vertices = vertices_per_primitive(gxmode);
    if (range <= 65534 && count <= 65534 &&
        uint64_t(range) * bytes_per_vertex <= CHUNK_BYTES) {
        if (!copy_range(min, max)) return false;
        s_plan.chunked = false;
        s_plan.index_size = range <= 255 ? 1 : 2;
    } else {
        /* Only list primitives are split; the caller's loop vertex is a
         * line strip's */
        if (s_plan.prim_vertices == 0) return false;
        s_plan.chunked = true;
        s_plan.index_size = 2;
    }
    s_plan.active = true;
    return true;
}

static void set_indexed_arrays()
{
    int n = count_attributes();
    for (int i = 0; i < n; i++) {
        if (s_plan.mode[i] != IDX_INDEX) continue;
        GX_SetArray(get_reader(&s_readers[i])->gx_format().attribute,
                    const_cast<void*>(s_plan.base[i]), s_plan.stride[i]);
    }
}

static void set_indexed_descriptors()
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
        GX_SetVtxDesc(f.attribute, input);
        GX_SetVtxAttrFmt(GX_VTXFMT0, f.attribute, f.type, f.size, 0);
    }
}

static void setup_indexed_draw()
{
    set_indexed_descriptors();
    if (!s_plan.chunked) set_indexed_arrays();
}

/* Sends the vertices [from, to) (vertex count for the loop: the first vertex
 * again after the last): index_of(i) is the array index of the i-th one. */
template <typename F>
static inline void emit_indexed(int from, int to, int count, F index_of)
{
    const int min = s_plan.min;
    if (s_plan.all_indexed) {
        const int n = s_plan.num_indexed;
        if (s_plan.index_size == 1) {
            for (int i = from; i < to; i++) {
                uint32_t v = index_of(i < count ? i : 0) - min;
                uint32_t rep = v * 0x01010101u;
                int k = n;
                for (; k >= 4; k -= 4) wgPipe->U32 = rep;
                if (k >= 2) { wgPipe->U16 = rep; k -= 2; }
                if (k) wgPipe->U8 = v;
            }
        } else {
            for (int i = from; i < to; i++) {
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
    for (int i = from; i < to; i++) {
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

/* Vertices per chunk of a split draw: whole primitives, within the copy
 * budget and the GX index range */
static int chunk_vertices()
{
    int bpv = s_plan.copy_bytes_per_vertex;
    int v = bpv ? CHUNK_BYTES / bpv : 65534;
    if (v > 65534) v = 65534;
    v -= v % s_plan.prim_vertices;
    return v > 0 ? v : s_plan.prim_vertices;
}

template <typename F>
static void draw_chunk_direct(uint8_t mode, int from, int to, F index_of)
{
    int n = count_attributes();
    for (int i = 0; i < n; i++) get_reader(&s_readers[i])->setup_draw();
    GX_InvVtxCache();
    GX_Begin(mode, GX_VTXFMT0, to - from);
    for (int i = from; i < to; i++) {
        int index = index_of(i);
        for (int a = 0; a < n; a++)
            get_reader(&s_readers[a])->process_element(index);
    }
    GX_End();
    set_indexed_descriptors();
}

/* ---------------------------------------------------------------------------
 * Index streams kept as GX display lists.
 *
 * The indices a draw sends depend only on the index data, the primitive and
 * the number of indexed attributes: for an element buffer object (until its
 * data changes) and for glDrawArrays (0, 1, 2, ... rebased) they are the same
 * every time. Such a stream is compiled into a display list the second time
 * it is drawn, and called after that: the CPU sends the array pointers and
 * the call, the GP reads the indices from memory. Display lists hold indices
 * rebased to the range's first vertex, so they work with buffer objects and
 * with the copies of client arrays alike.
 */

#define DL_BUDGET (2 * 1024 * 1024) /* all index display lists */
#define DL_VARIANTS 3               /* per index range (passes with other
                                       attributes, other primitives) */
#define MAX_LISTS_PER_BUFFER 1024
#define IDENTITY_SLOTS 64
#define MAX_IDENTITY_COUNT 65534

struct DlVariant {
    void *dl;
    uint32_t size;
    uint8_t gxmode;
    bool loop;
    uint8_t num_indexed;
};

struct IndexList {
    IndexList *next;
    uint32_t offset; /* of the indices in the element buffer */
    int count;
    GLenum type;
    int min, max;
    uint32_t uses;
    DlVariant var[DL_VARIANTS];
};

static uint32_t s_dl_bytes = 0;
static IndexList s_identity[IDENTITY_SLOTS];

static void drop_variants(IndexList *l)
{
    for (int i = 0; i < DL_VARIANTS; i++) {
        if (!l->var[i].dl) continue;
        _ogx_free_after_gpu(l->var[i].dl);
        s_dl_bytes -= l->var[i].size;
        l->var[i].dl = NULL;
    }
}

void _ogx_arrays_drop_index_lists(void *head)
{
    IndexList *l = static_cast<IndexList*>(head);
    while (l) {
        IndexList *next = l->next;
        drop_variants(l);
        free(l);
        l = next;
    }
}

/* The cached range of count indices at offset in an element buffer; NULL if
 * the buffer keeps no more entries */
static IndexList *element_list(VboType vbo, uint32_t offset, int count,
                               GLenum type, const void *indices)
{
    void **head = _ogx_vbo_index_lists(vbo);
    if (!head) return NULL;
    IndexList **prev = reinterpret_cast<IndexList**>(head);
    int n = 0;
    for (IndexList *l = *prev; l; prev = &l->next, l = l->next, n++) {
        if (l->offset != offset || l->count != count || l->type != type)
            continue;
        /* Move to the front: draws repeat in the same order every frame */
        *prev = l->next;
        l->next = static_cast<IndexList*>(*head);
        *head = l;
        return l;
    }
    if (n >= MAX_LISTS_PER_BUFFER) return NULL;
    IndexList *l = static_cast<IndexList*>(calloc(1, sizeof(IndexList)));
    if (!l) return NULL;
    l->offset = offset;
    l->count = count;
    l->type = type;
    _ogx_arrays_index_range(indices, type, count, &l->min, &l->max);
    l->next = static_cast<IndexList*>(*head);
    *head = l;
    return l;
}

static IndexList *identity_list(int count)
{
    if (count > MAX_IDENTITY_COUNT) return NULL;
    IndexList *l = &s_identity[count % IDENTITY_SLOTS];
    if (l->count != count) {
        drop_variants(l);
        l->count = count;
        l->min = 0;
        l->max = count - 1;
        l->uses = 0;
    }
    return l;
}

/* Calls the display list of the planned draw's index stream, compiling it
 * if it was drawn before. False if the indices must be streamed. */
template <typename F>
static bool call_index_list(const OgxDrawData *draw_data, F index_of)
{
    IndexList *l = s_plan.list;
    if (!l || s_plan.chunked || !s_plan.all_indexed) return false;
    const uint8_t mode = draw_data->gxmode.mode;
    const bool loop = draw_data->gxmode.loop;
    const uint8_t n = s_plan.num_indexed;
    DlVariant *free_slot = NULL;
    for (int i = 0; i < DL_VARIANTS; i++) {
        DlVariant &v = l->var[i];
        if (!v.dl) {
            if (!free_slot) free_slot = &v;
            continue;
        }
        if (v.gxmode == mode && v.loop == loop && v.num_indexed == n) {
            GX_InvVtxCache();
            GX_CallDispList(v.dl, v.size);
            return true;
        }
    }
    if (l->uses < 2 || !free_slot) return false;

    const int count = draw_data->count, total = count + loop;
    uint32_t bytes = 3 + uint32_t(total) * n * s_plan.index_size;
    uint32_t capacity = (bytes + 63) & ~31u;
    if (s_dl_bytes + capacity > DL_BUDGET) return false;
    void *dl = memalign(32, capacity);
    if (!dl) return false;
    DCInvalidateRange(dl, capacity);
    GX_BeginDispList(dl, capacity);
    GX_Begin(mode, GX_VTXFMT0, total);
    /* Rebased to the range's first index, like the arrays */
    emit_indexed(0, total, count, index_of);
    GX_End();
    uint32_t size = GX_EndDispList();
    if (size == 0) {
        free(dl);
        return false;
    }
    free_slot->dl = dl;
    free_slot->size = size;
    free_slot->gxmode = mode;
    free_slot->loop = loop;
    free_slot->num_indexed = n;
    s_dl_bytes += capacity;
    GX_InvVtxCache();
    GX_CallDispList(dl, size);
    return true;
}

template <typename F>
static void draw_indexed(const OgxDrawData *draw_data, F index_of,
                         bool ascending)
{
    const uint8_t mode = draw_data->gxmode.mode;
    const int count = draw_data->count;
    if (!s_plan.chunked) {
        if (call_index_list(draw_data, index_of)) return;
        const int total = count + draw_data->gxmode.loop;
        GX_InvVtxCache();
        GX_Begin(mode, GX_VTXFMT0, total);
        emit_indexed(0, total, count, index_of);
        GX_End();
        return;
    }

    const int per = chunk_vertices();
    for (int from = 0; from < count; from += per) {
        int to = from + per < count ? from + per : count;
        to -= (to - from) % s_plan.prim_vertices;
        if (to <= from) break;
        int lo, hi;
        if (ascending) {
            lo = index_of(from);
            hi = index_of(to - 1);
        } else {
            lo = 0x7fffffff; hi = -1;
            for (int i = from; i < to; i++) {
                int v = index_of(i);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        int range = hi - lo + 1;
        if (range > 65534 || range > 2 * (to - from) + 64 ||
            uint64_t(range) * s_plan.copy_bytes_per_vertex > RING_SIZE / 2 ||
            !copy_range(lo, hi)) {
            draw_chunk_direct(mode, from, to, index_of);
            continue;
        }
        set_indexed_arrays();
        GX_InvVtxCache();
        GX_Begin(mode, GX_VTXFMT0, to - from);
        emit_indexed(from, to, count, index_of);
        GX_End();
    }
}

bool _ogx_arrays_plan_elements(uint8_t gxmode, bool loop, int count,
                               GLenum type, const void *indices,
                               VboType vbo, uint32_t offset)
{
    IndexList *l = NULL;
    int min, max;
    if (vbo && count > 0 && _ogx_vbo_usage(vbo) != GL_STREAM_DRAW)
        l = element_list(vbo, offset, count, type, indices);
    if (l) {
        min = l->min;
        max = l->max;
    } else {
        _ogx_arrays_index_range(indices, type, count, &min, &max);
    }
    if (min < 0 || max < 0) return false;
    if (!_ogx_arrays_plan_indexed(gxmode, min, max, count + loop)) return false;
    if (l) l->uses++;
    s_plan.list = l;
    return true;
}

bool _ogx_arrays_plan_arrays(uint8_t gxmode, bool loop, int first, int count)
{
    if (first < 0 || count <= 0) return false;
    if (!_ogx_arrays_plan_indexed(gxmode, first, first + count - 1,
                                  count + loop))
        return false;
    IndexList *l = identity_list(count);
    if (l) l->uses++;
    s_plan.list = l;
    return true;
}

/* ---------------------------------------------------------------------------
 * Draws compiled into OpenGL display lists (glNewList): the vertex data of
 * every attribute is copied once (packed per attribute, buffer object data
 * too: the list keeps the data of the time it was compiled) and the draw's
 * indices become a GX display list.
 */

struct ListArray {
    uint8_t attribute, type, size, stride;
    void *data;
};

struct OgxListArrays {
    uint8_t num_arrays;
    uint8_t input; /* GX_INDEX8 or GX_INDEX16 */
    ListArray a[MAX_READERS];
    void *dl;
    uint32_t dl_size;
};

OgxListArrays *_ogx_arrays_compile_list(uint8_t gxmode, int count, bool loop,
                                        int (*index_cb)(int i, void *data),
                                        void *cb_data, bool add_normal,
                                        bool add_colors)
{
    if (count <= 0) return NULL;
    if (!s_plan.classified) classify_sources();
    if (s_plan.num_indexed == 0 || !s_plan.all_indexed) return NULL;
    const int total = count + loop;
    if (total > 65534) return NULL;

    int min = 0x7fffffff, max = -1;
    for (int i = 0; i < count; i++) {
        int v = index_cb(i, cb_data);
        if (v < min) min = v;
        if (v > max) max = v;
    }
    if (min < 0 || max - min + 1 > 65534) return NULL;
    const int range = max - min + 1;

    OgxListArrays *l =
        static_cast<OgxListArrays*>(calloc(1, sizeof(OgxListArrays)));
    if (!l) return NULL;
    l->input = range <= 255 ? GX_INDEX8 : GX_INDEX16;
    const int isz = l->input == GX_INDEX8 ? 1 : 2;

    const int n = count_attributes();
    int bytes_per_vertex = 0;
    for (int i = 0; i < n; i++) {
        if (s_plan.mode[i] != IDX_INDEX) continue;
        const GxSource &src = s_plan.src[i];
        const GxVertexFormat &f = get_reader(&s_readers[i])->gx_format();
        bool color_float = s_plan.kind[i] == GXS_COLOR_FLOAT;
        int elem = color_float ? src.num_components : src.elem_bytes;
        ListArray &a = l->a[l->num_arrays++];
        a.attribute = f.attribute;
        a.type = f.type;
        a.size = f.size;
        a.stride = elem;
        a.data = memalign(32, range * elem);
        if (!a.data) {
            _ogx_arrays_free_list(l);
            return NULL;
        }
        uint8_t *out = static_cast<uint8_t*>(a.data);
        const char *in = src.data + min * src.stride;
        for (int v = 0; v < range; v++, in += src.stride, out += elem) {
            if (color_float)
                ColorVertexReader<float>::convert(
                    reinterpret_cast<const float*>(in), elem, out);
            else
                memcpy(out, in, elem);
        }
        DCStoreRange(a.data, range * elem);
        bytes_per_vertex += isz;
    }
    bytes_per_vertex += add_normal + 2 * add_colors;

    uint32_t capacity = (3 + uint32_t(total) * bytes_per_vertex + 63) & ~31u;
    l->dl = memalign(32, capacity);
    if (!l->dl) {
        _ogx_arrays_free_list(l);
        return NULL;
    }
    DCInvalidateRange(l->dl, capacity);
    GX_BeginDispList(l->dl, capacity);
    GX_Begin(gxmode, GX_VTXFMT0, total);
    for (int i = 0; i < total; i++) {
        uint32_t v = index_cb(i < count ? i : 0, cb_data) - min;
        for (int a = 0; a < n; a++) {
            if (s_plan.mode[a] == IDX_INDEX) {
                if (isz == 1) wgPipe->U8 = v;
                else wgPipe->U16 = v;
            }
            uint8_t attribute =
                get_reader(&s_readers[a])->gx_format().attribute;
            /* The current normal and colours (indexed arrays of one
             * element, set up when the list is called) in the GX attribute
             * order: position, normal, colour 0, colour 1 */
            if (attribute == GX_VA_POS && add_normal) wgPipe->U8 = 0;
            if ((attribute == GX_VA_POS && add_normal) ||
                attribute == GX_VA_NRM) {
                if (add_colors) { wgPipe->U8 = 0; wgPipe->U8 = 0; }
            }
        }
    }
    GX_End();
    l->dl_size = GX_EndDispList();
    if (l->dl_size == 0) {
        _ogx_arrays_free_list(l);
        return NULL;
    }
    return l;
}

void _ogx_arrays_list_setup(const OgxListArrays *l)
{
    for (int i = 0; i < l->num_arrays; i++) {
        const ListArray &a = l->a[i];
        GX_SetVtxDesc(a.attribute, l->input);
        GX_SetVtxAttrFmt(GX_VTXFMT0, a.attribute, a.type, a.size, 0);
        GX_SetArray(a.attribute, a.data, a.stride);
    }
}

void *_ogx_arrays_list_dl(const OgxListArrays *l, uint32_t *size)
{
    *size = l->dl_size;
    return l->dl;
}

void _ogx_arrays_free_list(OgxListArrays *l)
{
    if (!l) return;
    for (int i = 0; i < l->num_arrays; i++) free(l->a[i].data);
    free(l->dl);
    free(l);
}

bool _ogx_arrays_indexed_planned()
{
    return s_plan.active;
}

void _ogx_arrays_draw_indexed_elements(const OgxDrawData *draw_data,
                                       const void *indices)
{
    switch (draw_data->type) {
    case GL_UNSIGNED_BYTE: {
        const uint8_t *idx = static_cast<const uint8_t*>(indices);
        draw_indexed(draw_data, [idx](int i) { return int(idx[i]); }, false);
        break;
    }
    case GL_UNSIGNED_SHORT: {
        const uint16_t *idx = static_cast<const uint16_t*>(indices);
        draw_indexed(draw_data, [idx](int i) { return int(idx[i]); }, false);
        break;
    }
    case GL_UNSIGNED_INT: {
        const uint32_t *idx = static_cast<const uint32_t*>(indices);
        draw_indexed(draw_data, [idx](int i) { return int(idx[i]); }, false);
        break;
    }
    }
}

void _ogx_arrays_draw_indexed_arrays(const OgxDrawData *draw_data)
{
    int first = draw_data->first;
    draw_indexed(draw_data, [first](int i) { return first + i; }, true);
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
    s_plan.classified = false;
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

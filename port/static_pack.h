#ifndef P4_STATIC_PACK_H
#define P4_STATIC_PACK_H
#include <stdint.h>
#include <string.h>
#include <math.h>

enum { P4ST_MAGIC = 0x54533450, P4ST_ENTRY = 0x31534750, P4ST_WORLD = 0x32534750,
       P4ST_MAX_BYTES = 1 << 20, P4ST_BATCH_TRIANGLES = 64, P4ST_BATCH_WORDS = 6 };
typedef struct { uint32_t key, check, offset, bytes, hash; } P4StaticIndex;
typedef struct {
    uint32_t magic, vertices, meshes, lit, bits, stride, vtype, mesh_offset;
    float centre[3], scale, uv[4], lo[3], hi[3];
    uint32_t batch_offset, batches;
} P4StaticHeader;
typedef struct { uint32_t vertex, vertices, index, count, commands, words, prim, vtype; } P4StaticMesh;
typedef struct { float lo[3], hi[3]; uint32_t index, command; } P4StaticBatch;
_Static_assert(sizeof(P4StaticHeader) == 96 && sizeof(P4StaticMesh) == 32 && sizeof(P4StaticBatch) == 32, "static pack layout");

static void p4_static_hash(uint32_t key[2], const void *data, unsigned bytes)
{
    const unsigned char *p = data;
    for (unsigned i = 0; i < bytes; i++) {
        key[0] = (key[0] ^ p[i]) * 16777619u;
        key[1] = (key[1] * 33) ^ p[i];
    }
}

static int p4_static_range(unsigned offset, unsigned count, unsigned stride, unsigned bytes)
{
    return offset <= bytes && stride && count <= (bytes - offset) / stride;
}

static unsigned p4_static_align(unsigned bytes) { return (bytes + 15) & ~15u; }

static int p4_static_validate_world(const P4StaticHeader *h, unsigned bytes, const P4StaticMesh *m, unsigned start)
{
    const char *base = (const void *)h;
    if (h->batch_offset != start || !h->batches ||
        !p4_static_range(start, h->batches, sizeof(P4StaticBatch), bytes)) return 0;
    const P4StaticBatch *b = (const void *)(base + start);
    unsigned vertex = p4_static_align(start + h->batches * sizeof *b);
    unsigned packed = m->vertices;
    if (packed < h->vertices || packed > 65535 || !p4_static_range(vertex, packed, h->stride, bytes)) return 0;
    unsigned next = p4_static_align(vertex + packed * h->stride), first = 0;
    float step = h->scale / 32768;
    for (unsigned i = 0; i < h->meshes; i++, m++) {
        if (m->vertex != vertex || m->vertices != packed || m->vtype != (h->vtype | 2 << 11) ||
            m->index != next || m->prim != first || m->words < 7 || (m->words - 1) % P4ST_BATCH_WORDS ||
            !p4_static_range(m->index, m->count, 2, bytes) ||
            m->commands != p4_static_align(m->index + m->count * 2) ||
            !p4_static_range(m->commands, m->words, 4, bytes)) return 0;
        const uint32_t *w = (const void *)(base + m->commands);
        unsigned batches = (m->words - 1) / P4ST_BATCH_WORDS, end = m->index + m->count * 2;
        if (batches > h->batches - first || w[m->words - 1] != 0x0b000000) return 0;
        for (unsigned j = 0; j < batches; j++, w += P4ST_BATCH_WORDS, b++, first++) {
            unsigned prim = b->command >> 16, count = b->command & 0xffff;
            if (b->index != next || count < 3 || count > P4ST_BATCH_TRIANGLES * 3 ||
                (prim != 4 && (prim != 3 || count % 3)) ||
                !p4_static_range(next, count, 2, end) ||
                w[0] != (0x12000000 | m->vtype) || w[1] != 0x10000000 || w[2] != (0x01000000 | vertex) ||
                w[3] != 0x10000000 || w[4] != (0x02000000 | next) || w[5] != (0x04000000 | b->command)) return 0;
            for (int k = 0; k < 3; k++)
                if (!(b->lo[k] >= h->lo[k] && b->hi[k] <= h->hi[k] && b->lo[k] <= b->hi[k])) return 0;
            const uint16_t *ix = (const void *)(base + next);
            unsigned triangles = 0;
            for (unsigned k = 0; k < count; k++) {
                if (ix[k] >= packed) return 0;
                if (prim == 4 && k >= 2 && ix[k] != ix[k-1] && ix[k] != ix[k-2] && ix[k-1] != ix[k-2])
                    triangles++;
                const int16_t *p = (const void *)(base + vertex + ix[k] * h->stride + 8 + h->lit * 4);
                for (int axis = 0; axis < 3; axis++) {
                    float x = h->centre[axis] + p[axis] * step;
                    if (x < b->lo[axis] || x > b->hi[axis]) return 0;
                }
            }
            if (prim == 4 && (!triangles || triangles > P4ST_BATCH_TRIANGLES)) return 0;
            next += count * 2;
        }
        if (next != end) return 0;
        next = p4_static_align(m->commands + m->words * 4);
    }
    return first == h->batches && next == p4_static_align(bytes);
}

/* Validate the entire command language before relocating or publishing data to the GE. */
static int p4_static_validate(const void *data, unsigned bytes)
{
    if (!data || bytes < sizeof(P4StaticHeader) || bytes > P4ST_MAX_BYTES) return 0;
    const P4StaticHeader *h = data;
    int world = h->magic == P4ST_WORLD;
    if ((!world && h->magic != P4ST_ENTRY) || !h->vertices || h->vertices > 65535 || !h->meshes || h->meshes > 32 || h->lit > 1 ||
        (h->bits != 8 && h->bits != 16) || h->mesh_offset != sizeof *h ||
        !p4_static_range(h->mesh_offset, h->meshes, sizeof(P4StaticMesh), bytes)) return 0;
    unsigned type = 2 | 7 << 2 | (h->lit ? 1 << 5 : 0) | (h->bits == 8 ? 1 : 2) << 7;
    unsigned stride = 8 + h->lit * 4 + (h->bits == 8 ? 4 : 8);
    if (h->vtype != type || h->stride != stride || !(h->scale > 0) || !isfinite(h->scale) ||
        (world ? (h->bits != 16 || (h->scale != 8192 && h->scale != 16384)) :
         h->scale != 0.25f * (h->bits == 8 ? 128 : 32768))) return 0;
    for (int i = 0; i < 3; i++)
        if (!isfinite(h->centre[i]) || !isfinite(h->lo[i]) || !isfinite(h->hi[i]) || h->lo[i] > h->hi[i]) return 0;
    for (int i = 0; i < 4; i++) if (!isfinite(h->uv[i])) return 0;
    if (!(h->uv[0] > 0 && h->uv[1] > 0)) return 0;
    const P4StaticMesh *m = (const void *)((const char *)data + h->mesh_offset);
    unsigned start = h->mesh_offset + h->meshes * sizeof *m;
    if (world) return p4_static_validate_world(h, bytes, m, start);
    for (unsigned i = 0; i < h->meshes; i++, m++) {
        int indexed = m->index != 0;
        if (!m->vertices || m->vertices > 65535 || !m->count || m->count > 65535 ||
            (m->prim != 3 && m->prim != 4) || (m->prim == 3 && m->count % 3) || m->count < 3 ||
            m->vtype != (type | (indexed ? 2 << 11 : 0)) || m->words != (indexed ? 13u : 11u) ||
            (m->vertex & 15) || (m->commands & 15) || (m->index & 15) || m->vertex < start || m->commands < start ||
            !p4_static_range(m->vertex, m->vertices, stride, bytes) || !p4_static_range(m->commands, m->words, 4, bytes)) return 0;
        if (indexed) {
            if (m->index < start || !p4_static_range(m->index, m->count, 2, bytes)) return 0;
            const uint16_t *ix = (const void *)((const char *)data + m->index);
            for (unsigned j = 0; j < m->count; j++) if (ix[j] >= m->vertices) return 0;
        } else if (m->vertices != m->count) return 0;
        const uint32_t *w = (const void *)((const char *)data + m->commands);
        unsigned box = w[2] & 0xffffff, ret = m->commands + (m->words - 1) * 4;
        if (box < start || (box & 15) || !p4_static_range(box, 8, 6, bytes) ||
            w[0] != 0x12000100 || w[1] != 0x10000000 || w[2] != (0x01000000 | box) || w[3] != 0x07000008 ||
            w[4] != 0x10000000 || w[5] != (0x09000000 | ret) || w[6] != (0x12000000 | m->vtype) ||
            w[7] != 0x10000000 || w[8] != (0x01000000 | m->vertex) ||
            w[m->words - 2] != (0x04000000 | m->prim << 16 | m->count) || w[m->words - 1] != 0x0b000000) return 0;
        if (indexed && (w[9] != 0x10000000 || w[10] != (0x02000000 | m->index))) return 0;
    }
    return 1;
}

static void p4_static_address(uint32_t *base, uint32_t *low, uint32_t address)
{
    *base = 0x10000000 | (address >> 8 & 0xf0000);
    *low = (*low & 0xff000000) | (address & 0xffffff);
}

enum { P4ST_OUT, P4ST_INSIDE, P4ST_EDGE, P4ST_CLIP };

/* Perspective boxes. Compare homogeneous corners without dividing, including corners behind the eye. */
/* near_clip 0 (ICO): the GE clips at the near plane, so a corner behind it neither asks for the CPU clip nor
   breaks a whole-list call; only corners in front of the eye past the guard window do. */
static int p4_static_screen(const float lo[3], const float hi[3], const float m[4][4], float near, float guard,
                            float clip_area, int near_clip)
{
    float x[8], y[8], w[8];
    x[0] = m[3][0] + lo[0]*m[0][0] + lo[1]*m[1][0] + lo[2]*m[2][0];
    y[0] = m[3][1] + lo[0]*m[0][1] + lo[1]*m[1][1] + lo[2]*m[2][1];
    w[0] = m[3][3] + lo[0]*m[0][3] + lo[1]*m[1][3] + lo[2]*m[2][3];
    for (unsigned axis = 0; axis < 3; axis++) {
        unsigned n = 1u << axis;
        float d = hi[axis] - lo[axis];
        float dx = d * m[axis][0], dy = d * m[axis][1], dw = d * m[axis][3];
        for (unsigned i = 0; i < n; i++)
            x[i+n] = x[i] + dx, y[i+n] = y[i] + dy, w[i+n] = w[i] + dw;
    }
    unsigned any = 0, all = 31, unsafe = 0;
    float gx = clip_area ? 1 + 1024.0f / 240 : guard;
    float gy = clip_area ? 1 + 1024.0f / 136 : guard;
    for (int i = 0; i < 8; i++) {
        /* Cull with a small outward margin for GE matrix/viewport rounding. */
        float eps = (fabsf(x[i]) + fabsf(y[i]) + fabsf(w[i]) + 1) * 0.0001f;
        float z = w[i] + eps;
        unsigned code = (x[i] > z) | (x[i] < -z) << 1 | (y[i] > z) << 2 | (y[i] < -z) << 3 | (z < near) << 4;
        any |= near_clip || !(code & 16) ? code : 0; /* behind the near plane x and y say nothing */
        all &= code;
        unsafe |= (near_clip && !clip_area && w[i] - eps < near) ||
                  (w[i] > 0 && (fabsf(x[i]) > w[i] * gx - eps || fabsf(y[i]) > w[i] * gy - eps));
    }
    if (all) return P4ST_OUT;
    if (unsafe) {
        if (!clip_area) return P4ST_CLIP;
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int i = 0; i < 8; i++) {
            if (w[i] <= 0) return P4ST_EDGE;
            float inv = 1.0f / w[i], sx = x[i] * inv, sy = y[i] * inv;
            if (sx < x0) x0 = sx;
            if (sx > x1) x1 = sx;
            if (sy < y0) y0 = sy;
            if (sy > y1) y1 = sy;
        }
        if ((x1 - x0) * (y1 - y0) >= clip_area) return P4ST_CLIP;
        return P4ST_EDGE;
    }
    return any ? P4ST_EDGE : P4ST_INSIDE;
}

static void p4_static_relocate(void *data, uint32_t address)
{
    P4StaticHeader *h = data;
    P4StaticMesh *m = (void *)((char *)data + h->mesh_offset);
    for (unsigned i = 0; i < h->meshes; i++, m++) {
        uint32_t *w = (void *)((char *)data + m->commands);
        if (h->magic == P4ST_WORLD) {
            for (unsigned j = 0; j + 1 < m->words; j += P4ST_BATCH_WORDS) {
                p4_static_address(w + j + 1, w + j + 2, address + m->vertex);
                p4_static_address(w + j + 3, w + j + 4, address + (w[j + 4] & 0xffffff));
            }
            continue;
        }
        p4_static_address(w + 1, w + 2, address + (w[2] & 0xffffff));
        p4_static_address(w + 4, w + 5, address + m->commands + (m->words - 1) * 4);
        p4_static_address(w + 7, w + 8, address + m->vertex);
        if (m->index) p4_static_address(w + 9, w + 10, address + m->index);
    }
}
#endif

/* Only exceptional batches use the triangle clipper. Float output uses the same model and UV decode. */
static int static_world_clip(const P4StaticHeader *h, const P4StaticMesh *m, const P4StaticBatch *b,
                             const Mat4 projection, float near, unsigned **words)
{
    const char *base = (const void *)h;
    const uint16_t *ix = (const void *)(base + b->index);
    int strip = b->command >> 16 == GU_TRIANGLE_STRIP, count = b->command & 0xffff;
    int triangles = strip ? count - 2 : count / 3;
    int stride = h->lit ? sizeof(Vertex3DN) : sizeof(Vertex3D);
    int space = (MAX_VERTICES - vertex_used) * (int)sizeof(Vertex3D) / stride, emitted = 0;
    void *output = vertex_pool + vertex_used;
    for (int t = 0; t < triangles; t++) {
        int first = strip ? t : t * 3;
        int swap = strip && (t & 1);
        unsigned ids[3] = { ix[first + swap], ix[first + 1 - swap], ix[first + 2] };
        if (ids[0] == ids[1] || ids[0] == ids[2] || ids[1] == ids[2]) continue;
        ClipVertex tri[3];
        unsigned planes = 0, outside = 31, offscreen = 15;
        for (int k = 0; k < 3; k++) {
            const char *src = base + m->vertex + ids[k] * h->stride;
            const uint16_t *uv = (const void *)src;
            const int16_t *p = (const void *)(src + 8 + h->lit * 4);
            const int8_t *n = (const void *)(src + 8);
            float x = p[0] / 32768.0f, y = p[1] / 32768.0f, z = p[2] / 32768.0f;
            tri[k].v = (Vertex3D){uv[0] / 32768.0f, uv[1] / 32768.0f, *(const unsigned *)(src + 4), x, y, z};
            for (int a = 0; a < 3; a++)
                tri[k].n[a] = h->lit ? n[a] : 0;
            for (int a = 0; a < 4; a++)
                tri[k].c[a] = x*projection[0][a] + y*projection[1][a] + z*projection[2][a] + projection[3][a];
            float cx = tri[k].c[0], cy = tri[k].c[1], w = tri[k].c[3];
            float limit = w + (fabsf(cx) + fabsf(cy) + fabsf(w) + 1) * 0.0001f;
            offscreen &= (cx > limit) | (cx < -limit) << 1 | (cy > limit) << 2 | (cy < -limit) << 3;
            unsigned code = 0;
            for (int a = 0; a < 5; a++)
                code |= (unsigned)(guard_distance(tri[k].c, a, near) < 0) << a;
            planes |= code;
            outside &= code;
        }
        if (outside || offscreen) continue;
        Vertex3D v[GUARD_OUT_MAX];
        float n[GUARD_OUT_MAX][3];
        int nv = clip_triangle_guard_fast(tri, near, v, h->lit ? n : 0, planes);
        if (nv > space - emitted) return 0;
        for (int k = 0; k < nv; k++, emitted++) {
            if (h->lit) {
                ((Vertex3DN *)output)[emitted] = (Vertex3DN){v[k].u, v[k].v, v[k].colour,
                    normal_s8(n[k][0] / 127), normal_s8(n[k][1] / 127), normal_s8(n[k][2] / 127), 0,
                    v[k].x, v[k].y, v[k].z};
            } else {
                ((Vertex3D *)output)[emitted] = v[k];
            }
        }
    }
    if (emitted) {
        sceKernelDcacheWritebackRange(output, emitted * stride);
        vertex_used += (emitted * stride + sizeof(Vertex3D) - 1) / sizeof(Vertex3D);
        unsigned type = (VT_3D & ~GU_INDEX_BITS) | (h->lit ? GU_NORMAL_8BIT : 0);
        *words = draw_address(*words, GU_TRIANGLES, type, emitted, 0, output);
    }
    return 1;
}

static int static_world_lists(const P4StaticHeader *h, const P4StaticMesh *m, Mat4 projection, float near,
                              const unsigned **lists)
{
    const char *base = (const void *)h;
    const P4StaticBatch *batches = (const void *)(base + h->batch_offset);
    /* Reserve for the original six words per batch and the list-memory jump per material. */
    if (!compact_room((h->batches * P4ST_BATCH_WORDS + h->meshes * 8) * 4)) return 0;
    Mat4 packed;
    memcpy(packed, projection, sizeof packed);
    for (int j = 0; j < 4; j++) {
        for (int k = 0; k < 3; k++) {
            packed[3][j] += h->centre[k] * projection[k][j];
            packed[k][j] *= h->scale;
        }
    }
    int saved = vertex_used;
    unsigned clips = 0, words = 0;
    /* ICO auto: 1024 pixels past the view and 16 screens of projected area. */
    float area = p4_static_pack == 2 ? 16 * 4 : 0;
    for (unsigned i = 0; i < h->meshes; i++, m++) {
        const P4StaticBatch *b = batches + m->prim;
        const unsigned *source = (const void *)(base + m->commands);
        unsigned *list = sceGuGetMemory(m->words * 4), *out = list;
        for (unsigned j = 0; j + 1 < m->words; j += P4ST_BATCH_WORDS, b++) {
            int verdict = p4_static_screen(b->lo, b->hi, projection, near, GUARD, area, p4_static_near);
            if (verdict == P4ST_OUT) continue;
            if (verdict == P4ST_CLIP) {
                prof_begin(PROF_GCLIP);
                int ok = static_world_clip(h, m, b, packed, near * 1.001f, &out);
                prof_end(PROF_GCLIP);
                if (!ok) {
                    vertex_used = saved;
                    guard_pool_exhausted++;
                    return 0;
                }
                clips++;
            } else {
                memcpy(out, source + j, P4ST_BATCH_WORDS * 4);
                out += P4ST_BATCH_WORDS;
            }
        }
        lists[i] = out == list ? 0 : list;
        *out++ = 0x0b000000;
        sceKernelDcacheWritebackRange(list, (out - list) * 4);
        words += out - list;
    }
    static_world_clips += clips;
    static_world_words += words;
    return 1;
}

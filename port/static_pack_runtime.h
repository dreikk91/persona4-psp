/* Included after the static-list lifetime, guard and model-matrix helpers. */
#include "static_pack.h"
int p4_static_pack = 0, p4_static_pack_check = 0;
/* 1: b451 rule, a world box reaching the near plane goes to the CPU clip; 0: the GE clips it (ICO). b454: 1 again,
   0 showed bright cloud bands on the PSP (b453 Heaven bridge: 65 ms vs 79 ms, both far above the 40 ms base) */
int p4_static_near = 1;
static P4StaticIndex *static_pack_index;
static unsigned static_pack_count, static_pack_hits, static_pack_misses, static_pack_bad, static_pack_checks, static_pack_diffs;
static unsigned static_pack_s8, static_pack_s16;
static unsigned static_world_out, static_world_calls, static_world_clips, static_world_words;
static int static_pack_tried;
typedef struct {
    const Geometry *g;
    const void *vertices, *uv, *prelit, *normals, *mesh;
    unsigned flags, mesh_flags, serial, count, meshes, key[2], bytes, allocation;
    int alpha[2];
} StaticPackRef;

static void static_pack_drop(StaticList *e)
{
    StaticPackRef *r = e->offline;
    if (r) {
        sl_bytes -= r->allocation;
        sl_bury(r);
        e->offline = 0;
    }
}

static int static_pack_load_index(void)
{
    if (static_pack_tried) return static_pack_count != 0;
    static_pack_tried = 1;
    SceUID fd = sceIoOpen(p4_path("p4static.bin"), PSP_O_RDONLY, 0);
    unsigned h[4];
    if (fd < 0) return 0;
    int length = sceIoLseek32(fd, 0, PSP_SEEK_END);
    int ok = length >= 16 && sceIoLseek32(fd, 0, PSP_SEEK_SET) == 0 && sceIoRead(fd, h, 16) == 16 &&
             h[0] == P4ST_MAGIC && h[1] == 1 && h[2] && h[2] <= 65536 && h[3] == 16 + h[2] * sizeof(P4StaticIndex) && h[3] <= (unsigned)length;
    if (ok) {
        unsigned bytes = h[2] * sizeof(P4StaticIndex);
        static_pack_index = malloc(bytes);
        ok = static_pack_index && sceIoRead(fd, static_pack_index, bytes) == (int)bytes;
    }
    unsigned next = ok ? h[3] : 0;
    for (unsigned i = 0; ok && i < h[2]; i++) {
        const P4StaticIndex *e = static_pack_index + i;
        ok = e->offset == next && e->bytes >= sizeof(P4StaticHeader) && e->bytes <= P4ST_MAX_BYTES &&
             e->offset <= (unsigned)length && e->bytes <= (unsigned)length - e->offset;
        if (i) {
            const P4StaticIndex *p = e - 1;
            ok &= p->key < e->key || (p->key == e->key && p->check < e->check);
        }
        next = e->offset + e->bytes;
    }
    ok &= next == (unsigned)length;
    sceIoClose(fd);
    if (!ok) {
        free(static_pack_index), static_pack_index = 0;
        static_pack_bad++;
        return 0;
    }
    static_pack_count = h[2];
    p4_log("static pack: %u entries\n", static_pack_count);
    return 1;
}

static void static_pack_key(const Geometry *g, const V3 *normals, uint32_t key[2])
{
    unsigned head[7] = { g->numVertices, g->mesh->numMeshes, g->mesh->flags & MESH_PRIM_MASK,
                        normals != 0, g->numTexCoordSets != 0, g->preLit != 0, normals != 0 };
    key[0] = 2166136261u, key[1] = 5381;
    p4_static_hash(key, head, sizeof head);
    p4_static_hash(key, g->morphTarget->verts, g->numVertices * 12);
    if (g->numTexCoordSets) p4_static_hash(key, g->texCoords[0], g->numVertices * 8);
    if (g->preLit) p4_static_hash(key, g->preLit, g->numVertices * 4);
    if (normals) p4_static_hash(key, normals, g->numVertices * 12);
    const Mesh *m = (const void *)((const char *)g->mesh + sizeof(MeshHeader) + g->mesh->firstMeshOffset);
    for (unsigned i = 0; i < g->mesh->numMeshes; i++) {
        p4_static_hash(key, &m[i].numIndices, 4);
        p4_static_hash(key, m[i].indices, m[i].numIndices * 2);
    }
}

static StaticPackRef *static_pack_bind(StaticList *e, const Geometry *g, const V3 *normals)
{
    const void *uv = g->numTexCoordSets ? g->texCoords[0] : 0;
    StaticPackRef *r = e->offline;
    int same = r && r->g == g && r->vertices == g->morphTarget->verts && r->uv == uv && r->prelit == g->preLit &&
        r->normals == normals && r->mesh == g->mesh && r->flags == g->flags && r->serial == g->mesh->serial &&
        r->mesh_flags == g->mesh->flags && r->count == (unsigned)g->numVertices && r->meshes == g->mesh->numMeshes;
    if (same) {
        if (p4_static_pack_check && !(cache_frame & 63)) {
            uint32_t key[2];
            static_pack_key(g, normals, key);
            static_pack_checks++;
            if (memcmp(key, r->key, sizeof key)) {
                static_pack_diffs++;
                static_pack_drop(e);
                return 0;
            }
        }
        return r;
    }
    static_pack_drop(e);
    uint32_t key[2];
    static_pack_key(g, normals, key);
    unsigned lo = 0, hi = static_pack_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const P4StaticIndex *p = static_pack_index + mid;
        if (p->key < key[0] || (p->key == key[0] && p->check < key[1])) lo = mid + 1;
        else hi = mid;
    }
    const P4StaticIndex *p = lo < static_pack_count && static_pack_index[lo].key == key[0] && static_pack_index[lo].check == key[1] ? static_pack_index + lo : 0;
    unsigned prefix = (sizeof(StaticPackRef) + 15) & ~15u, bytes = prefix + (p ? p->bytes : 0);
    if (sl_bytes > (unsigned)p4_static_list_budget || bytes > (unsigned)p4_static_list_budget - sl_bytes) return 0;
    r = memalign(16, bytes);
    if (!r) return 0;
    memset(r, 0, prefix);
    *r = (StaticPackRef){ .g=g, .vertices=g->morphTarget->verts, .uv=uv, .prelit=g->preLit, .normals=normals, .mesh=g->mesh,
        .flags=g->flags, .mesh_flags=g->mesh->flags, .serial=g->mesh->serial, .count=g->numVertices,
        .meshes=g->mesh->numMeshes, .key={key[0],key[1]}, .allocation=bytes };
    if (p) {
        void *data = (char *)r + prefix;
        SceUID fd = sceIoOpen(p4_path("p4static.bin"), PSP_O_RDONLY, 0);
        int ok = fd >= 0 && sceIoLseek32(fd, p->offset, PSP_SEEK_SET) == (int)p->offset && sceIoRead(fd, data, p->bytes) == (int)p->bytes;
        if (fd >= 0) sceIoClose(fd);
        uint32_t hash[2] = {2166136261u,5381};
        if (ok) p4_static_hash(hash, data, p->bytes);
        ok = ok && hash[0] == p->hash && p4_static_validate(data, p->bytes);
        const P4StaticHeader *h = data;
        ok = ok && h->vertices == (unsigned)g->numVertices && h->meshes == g->mesh->numMeshes && h->lit == (unsigned)(normals != 0);
        if (!ok) {
            static_pack_bad++;
            free(r);
            return 0;
        }
        r->alpha[0] = r->alpha[1] = 255;
        if (h->magic == P4ST_WORLD) {
            const P4StaticMesh *m = (const void *)((const char *)h + h->mesh_offset);
            r->alpha[1] = 0;
            for (unsigned i = 0; i < m->vertices; i++) {
                int a = ((const unsigned char *)h)[m->vertex + i * h->stride + 7];
                if (a < r->alpha[0]) r->alpha[0] = a;
                if (a > r->alpha[1]) r->alpha[1] = a;
            }
        }
        p4_static_relocate(data, (unsigned)data);
        sceKernelDcacheWritebackRange(data, p->bytes);
        r->bytes = p->bytes;
    } else static_pack_misses++;
    e->offline = r;
    sl_bytes += bytes;
    return r;
}

#include "static_world.h"

static int static_pack_try(StaticList *e, const Geometry *g, const Matrix *model, const V3 *normals)
{
    if (g->locked) { static_pack_drop(e); return 0; }
    if (!p4_static_pack || g->locked || g->numMorphTargets != 1 || g->numVertices < 1 || g->numVertices > 65535 ||
        !g->mesh->numMeshes || g->mesh->numMeshes > 32 || (g->flags & GEOMETRY_NATIVE) || p4_alpha_x2 ||
        !static_pack_load_index()) return 0;
    StaticPackRef *r = static_pack_bind(e, g, normals);
    if (!r || !r->bytes) return 0;
    const P4StaticHeader *h = (const void *)((const char *)r + ((sizeof *r + 15) & ~15u));
    const P4StaticMesh *p = (const void *)((const char *)h + h->mesh_offset);
    int world = h->magic == P4ST_WORLD;
    if (!world && p4_static_pack == 2) return 0;
    if (!world && (!render_state[RS_ZWRITE] || !render_state[RS_ZTEST] ||
        (sky_state[2] && (sky_state[2] & 0xff) != BLEND_NORMAL))) return 0;
    const Mesh *m = (const void *)((const char *)g->mesh + sizeof(MeshHeader) + g->mesh->firstMeshOffset);
    int modulate = (g->flags & 0x40) != 0;
    for (unsigned i = 0; i < g->mesh->numMeshes; i++)
        if (!m[i].indices || m[i].numIndices < 3 || m[i].numIndices > 65535 || !m[i].material ||
            (!world && m[i].material->colour >> 24 != 255) || (!normals && modulate && m[i].material->colour != 0xffffffff) ||
            matfx_env_entry(m[i].material) || ((!world || !normals) && translucent_material(m[i].material, modulate))) return 0;
    const unsigned *lists[32];
    int verdict = P4ST_INSIDE;
    float near = *(const float *)((const char *)current_camera + CAMERA_NEAR);
    if (world) {
        if (*(const int *)((const char *)current_camera + CAMERA_PROJECTION) == 2) return 0;
        Mat4 projection;
        guard_matrix_cached(projection, model);
        verdict = p4_static_screen(h->lo, h->hi, projection, near, GUARD, 0, p4_static_near);
        if (verdict == P4ST_OUT) {
            e->stamp = cache_frame;
            static_world_out++;
            return 1;
        }
        begin_frame();
        if (verdict != P4ST_INSIDE && !static_world_lists(h, p, projection, near, lists)) return 0;
    } else {
        begin_frame();
        gu_mesh_begin();
        for (unsigned i = 0; i < g->mesh->numMeshes; i++) {
            void **texture = m[i].material->texture;
            bind_raster(texture ? *texture : 0, GU_REPEAT);
            if (bound_alpha_min != 255) return 0;
        }
        float sphere[4];
        memcpy(sphere, g->morphTarget->sphere, sizeof sphere);
        sphere[3] += 0.25f;
        if (p4_guard_clip && guard_code && !replay_unguarded(model, sphere)) {
            guard_world_planes();
            if (!guard_sphere_world(model, sphere, near * 1.01f)) return 0;
        }
    }
    SceneLight light = mesh_state(model, normals != 0);
    StaticList decode = { .q_scale=h->scale };
    memcpy(decode.q_centre, h->centre, sizeof decode.q_centre);
    sl_model(&decode, model);
    bind_texscale(h->uv);
    draw_prelit = g->preLit, draw_count = g->numVertices, draw_alpha_known = 1, draw_prelit_min = r->alpha[0];
    draw_fixed_alpha = r->alpha;
    draw_site = DS_SLPLAY;
    draw_material_ge = 0;
    for (unsigned i = 0; i < h->meshes; i++) {
        const unsigned *commands = world && verdict != P4ST_INSIDE ? lists[i] : (const void *)((const char *)h + p[i].commands);
        if (!commands) continue;
        void **texture = m[i].material->texture;
        bind_mask(m[i].material->colour >> 24);
        bind_raster(texture ? *texture : 0, GU_REPEAT);
        draw_static_template = commands;
        draw_mesh_vertices(world ? GU_TRIANGLES : p[i].prim, p[i].vtype, p[i].count,
            p[i].index ? (const char *)h + p[i].index : 0, (const char *)h + p[i].vertex, m[i].material, modulate, &light);
        draw_static_template = 0;
        if (world && verdict == P4ST_INSIDE) static_world_calls++, static_world_words += p[i].words;
    }
    draw_fixed_alpha = 0;
    draw_site = DS_OTHER;
    bind_texscale(0);
    replay.valid = 0;
    e->stamp = cache_frame;
    static_pack_hits++;
    if (h->bits == 8) static_pack_s8++; else static_pack_s16++;
    return 1;
}

static void static_pack_report(void)
{
    if (p4_static_pack || static_pack_hits || static_pack_bad)
        p4_log("static pack %d hits %u s8 %u s16 %u misses %u bad %u checks %u diffs %u per 256 frames\n",
            p4_static_pack, static_pack_hits, static_pack_s8, static_pack_s16, static_pack_misses, static_pack_bad, static_pack_checks, static_pack_diffs);
    if (p4_static_pack && (static_world_out || static_world_calls || static_world_words))
        p4_log("static world per 256 frames: culled %u whole calls %u CPU clip batches %u list words %u\n",
               static_world_out, static_world_calls, static_world_clips, static_world_words);
    static_world_out = static_world_calls = static_world_clips = static_world_words = 0;
    static_pack_hits = static_pack_s8 = static_pack_s16 = static_pack_misses = static_pack_bad = static_pack_checks = static_pack_diffs = 0;
}

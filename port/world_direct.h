/* p4_world_direct (b453): the field's material-group passes drawn from our own arrays, not by RenderWare's walk.

   The game queues each field clump's draw buckets (sdkClumpCreateBuckets: KClumpDrawEntry lists by material
   category) as OT nodes; sdkOtFlushBuckets runs movieNativeDrawPass1/2/3 (retail 00461560, 00461A40, 00461BE0),
   which walk the lists, ask RW for each atomic's world sphere (RpAtomicGetWorldBoundingSphere), test it against the
   camera (RwCameraFrustumTestSphere) and call the atomic's render callback, our atomic_render_body. On a static
   list hit that body ends in one CALL, but on the way it reads the atomic, geometry, materials, textures and lights
   again for every atomic: ~25k cycles an atomic in Heaven (b452, 210 a frame), mostly cache misses.

   Here the passes are ours. Each bucket list gets a flat array in list order. An entry whose callback last ended in
   exactly one prepared-list CALL (sl_draw's hit or the instance memo) keeps that list, its model matrix and world
   sphere; an entry outside the view keeps its sphere only. Next frames draw it with its own sphere test and the
   CALL; the game's list, its gates, render states and order stay as they are. The body runs again (and learns
   again) when the frame's matrix, the render/Sky state, camera, lights, its rasters or the list changed, near the
   guard band, for vertex-alpha entries, for the fade buckets (their material-alpha tests run every frame), and for
   every entry once in 32 frames (material changes).
   b454: the frustum and guard results are kept while the camera (view, planes) and the matrix stay; the state key is
   computed again only when a body changed the render/Sky state; the camera is set once a step; textures are checked
   against the rasters changed since (rchg) instead of all the entry's rasters; a direct draw sets the instance memo
   as the body's hit does (b453's diffs: memo sources flipping). The log line says why entries took the body.
   p4_world_direct_check 1: every 64th frame all entries take the body and the result is compared (diffs logged).
   0: the retail passes' logic (every entry through the body), as b452. */
int p4_world_direct = 1, p4_world_direct_check = 0; /* b461: on (PSP Heaven 41.0 -> 37.9 ms, Junes 52.8 -> 44.4 ms, profiled; picture unchanged, check 0 list diffs) */

enum { WD_NONE, WD_CALL, WD_CULL };
typedef struct {
    const void *node, *atomic, *frame, *g, *e, *eatom;
    const unsigned *list;
    unsigned draws, built, state, bits, light, texgen, epoch, camkey, materials;
    const void *camera;
    signed char kind, lit, memo, vis, vis_known, inside; /* inside -1: not tested for camkey */
    signed char valpha;  /* b456: drawn with vertex alpha off (the entry's flag), keys taken in that state */
    signed char gate, modulate; /* b462: a gated list (p4_static_gate): crossing atomics, cut by chunks */
    signed char fp, far; /* b456: a far stand-in candidate (fp_try); far: its decision when learned (hysteresis) */
    int flags;
    float sphere[4]; /* model space: the guard-band test */
    float world[4];  /* the RW world sphere at learning: the frustum test */
    Matrix model __attribute__((aligned(16)));
    Matrix gemodel __attribute__((aligned(16))); /* model x the list's position decode (sl_model) */
} WdEntry;
typedef struct { const void *record; int head, n, cap; WdEntry *e; unsigned used; } WdList;
enum { WD_LISTS = 96 };
static WdList *wd_lists;
/* why an entry took the body (W_*), and why it was not kept after it (N_*) */
enum { W_NEW, W_FORCED, W_VALPHA, W_MATRIX, W_VISIBLE, W_LIST, W_CAMERA, W_STATE, W_LIGHT, W_TEX, W_GUARD, W_FADE, W_OFF, W_FAR, W_N };
enum { N_GATE, N_NONE, N_MULTI, N_FAR, N_CB, N_DRAWS, N_VALPHA, N_OFF, N_N };
static unsigned wd_gated, wd_rechecked, wd_direct, wd_moved, wd_culled, wd_body, wd_learned, wd_checks, wd_why[W_N], wd_not[N_N], wd_diff[4];
static unsigned wd_why_us[W_N], wd_not_us[N_N]; /* b457: the body's time by reason (us per 256 frames) */
/* b457: the geometries that take the body most (not kept, or crossing the guard band), by time */
enum { WD_TOP = 8 };
static struct { const Geometry *g; unsigned calls, us; int why, env, has, list, slwhy, split, meshes, retry, nv, nm, gflags; } wd_top[WD_TOP]; /* b488: the geometry's numbers taken when noted: it may be freed by the report (b485-b487 crash after a scene change) */
static const StaticList *wd_sl_peek(const void *atomic) /* sl_find without making an entry */
{
    if (!sl_table)
        return 0;
    unsigned h = ((unsigned)atomic >> 4) * 2654435761u;
    h ^= h >> 15;
    const StaticList *a = &sl_table[(h & (SL_SLOTS / 2 - 1)) * 2];
    return a->atomic == atomic ? a : a[1].atomic == atomic ? a + 1 : 0;
}
static void wd_top_note(const void *atomic, const Geometry *g, unsigned us, int why)
{
    int k = 0, low = 0;
    for (; k < WD_TOP && wd_top[k].g && wd_top[k].g != g; k++)
        if (wd_top[k].us < wd_top[low].us)
            low = k;
    if (k == WD_TOP)
        k = low, wd_top[k].g = 0;
    if (!wd_top[k].g)
        wd_top[k].g = g, wd_top[k].calls = wd_top[k].us = 0, wd_top[k].why = why;
    wd_top[k].calls++, wd_top[k].us += us;
    wd_top[k].nv = g->numVertices, wd_top[k].nm = g->mesh->numMeshes, wd_top[k].gflags = g->flags;
    const StaticList *e = wd_sl_peek(atomic);
    wd_top[k].env = matfx_geometry_env(g), wd_top[k].has = e != 0, wd_top[k].list = e && e->list, wd_top[k].slwhy = e ? e->why : -1;
    wd_top[k].split = e && e->split, wd_top[k].meshes = e && e->meshes, wd_top[k].retry = e ? (int)(e->retry - cache_frame) : 0;
}
static int wd_seen; /* wd_watch: the body's prepared-list calls for the atomic being learned */
static struct { const void *e; const unsigned *list; unsigned draws, state, bits; const Matrix *model; int lit, memo, gate; } wd_got;
static unsigned wd_pass_state, wd_pass_bits, wd_pass_light, wd_camkey;
static int wd_pass_valid, wd_pass_light_valid, wd_cam_ok;
/* b481 p4_world_defer: direct entries are not emitted as they are decided but queued and emitted together, in
   order, when a body is about to draw or the list ends: the decision loop (game nodes, frames, our entries) and the
   emission loop (GE words, matrices) each run with their own code and data in the caches. 0: emitted at once. */
int p4_world_defer = 1; /* b481 PSP Heaven: imiss/frame 38.2k -> 36.6k, frame -0.1 M cycles, picture same: on */
enum { WD_Q = 256 };
static unsigned short wd_q[WD_Q];
static int wd_qn;
static WdList *wd_q_list;
static unsigned wd_flushes;
static void wd_flush(void);
static void wd_emit_now(WdEntry *w);
static int wd_snap_render[MAX_STATE];
static unsigned wd_snap_sky[8], wd_snap_mask;

static void wd_note(const void *e, const unsigned *list, unsigned draws, const Matrix *model, const void *normals, int memo)
{
    if (sl_in_part) { /* a mesh part: never one call for the atomic */
        wd_seen += 2;
        return;
    }
    wd_seen++;
    wd_got.e = e, wd_got.list = list, wd_got.draws = draws, wd_got.model = model, wd_got.lit = normals != 0, wd_got.memo = memo;
    wd_got.state = sl_state_key(), wd_got.bits = sl_bits(); /* the state the list was taken in (b456: vertex alpha) */
    wd_got.gate = 0;
}
static void wd_note_gate(const void *e, const Matrix *model, const void *normals)
{
    wd_note(e, ((const StaticList *)e)->list, ((const StaticList *)e)->draws, model, normals, 0);
    wd_got.gate = 1;
}

/* atomic_cover from the entry's own sphere copy */
static float wd_cover(const float *s, const Matrix *model)
{
    const Matrix *v = (const Matrix *)((char *)current_camera + CAMERA_VIEW);
    float wx = s[0] * model->right.x + s[1] * model->up.x + s[2] * model->at.x + model->pos.x;
    float wy = s[0] * model->right.y + s[1] * model->up.y + s[2] * model->at.y + model->pos.y;
    float wz = s[0] * model->right.z + s[1] * model->up.z + s[2] * model->at.z + model->pos.z;
    float z = wx * v->right.z + wy * v->up.z + wz * v->at.z + v->pos.z;
    float scale = sqrtf(model->right.x * model->right.x + model->right.y * model->right.y + model->right.z * model->right.z);
    float sx = sqrtf(v->right.x * v->right.x + v->up.x * v->up.x + v->at.x * v->at.x);
    float r = s[3] * scale;
    if (z - r <= 1)
        return 1;
    float q = r * sx / z;
    float c = 3.14159f * q * q;
    return c > 1 ? 1 : c;
}

static WdList *wd_list(const void *record, int head)
{
    if (!wd_lists && !(wd_lists = calloc(WD_LISTS, sizeof *wd_lists)))
        return 0;
    WdList *free_slot = 0, *old = 0;
    for (int i = 0; i < WD_LISTS; i++) {
        WdList *l = &wd_lists[i];
        if (l->record == record && l->head == head) {
            l->used = cache_frame;
            return l;
        }
        if (!l->record && !free_slot)
            free_slot = l;
        if (l->record && cache_frame - l->used > 64 && (!old || (int)(l->used - old->used) < 0))
            old = l;
    }
    WdList *l = free_slot ? free_slot : old;
    if (!l)
        return 0;
    l->record = record, l->head = head, l->n = 0, l->used = cache_frame; /* the array is kept for the next owner */
    return l;
}

static WdEntry *wd_at(WdList *l, int i, const void *node)
{
    if (i < l->n && l->e[i].node == node)
        return &l->e[i];
    if (i >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        WdEntry *e = memalign(16, cap * sizeof *e);
        if (!e)
            return 0;
        if (l->e)
            memcpy(e, l->e, l->n * sizeof *e);
        free(l->e);
        l->e = e, l->cap = cap;
    }
    l->n = i + 1; /* the rest of the list is learned again */
    memset(&l->e[i], 0, sizeof l->e[i]);
    l->e[i].node = node;
    return &l->e[i];
}

static int wd_global_ok(void)
{
    return p4_world_direct && current_camera && !rtt_target && !shadow_shear_m && !p4_probe_flags && !p4_probe_geometry &&
           !p4_probe_geometry2 && !p4_probe_geometry3 && !p4_probe_geometry4 && !trace_atomics && p4_static_list && p4_state_cache &&
           p4_draw_batch > 1 && !p4_mesh_frustum && !p4_probe_depth_mode && !p4_probe_3d && !p4_hide_material &&
           !p4_gs_test_raw_prelit && !draw_fixed_alpha && !draw_skin_alpha && p4_cull_far <= 0 && p4_cull_cover <= 0 &&
           !p4_tex_census && !p4_cover_stats && !p4_exp_ge_time && !p4_probe_census
#ifdef P4_HW_PROFILE
           && !p4_model_census
#endif
        ;
}

static void wd_pass_keys(void)
{
    if (!wd_pass_valid) {
        wd_pass_state = sl_state_key();
        wd_pass_bits = sl_bits();
        memcpy(wd_snap_render, render_state, sizeof wd_snap_render);
        memcpy(wd_snap_sky, sky_state, sizeof wd_snap_sky);
        wd_snap_mask = pixel_mask;
        wd_pass_valid = 1;
    }
}

/* after a body: the keys stay unless it left the render/Sky state or the mask changed */
static void wd_after_body(void)
{
    if (wd_pass_valid && (wd_snap_mask != pixel_mask || memcmp(wd_snap_render, render_state, sizeof wd_snap_render) ||
                          memcmp(wd_snap_sky, sky_state, sizeof wd_snap_sky)))
        wd_pass_valid = 0;
    wd_cam_ok = 0;
}

static unsigned wd_light(void)
{
    if (!wd_pass_light_valid) {
        SceneLight l = scene_lights();
        wd_pass_light = hash_words(2166136261u, &l, sizeof l);
        wd_pass_light_valid = 1;
    }
    return wd_pass_light;
}

static int wd_same_matrix(const Matrix *a, const Matrix *b)
{
    const unsigned *x = (const void *)a, *y = (const void *)b;
    for (int i = 0; i < 16; i++)
        if ((i & 3) != 3 && x[i] != y[i])
            return 0;
    return 1;
}

/* sl_model's matrix, once at learning */
static void wd_fold(const StaticList *e, const Matrix *model, Matrix *m)
{
    *m = *model;
    if (e->q_scale) {
        const float *c = e->q_centre, s = e->q_scale;
        m->pos.x += m->right.x * c[0] + m->up.x * c[1] + m->at.x * c[2];
        m->pos.y += m->right.y * c[0] + m->up.y * c[1] + m->at.y * c[2];
        m->pos.z += m->right.z * c[0] + m->up.z * c[1] + m->at.z * c[2];
    }
    float s = e->q_scale ? e->q_scale : e->position_scale;
    if (s) {
        m->right.x *= s; m->right.y *= s; m->right.z *= s;
        m->up.x *= s; m->up.y *= s; m->up.z *= s;
        m->at.x *= s; m->at.y *= s; m->at.z *= s;
    }
}

/* the camera as the frustum and guard tests see it: the view, near/far and the six planes (RwCamera +0x94) */
static unsigned wd_camera_key(void)
{
    const char *c = (const char *)current_camera;
    unsigned h = hash_words(2166136261u ^ (unsigned)c, c + CAMERA_VIEW, 64);
    h = hash_words(h, c + CAMERA_NEAR, 8);
    h = hash_words(h, c + 0x94, 6 * 5 * 4);
    return h ^ (unsigned)p4_guard_clip * 0x9e3779b1u ^ (unsigned)guard_code * 0x85ebca6bu;
}

/* the entry's rasters against the ones changed since it was checked (rchg); all of them if the ring lost some */
static int wd_tex_same(WdEntry *w, const StaticList *e)
{
    if (w->texgen == texture_generation)
        return 1;
    if (w->epoch == texture_epoch && !rgen_full && e->ntex != 255 && (e->tex || !e->ntex) &&
        (rchg_n < RCHG || rchg[rchg_n & (RCHG - 1)].gen <= w->texgen + 1)) {
        int hit = 0;
        for (unsigned k = 0; k < RCHG && k < rchg_n && !hit; k++)
            if (rchg[k].gen > w->texgen)
                for (int t = 0; t < e->ntex; t++)
                    hit |= e->tex[t] == rchg[k].r;
        if (!hit) {
            w->texgen = texture_generation;
            return 1;
        }
    }
    if (!sl_tex_same(e, texture_generation))
        return 0;
    w->texgen = texture_generation, w->epoch = texture_epoch;
    return 1;
}

typedef int (*WdRenderState)(int, void *);
extern WdRenderState D_00887300[], D_00887304[];
extern unsigned char D_008872E0[];
extern int iGpffffaf60;
extern float *func_003bfae0(void *atomic); /* RpAtomicGetWorldBoundingSphere */
extern void *func_003c21e0(void *geometry, void *callback, void *data); /* RpGeometryForAllMaterials */
extern int K_Clump_CheckSentinelFrameCb(void *material, int *found);

/* the retail entry body: sphere, frustum, vertex alpha, gate, callback. 1: the callback ran, 0: gate off, -1: outside */
static int wd_body_call(const unsigned char *node, int gate)
{
    void *atomic = *(void *const *)node;
    float *sphere = func_003bfae0(atomic);
    if (!func_003e8200(*(char **)D_008872E0, sphere))
        return -1;
    int vertex_alpha = *(const int *)(node + 4) == 1, saved = 0, ran = 0;
    if (vertex_alpha) {
        D_00887304[0](0xE, &saved);
        D_00887300[0](0xE, 0);
    }
    if ((&iGpffffaf60)[gate] == 1) {
        ((void *(*)(void *))*(void **)((char *)atomic + 0x48))(atomic);
        ran = 1;
    }
    if (vertex_alpha)
        D_00887300[0](0xE, (void *)saved);
    wd_after_body();
    return ran;
}

/* the body for an entry, watched: what it drew is learned when it was one prepared-list call */
static void wd_learn(WdEntry *w, const unsigned char *node, int gate, int why)
{
    void *atomic = *(void *const *)node;
    const Geometry *g = *(Geometry **)((char *)atomic + ATOMIC_GEOMETRY);
    WdEntry old = *w;
    w->kind = WD_NONE, w->vis_known = 0;
    wd_seen = 0, wd_watch = 1;
    unsigned calls = mesh_draw_calls, t0 = sceKernelGetSystemTimeLow();
    int ran = wd_body_call(node, gate);
    unsigned us = sceKernelGetSystemTimeLow() - t0;
    wd_watch = 0;
    wd_why[why] += ran > 0;
    wd_why_us[why] += us;
    if (ran > 0 && g && why == W_GUARD)
        wd_top_note(atomic, g, us, N_N);
    wd_body += ran > 0;
    int n = !wd_global_ok() ? N_OFF : !ran ? N_GATE : -1;
    if (ran < 0 && n < 0 && g && g->morphTarget) { /* outside: keep the sphere, the next frames test it here */
        w->kind = WD_CULL, w->atomic = atomic, w->frame = *(void **)((char *)atomic + ATOMIC_FRAME);
        if (w->frame) {
            w->model = *p4r_003e9700((void *)w->frame);
            memcpy(w->world, func_003bfae0(atomic), sizeof w->world);
            w->camkey = wd_camkey, w->vis = 0, w->vis_known = 1;
        } else
            w->kind = WD_NONE;
        return;
    }
    if (n < 0)
        n = wd_seen == 0 ? N_NONE : wd_seen != 1 ? N_MULTI : !g ? N_NONE :
            *(void **)((char *)atomic + 0x48) != (void *)func_003be9f0 ? N_CB :
            (!wd_got.gate && mesh_draw_calls - calls != wd_got.draws) || !wd_got.model ? N_DRAWS : -1;
    if (n >= 0) {
        wd_not[n] += ran > 0;
        wd_not_us[n] += us;
        if (ran > 0 && g)
            wd_top_note(atomic, g, us, n);
        return;
    }
    const StaticList *e = wd_got.e;
    w->kind = WD_CALL, w->atomic = atomic, w->frame = *(void **)((char *)atomic + ATOMIC_FRAME), w->g = g, w->e = e;
    w->eatom = e->atomic, w->valpha = *(const int *)(node + 4) == 1, w->materials = sl_material_key(g);
    w->gate = wd_got.gate && e->gates && ((const SlGates *)e->gates)->mesh, w->modulate = (g->flags & 0x40) != 0;
    w->list = wd_got.list, w->draws = wd_got.draws, w->built = e->built;
    w->lit = wd_got.lit, w->memo = wd_got.memo, w->flags = g->flags & 0xffff;
    w->model = *wd_got.model;
    wd_fold(e, &w->model, &w->gemodel);
    memcpy(w->sphere, g->morphTarget->sphere, sizeof w->sphere);
    memcpy(w->world, func_003bfae0(atomic), sizeof w->world);
    w->state = wd_got.state, w->bits = wd_got.bits, w->light = w->lit ? wd_light() : 0;
    /* b456: a far stand-in candidate keeps fp_try's decision (its list is the stand-in's when far, or the atomic's
       own when near or when the stand-in was refused); taken again here when the camera or the matrix moved */
    w->fp = p4_far_proxy > 0 && g->numVertices >= p4_far_proxy_min_vertices && g->mesh->numMeshes >= 2;
    if (w->fp) {
        float size = wd_cover(w->sphere, &w->model) * 100000.0f;
        int was = e->atomic == (const char *)atomic + 1;
        w->far = size < (float)p4_far_proxy || (was && size < (float)p4_far_proxy * 1.2f);
    }
    w->texgen = texture_generation, w->epoch = texture_epoch, w->camera = current_camera;
    w->camkey = wd_camkey, w->vis = 1, w->vis_known = 1, w->inside = 1; /* it was drawn whole just now */
    if (old.kind != WD_CALL)
        wd_learned++;
    else if (old.e != w->e || old.list != w->list || old.draws != w->draws || !wd_same_matrix(&old.model, &w->model))
        wd_diff[old.e != w->e ? 0 : old.list != w->list ? 1 : old.draws != w->draws ? 2 : 3]++;
}

/* 1: drawn here; -1: not drawn (outside, as the retail test); 0: the body decides (*why) */
static int wd_direct_emit(WdEntry *w, int *why);
/* b462: a crossing atomic's gated list, drawn here as sl_gate_draw's hit does it (gates, CALL, closed chunks cut) */
static int wd_gate_emit(WdEntry *w, int *why)
{
    StaticList *e = (StaticList *)w->e;
    wd_flush(); /* b481: queued entries before it, in order */
    if (!p4_static_gate || !p4_guard_clip || !guard_code || !e->gates || !((const SlGates *)e->gates)->mesh) {
        *why = W_GUARD;
        return 0;
    }
    if (!list_room())
        return -1;
    prof_begin(PROF_WEMIT);
    cull_atomic_flags = w->flags;
    ib_before(w->g);
    guard_world_planes();
    float near = *(float *)((char *)current_camera + CAMERA_NEAR) * 1.01f;
    const Geometry *g = w->g;
    int r = sl_gate_emit(e, g, &w->model, w->lit ? g->morphTarget->normals : 0, w->modulate, near);
    wd_cam_ok = 0; /* the cut chunks bound their own state */
    wd_pass_valid = 0;
    prof_end(PROF_WEMIT);
    if (!r) {
        *why = W_GUARD;
        return 0;
    }
    e->stamp = cache_frame;
    atomics_drawn++;
    wd_direct++, wd_gated++;
    return 1;
}
static int wd_direct_draw(WdEntry *w, const unsigned char *node, int *why)
{
    if ((*(const int *)(node + 4) == 1) != w->valpha) {
        *why = W_VALPHA;
        return 0;
    }
    prof_begin(PROF_WLTM);
    const Matrix *model = p4r_003e9700((void *)w->frame);
    int same = wd_same_matrix(model, &w->model);
    if (!same && w->kind == WD_CALL) { /* b455: a moving frame (the list is in model space): new matrix and sphere */
        w->model = *model;
        wd_fold(w->e, &w->model, &w->gemodel);
        memcpy(w->world, func_003bfae0((void *)w->atomic), sizeof w->world);
        w->vis_known = 0;
        wd_moved++;
        same = 1;
    }
    prof_end(PROF_WLTM);
    if (!same) {
        *why = W_MATRIX;
        return 0;
    }
    prof_begin(PROF_WVIS);
    if (!w->vis_known || w->camkey != wd_camkey) {
        w->vis = func_003e8200(*(char **)D_008872E0, w->world) != 0;
        w->camkey = wd_camkey, w->vis_known = 1, w->inside = -1;
        if (w->vis && w->kind == WD_CALL && w->fp) { /* fp_try's decision, with its hysteresis */
            float size = wd_cover(w->sphere, &w->model) * 100000.0f;
            int far = size < (float)p4_far_proxy || (w->far && size < (float)p4_far_proxy * 1.2f);
            w->inside = far == w->far ? -1 : -2;
        }
    }
    prof_end(PROF_WVIS);
    if (!w->vis) {
        wd_culled++;
        return -1;
    }
    if (w->kind != WD_CALL) {
        *why = W_VISIBLE;
        return 0;
    }
    if (w->inside == -2) {
        *why = W_FAR;
        return 0;
    }
    const StaticList *e = w->e;
    if (w->camera != current_camera) {
        *why = W_CAMERA;
        return 0;
    }
    if (e->list != w->list || !w->list || e->built != w->built || (w->memo ? e->g != w->g : e->atomic != w->eatom)) {
        *why = W_LIST;
        return 0;
    }
    if (!w->valpha)
        return wd_direct_emit(w, why);
    int saved = 0; /* b456: as the retail loop, vertex alpha off around the draw */
    D_00887304[0](0xE, &saved);
    D_00887300[0](0xE, 0);
    wd_pass_valid = 0;
    int r = wd_direct_emit(w, why);
    D_00887300[0](0xE, (void *)saved);
    wd_pass_valid = 0;
    return r;
}

static int wd_direct_emit(WdEntry *w, int *why)
{
    const StaticList *e = w->e;
    prof_begin(PROF_WKEY);
    wd_pass_keys();
    int r = w->state != wd_pass_state || w->bits != wd_pass_bits ? W_STATE : w->lit && w->light != wd_light() ? W_LIGHT :
            !wd_tex_same(w, e) ? W_TEX : -1;
    prof_end(PROF_WKEY);
    if (r >= 0) {
        *why = r;
        return 0;
    }
    if (w->gate)
        return wd_gate_emit(w, why);
    if (w->inside < 0) {
        prof_begin(PROF_WVIS);
        w->inside = sl_inside(e, &w->model, w->sphere) != 0;
        prof_end(PROF_WVIS);
    }
    if (!w->inside) {
        *why = W_GUARD;
        return 0;
    }
    if (!list_room())
        return -1; /* as the body: nothing drawn */
    if (p4_world_defer && wd_q_list) { /* b481: emitted with the next run of direct entries */
        wd_q[wd_qn++] = (unsigned short)(w - wd_q_list->e);
        if (wd_qn == WD_Q)
            wd_flush();
        return 1;
    }
    wd_emit_now(w);
    return 1;
}

static void wd_emit_now(WdEntry *w)
{
    const StaticList *e = w->e;
    prof_begin(PROF_WEMIT);
    cull_atomic_flags = w->flags;
    ib_before(w->g);
    begin_frame();
    gu_mesh_begin();
    if (!wd_cam_ok) {
        set_camera();
        wd_cam_ok = 1;
    }
    set_matrix(GU_MODEL, &w->gemodel);
    unsigned *l = gu_list->current;
    l[0] = 0x10u << 24 | ((unsigned)w->list >> 8 & 0xf0000); /* BASE */
    l[1] = 0x0Au << 24 | ((unsigned)w->list & 0xffffff);      /* CALL */
    gu_list->current = l + 2;
    sl_forget();
    replay.valid = 0;
    if (!w->memo) { /* as sl_draw's hit: the entry stays, the next instance of its geometry takes the memo */
        ((StaticList *)e)->stamp = cache_frame;
        if (e->atomic == w->atomic) /* not a stand-in's entry (fp_try leaves the memo alone) */
            sl_memo_set(e, w->g, w->lit ? (const V3 *)w->g : 0); /* only tested for null */
    }
    atomics_drawn++;
    wd_direct++;
    mesh_draw_calls += w->draws;
    draws_since_stall += w->draws;
    if (draws_since_stall >= (unsigned)p4_draw_batch && !gu_object_stack_depth)
        stall_flush();
    prof_end(PROF_WEMIT);
}

/* b481: the queued direct entries of this list, in their order; called before any body draws (the order stays
   the retail one) and at the end of the list */
static void wd_flush(void)
{
    if (!wd_qn)
        return;
    prof_begin(PROF_WFLUSH);
    for (int k = 0; k < wd_qn; k++)
        wd_emit_now(&wd_q_list->e[wd_q[k]]);
    wd_qn = 0;
    wd_flushes++;
    prof_end(PROF_WFLUSH);
}

enum { WD_PLAIN, WD_FADE_IN, WD_SENT_NOT1, WD_SENT_0 };
static void wd_head(const unsigned char *record, int head, int gate, int mode, int direct)
{
    const unsigned char *node = ((const unsigned char *const *)record)[head];
    if (!node || (&iGpffffaf60)[gate] != 1) /* gate off: the retail loop draws nothing (its vertex-alpha set is undone) */
        return;
    WdList *l = direct && mode == WD_PLAIN ? wd_list(record, head) : 0;
    wd_q_list = l, wd_qn = 0;
    int check = p4_world_direct_check && !(cache_frame & 63);
    for (int i = 0; node; i++, node = *(const unsigned char *const *)(node + 0x24)) {
        void *atomic = *(void *const *)node;
        if (mode != WD_PLAIN) { /* the fade buckets: their material alpha decides, every frame */
            const void *g = *(void **)((char *)atomic + ATOMIC_GEOMETRY);
            if (!g)
                continue;
            int sentinel = 0;
            func_003c21e0((void *)g, K_Clump_CheckSentinelFrameCb, &sentinel);
            if (mode == WD_FADE_IN ? !(*(const float *)(node + 0x14) >= 1.0f && sentinel != 0) :
                mode == WD_SENT_NOT1 ? sentinel == 1 : sentinel != 0)
                continue;
            wd_flush();
            int ran = wd_body_call(node, gate) > 0;
            wd_why[W_FADE] += ran, wd_body += ran;
            continue;
        }
        WdEntry *w = l ? wd_at(l, i, node) : 0;
        if (!w) {
            wd_flush();
            int ran = wd_body_call(node, gate) > 0;
            wd_why[W_OFF] += ran, wd_body += ran;
            continue;
        }
        int r = 0, why = w->kind == WD_NONE ? W_NEW : W_FORCED;
        /* b460: once in 32 frames the entry's own inputs are read again (callback, frame, geometry, its materials
           and rasters as sl_material_key sees them) instead of running the body: same, it stays direct */
        int recheck = w->kind != WD_NONE && !check && !(((unsigned)i + cache_frame) & 31);
        if (recheck) {
            const char *a = (const char *)w->atomic;
            const Geometry *g = *(Geometry **)(a + ATOMIC_GEOMETRY);
            recheck = a == atomic && *(void **)(a + ATOMIC_FRAME) == w->frame &&
                      (w->kind == WD_CULL || (g == w->g && *(void **)(a + 0x48) == (void *)func_003be9f0 && sl_material_key(g) == w->materials));
            wd_rechecked += recheck;
        }
        if (w->kind != WD_NONE && !check && (recheck || ((unsigned)i + cache_frame) & 31)) {
            prof_begin(PROF_WDIR);
            r = wd_direct_draw(w, node, &why);
            prof_end(PROF_WDIR);
        }
        if (!r) {
            wd_flush();
            wd_learn(w, node, gate, why);
        }
        wd_checks += check;
    }
    wd_flush();
    wd_q_list = 0;
}

typedef struct { signed char set8, head, gate, mode; unsigned blend, test; } WdStep; /* set8: -1 none, else RS 8 */
static const WdStep wd_pass1[] = { {1, 2, -6, WD_PLAIN, 0x44, 0x717FB}, {-1, 5, -6, WD_FADE_IN, 0, 0},
                                   {0, 6, -2, WD_PLAIN, 0x44, 0x717FB}, {1, 7, -1, WD_PLAIN, 0x44, 0x715FB} };
static const WdStep wd_pass2[] = { {0, 8, 0, WD_SENT_NOT1, 0x44, 0x717FB} };
static const WdStep wd_pass3[] = { {1, 3, -5, WD_PLAIN, 0x44, 0x715FB}, {-1, 4, -4, WD_PLAIN, 0x44, 0x735FB},
                                   {0, 5, -3, WD_SENT_0, 0x44, 0x717FB}, {-1, 10, 2, WD_PLAIN, 0x42, 0x71801},
                                   {-1, 9, 1, WD_PLAIN, 0x48, 0x71801} };

static void wd_pass(unsigned char *bucket, const WdStep *step, int n)
{
    const unsigned char *record = *(const unsigned char **)(bucket + 0x1C);
    if (p4_skip_visual_prep(1) || !record)
        return;
    prof_begin(PROF_WPASS);
    int own_state = !(*(const unsigned short *)(bucket + 0x1A) & 2), direct = wd_global_ok();
    if (direct)
        wd_camkey = wd_camera_key();
    wd_pass_light_valid = 0;
    for (int k = 0; k < n; k++, step++) {
        if (step->set8 >= 0 && own_state) {
            D_00887300[0](6, (void *)1);
            D_00887300[0](8, (void *)(int)step->set8);
        }
        if (step->blend) { /* WD_FADE_IN keeps the step before it */
            RpSkyRenderStateSet(2, step->blend);
            RpSkyRenderStateSet(3, step->test);
        }
        wd_pass_valid = 0, wd_cam_ok = 0;
        wd_head(record, step->head, step->gate, step->mode, direct);
    }
    prof_end(PROF_WPASS);
}

/* The three passes replace the game's (psp_link weakens them). With p4_world_direct 0 every entry takes the retail
   body above (wd_body_call), in the retail order and state steps. */
void movieNativeDrawPass1(unsigned char *bucket) { wd_pass(bucket, wd_pass1, sizeof wd_pass1 / sizeof *wd_pass1); }
void movieNativeDrawPass2(unsigned char *bucket) { wd_pass(bucket, wd_pass2, sizeof wd_pass2 / sizeof *wd_pass2); }
void movieNativeDrawPass3(unsigned char *bucket) { wd_pass(bucket, wd_pass3, sizeof wd_pass3 / sizeof *wd_pass3); }

void p4_wd_report(void)
{
    if (p4_world_direct || wd_direct || wd_body)
    { /* three lines: p4_log cuts long ones */
        p4_log("world direct %d check %d per 256 frames: direct %u gated %u rechecked %u moved %u culled %u body %u learned %u checks %u diffs e/list/draws/matrix %u/%u/%u/%u\n",
               p4_world_direct, p4_world_direct_check, wd_direct, wd_gated, wd_rechecked, wd_moved, wd_culled, wd_body, wd_learned, wd_checks, wd_diff[0],
               wd_diff[1], wd_diff[2], wd_diff[3]);
        p4_log("world direct body why: new %u forced %u valpha %u matrix %u visible %u list %u camera %u state %u light %u tex %u guard %u fade %u off %u far %u\n",
               wd_why[W_NEW], wd_why[W_FORCED], wd_why[W_VALPHA], wd_why[W_MATRIX], wd_why[W_VISIBLE], wd_why[W_LIST], wd_why[W_CAMERA],
               wd_why[W_STATE], wd_why[W_LIGHT], wd_why[W_TEX], wd_why[W_GUARD], wd_why[W_FADE], wd_why[W_OFF], wd_why[W_FAR]);
        p4_log("world direct not kept: gate %u none %u multi %u far %u cb %u draws %u valpha %u off %u\n", wd_not[N_GATE], wd_not[N_NONE],
               wd_not[N_MULTI], wd_not[N_FAR], wd_not[N_CB], wd_not[N_DRAWS], wd_not[N_VALPHA], wd_not[N_OFF]);
    }
#ifdef P4_HW_PROFILE
    {
        extern unsigned p4_col_tris, p4_col_queries, p4_col_atoms, p4_col_atris;
        p4_log("collision per 256 frames: world queries %u triangles %u; atomic queries %u triangles %u\n", p4_col_queries, p4_col_tris,
               p4_col_atoms, p4_col_atris);
        p4_col_tris = p4_col_queries = p4_col_atoms = p4_col_atris = 0;
    }
#endif
    if (p4_world_direct) {
        p4_log("world direct body us: forced %u guard %u fade %u; not kept none %u multi %u draws %u cb %u\n", wd_why_us[W_FORCED],
               wd_why_us[W_GUARD], wd_why_us[W_FADE], wd_not_us[N_NONE], wd_not_us[N_MULTI], wd_not_us[N_DRAWS], wd_not_us[N_CB]);
        for (int k = 0; k < WD_TOP; k++)
            if (wd_top[k].g)
                p4_log("world direct top %s: geometry %p v%d meshes %d flags %x calls %u us %u env %d entry %d list %d why %d split %d meshes %d retry %d\n",
                       wd_top[k].why == N_N ? "guard" : wd_top[k].why == N_NONE ? "none" : wd_top[k].why == N_MULTI ? "multi" : "other",
                       (const void *)wd_top[k].g, wd_top[k].nv, wd_top[k].nm, wd_top[k].gflags,
                       wd_top[k].calls, wd_top[k].us, wd_top[k].env, wd_top[k].has, wd_top[k].list, wd_top[k].slwhy,
                       wd_top[k].split, wd_top[k].meshes, wd_top[k].retry);
    }
    memset(wd_top, 0, sizeof wd_top), memset(wd_why_us, 0, sizeof wd_why_us), memset(wd_not_us, 0, sizeof wd_not_us);
    wd_gated = wd_rechecked = wd_direct = wd_moved = wd_culled = wd_body = wd_learned = wd_checks = 0;
    memset(wd_why, 0, sizeof wd_why), memset(wd_not, 0, sizeof wd_not), memset(wd_diff, 0, sizeof wd_diff);
}

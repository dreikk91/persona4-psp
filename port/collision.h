/* p4_col_cull (b466): the field's sphere sweeps without walking atomics that cannot be hit.

   A moving field actor makes, every game tick (drawn and skipped), a floor ray and three sphere sweeps
   (FldFrameTask_Update -> FldFrame_RaycastFieldWorlds -> FldFrame_RaycastFieldObjects). Each sweep runs
   RpClumpForAllAtomics over the collision clump and the field models, and RpAtomicForAllIntersections
   (retail 00394E70, reassembled MIPS) for every atomic. b465 Heaven bridge route: ~117 such calls a drawn frame,
   ~46k cycles each, 0.15 triangles reported a call: ~5 M cycles a frame spent finding nothing.

   Here a sphere query (rpINTERSECTSPHERE) first meets the atomic's geometry box in world space (the model-space box
   of its vertices, through the frame's matrix, kept while the matrix, geometry and vertices stay). A sphere that does
   not reach the box cannot touch a triangle: nothing is reported and RW is not called. Everything else (other query
   types, a sphere reaching the box) goes to the retail body, so the triangles, their order and the callback's
   distances are RW's own.
   b467: a big geometry (COL_GRID_MIN triangles and up) that the sphere reaches gets RW's own walk over a subset: its
   triangles are kept in a world-space XZ grid (from the frame's matrix), the query takes those whose box the sphere
   reaches, in their own order, and RW runs with the geometry's triangle array swapped for them for that one call
   (RW computes every normal, distance and test itself; only triangles it could not report are left out).
   b472: line queries (rpINTERSECTLINE: the floor rays, the field objects' rays) the same way, by the segment.
   p4_col_cull_check 1: the skipped atomics are run through RW anyway and any triangle it reports is counted, and a
   subset call is compared with the full call (the triangles' vertices, in order): diffs. The full call is the one
   that reaches the game then. 0: every query goes to RW, as b465. */
int p4_col_cull = 1, p4_col_cull_check = 0;
static unsigned col_skipped, col_passed, col_built, col_diffs;
static unsigned col_other[7], col_rw_calls, col_subset_calls, col_subset_tris, col_full_tris, col_sub_diffs;

enum { COL_GRID_MIN = 96, COL_GRID = 16, COL_SUB_MAX = 2048 };
typedef struct ColGrid {
    float x0, z0, inv;     /* cell = (x - x0) * inv */
    unsigned short *start; /* COL_GRID * COL_GRID + 1 offsets into idx */
    unsigned short *idx;   /* triangle numbers per cell, ascending */
    float (*box)[6];       /* each triangle's world box */
} ColGrid;
typedef struct {
    const void *atomic, *g, *verts, *tris;
    unsigned m[12];
    float lo[3], hi[3];
    ColGrid *grid;
} ColBox;
enum { COL_BOXES = 1024 };
static ColBox *col_box;

static void *col_check_cb(void *intersection, void *triangle, float distance, void *data)
{
    (*(unsigned *)data)++;
    return triangle;
}

typedef struct { unsigned n; const void *v[64][3]; } ColTrace;
static void *col_trace_cb(void *intersection, void *triangle, float distance, void *data)
{
    ColTrace *t = data;
    if (t->n < 64)
        memcpy(t->v[t->n], (const char *)triangle + 0x1C, sizeof t->v[0]); /* RpCollisionTriangle.vertices */
    t->n++;
    return triangle;
}

static void col_grid_free(ColGrid *gr)
{
    if (gr)
        free(gr->start), free(gr->idx), free(gr->box), free(gr);
}

static int col_cell(float x, float x0, float inv)
{
    int c = (int)((x - x0) * inv);
    return c < 0 ? 0 : c >= COL_GRID ? COL_GRID - 1 : c;
}

/* the geometry's triangles (RpTriangle: three vertex numbers, a material) through the matrix, in an XZ grid */
static ColGrid *col_grid_build(const Geometry *g, const Matrix *m, const ColBox *b)
{
    int nt = g->numTriangles;
    const unsigned short *tri = g->triangles;
    const V3 *v = g->morphTarget->verts;
    if (nt <= 0 || nt > 65535 || !tri)
        return 0;
    ColGrid *gr = calloc(1, sizeof *gr);
    if (!gr)
        return 0;
    float sx = b->hi[0] - b->lo[0], sz = b->hi[2] - b->lo[2], size = sx > sz ? sx : sz;
    gr->x0 = b->lo[0], gr->z0 = b->lo[2], gr->inv = size > 0 ? COL_GRID / size * 0.9999f : 0;
    gr->box = malloc(nt * sizeof *gr->box);
    gr->start = calloc(COL_GRID * COL_GRID + 1, sizeof *gr->start);
    unsigned short *fill = malloc(COL_GRID * COL_GRID * sizeof *fill);
    if (!gr->box || !gr->start || !fill) {
        free(fill), col_grid_free(gr);
        return 0;
    }
    unsigned total = 0;
    for (int t = 0; t < nt; t++) {
        float *bx = gr->box[t];
        bx[0] = bx[1] = bx[2] = 1e30f, bx[3] = bx[4] = bx[5] = -1e30f;
        for (int k = 0; k < 3; k++) {
            const V3 *p = &v[tri[t * 4 + k]];
            for (int a = 0; a < 3; a++) {
                float w = p->x * (&m->right.x)[a] + p->y * (&m->up.x)[a] + p->z * (&m->at.x)[a] + (&m->pos.x)[a];
                if (w < bx[a]) bx[a] = w;
                if (w > bx[a + 3]) bx[a + 3] = w;
            }
        }
        for (int a = 0; a < 3; a++) { /* rounding margin */
            float e = (fabsf(bx[a]) + fabsf(bx[a + 3])) * 0.0001f + 0.01f;
            bx[a] -= e, bx[a + 3] += e;
        }
        int c0 = col_cell(bx[0], gr->x0, gr->inv), c1 = col_cell(bx[3], gr->x0, gr->inv);
        int r0 = col_cell(bx[2], gr->z0, gr->inv), r1 = col_cell(bx[5], gr->z0, gr->inv);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
                gr->start[r * COL_GRID + c]++, total++;
    }
    if (total > 65535 || !(gr->idx = malloc((total ? total : 1) * sizeof *gr->idx))) {
        free(fill), col_grid_free(gr);
        return 0;
    }
    unsigned at = 0;
    for (int c = 0; c < COL_GRID * COL_GRID; c++) {
        unsigned n = gr->start[c];
        gr->start[c] = fill[c] = at;
        at += n;
    }
    gr->start[COL_GRID * COL_GRID] = at;
    for (int t = 0; t < nt; t++) { /* ascending in every cell */
        const float *bx = gr->box[t];
        int c0 = col_cell(bx[0], gr->x0, gr->inv), c1 = col_cell(bx[3], gr->x0, gr->inv);
        int r0 = col_cell(bx[2], gr->z0, gr->inv), r1 = col_cell(bx[5], gr->z0, gr->inv);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
                gr->idx[fill[r * COL_GRID + c]++] = t;
    }
    free(fill);
    return gr;
}

void *p4r_00394e70(void *atomic, void *intersection, void *callback, void *data);

/* b468: 1 when the triangle's nearest point (world space, through the kept matrix) is farther than r from the
   sphere's centre: RW cannot report it (r carries the margin, so a triangle at the edge stays RW's to decide) */
static int col_tri_far(const ColBox *b, const Geometry *g, int t, const float *c, float r)
{
    const unsigned short *tri = (const unsigned short *)g->triangles + t * 4;
    const float *m = (const float *)b->m; /* right, up, at, pos */
    float p[3][3];
    for (int k = 0; k < 3; k++) {
        const V3 *v = &g->morphTarget->verts[tri[k]];
        for (int a = 0; a < 3; a++)
            p[k][a] = v->x * m[a] + v->y * m[3 + a] + v->z * m[6 + a] + m[9 + a];
    }
    /* the nearest point on the triangle (Ericson, Real-Time Collision Detection 5.1.5) */
    float ab[3], ac[3], ap[3], q[3];
    for (int a = 0; a < 3; a++)
        ab[a] = p[1][a] - p[0][a], ac[a] = p[2][a] - p[0][a], ap[a] = c[a] - p[0][a];
    float d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2], d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
    if (d1 <= 0 && d2 <= 0) {
        memcpy(q, p[0], sizeof q);
    } else {
        float bp[3], cp[3];
        for (int a = 0; a < 3; a++)
            bp[a] = c[a] - p[1][a], cp[a] = c[a] - p[2][a];
        float d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2], d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
        float d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2], d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
        float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0 && d4 <= d3) {
            memcpy(q, p[1], sizeof q);
        } else if (d6 >= 0 && d5 <= d6) {
            memcpy(q, p[2], sizeof q);
        } else if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            float v = d1 / (d1 - d3);
            for (int a = 0; a < 3; a++) q[a] = p[0][a] + v * ab[a];
        } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            float w = d2 / (d2 - d6);
            for (int a = 0; a < 3; a++) q[a] = p[0][a] + w * ac[a];
        } else if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            for (int a = 0; a < 3; a++) q[a] = p[1][a] + w * (p[2][a] - p[1][a]);
        } else {
            float den = va + vb + vc;
            if (!(den > 0))
                return 0; /* degenerate: RW decides */
            float v = vb / den, w = vc / den;
            for (int a = 0; a < 3; a++) q[a] = p[0][a] + ab[a] * v + ac[a] * w;
        }
    }
    float dx = c[0] - q[0], dy = c[1] - q[1], dz = c[2] - q[2];
    return dx * dx + dy * dy + dz * dz > r * r;
}

/* RW over the triangles the sphere's box reaches, the geometry's own array swapped for that call. 0: not done */
/* b472: 1 when the segment a->b meets the box lo..hi (slabs) */
static int col_seg_box(const float *a, const float *b, const float *lo, const float *hi)
{
    float t0 = 0, t1 = 1;
    for (int k = 0; k < 3; k++) {
        float d = b[k] - a[k];
        if (d == 0) {
            if (a[k] < lo[k] || a[k] > hi[k])
                return 0;
            continue;
        }
        float u = (lo[k] - a[k]) / d, v = (hi[k] - a[k]) / d;
        if (u > v) {
            float w = u;
            u = v, v = w;
        }
        if (u > t0) t0 = u;
        if (v < t1) t1 = v;
        if (t0 > t1)
            return 0;
    }
    return 1;
}

static int col_subset(const ColBox *b, Geometry *g, void *atomic, void *intersection, void *callback, void *data)
{
    int line = *(const int *)((const char *)intersection + 24) == 1; /* b472: RwLine start, end; else RwSphere */
    /* b517: in the image's VU block (vu_place.h) */
    _Static_assert(COL_SUB_MAX * 4 * 2 <= VU_COL_SUB_SIZE && COL_SUB_MAX * 2 <= VU_COL_PICK_SIZE, "vu_place.h: sub/pick grew");
    unsigned short *sub = &VU_PLACE(unsigned short, VU_COL_SUB), *pick = &VU_PLACE(unsigned short, VU_COL_PICK);
    static unsigned char *seen;
    static int seen_n;
    const ColGrid *gr = b->grid;
    const float *s = (const float *)intersection;
    float r = line ? 0.01f : s[3] * 1.001f + 0.01f, qlo[3], qhi[3];
    for (int a = 0; a < 3; a++) {
        float x0 = s[a], x1 = line ? s[3 + a] : s[a];
        qlo[a] = (x0 < x1 ? x0 : x1) - r, qhi[a] = (x0 < x1 ? x1 : x0) + r;
    }
    if (!gr)
        return 0;
    if (seen_n < g->numTriangles) {
        free(seen);
        seen_n = g->numTriangles;
        if (!(seen = calloc((seen_n + 7) / 8, 1))) {
            seen_n = 0;
            return 0;
        }
    }
    int c0 = col_cell(qlo[0], gr->x0, gr->inv), c1 = col_cell(qhi[0], gr->x0, gr->inv);
    int r0 = col_cell(qlo[2], gr->z0, gr->inv), r1 = col_cell(qhi[2], gr->z0, gr->inv);
    int n = 0;
    for (int row = r0; row <= r1; row++)
        for (int c = c0; c <= c1; c++)
            for (unsigned i = gr->start[row * COL_GRID + c]; i < gr->start[row * COL_GRID + c + 1]; i++) {
                int t = gr->idx[i];
                if (seen[t >> 3] & 1 << (t & 7))
                    continue;
                const float *bx = gr->box[t];
                if (qhi[0] < bx[0] || qlo[0] > bx[3] || qhi[1] < bx[1] || qlo[1] > bx[4] || qhi[2] < bx[2] || qlo[2] > bx[5])
                    continue;
                if (line ? !col_seg_box(s, s + 3, bx, bx + 3) : col_tri_far(b, g, t, s, r))
                    continue;
                if (n == COL_SUB_MAX) { /* too many: clear the marks, RW takes the whole geometry */
                    for (int k = 0; k < n; k++)
                        seen[pick[k] >> 3] &= ~(1 << (pick[k] & 7));
                    return 0;
                }
                seen[t >> 3] |= 1 << (t & 7);
                pick[n++] = t;
            }
    for (int i = 1; i < n; i++) { /* RW's order: ascending triangle numbers (several cells merge) */
        unsigned short t = pick[i];
        int j = i;
        for (; j > 0 && pick[j - 1] > t; j--)
            pick[j] = pick[j - 1];
        pick[j] = t;
    }
    const unsigned short *tri = g->triangles;
    for (int i = 0; i < n; i++) {
        memcpy(&sub[i * 4], &tri[pick[i] * 4], 8);
        seen[pick[i] >> 3] &= ~(1 << (pick[i] & 7));
    }
    col_subset_calls++, col_subset_tris += n, col_full_tris += g->numTriangles;
    if (!n)
        return 1; /* no triangle's box is reached: RW would report none */
    void *saved = g->triangles;
    int saved_n = g->numTriangles;
    g->triangles = sub, g->numTriangles = n;
    col_rw_calls++;
    prof_begin(PROF_CRW);
    p4r_00394e70(atomic, intersection, callback, data);
    prof_end(PROF_CRW);
    g->triangles = saved, g->numTriangles = saved_n;
    return 1;
}

void *func_00394e70(void *atomic, void *intersection, void *callback, void *data)
{
    int type = *(const int *)((const char *)intersection + 24); /* RpIntersection.type: 3 rpINTERSECTSPHERE */
    if (!p4_col_cull || (type != 3 && type != 1)) {
        col_other[type == 1 ? 0 : 1]++;
        return p4r_00394e70(atomic, intersection, callback, data);
    }
    Geometry *g = *(Geometry **)((char *)atomic + ATOMIC_GEOMETRY);
    char *frame = *(char **)((char *)atomic + ATOMIC_FRAME);
    if (!g || !frame || !g->morphTarget || !g->morphTarget->verts || g->numVertices <= 0 ||
        (!col_box && !(col_box = calloc(COL_BOXES, sizeof *col_box)))) {
        col_other[!g ? 2 : !frame ? 3 : !g->morphTarget ? 4 : !g->morphTarget->verts ? 5 : 6]++;
        return p4r_00394e70(atomic, intersection, callback, data);
    }
    prof_begin(PROF_CBOX);
    const Matrix *m = p4r_003e9700(frame);
    unsigned words[12];
    memcpy(words, &m->right, 12), memcpy(words + 3, &m->up, 12), memcpy(words + 6, &m->at, 12), memcpy(words + 9, &m->pos, 12);
    ColBox *b = &col_box[((unsigned)atomic >> 4) * 2654435761u >> 22];
    if (b->atomic != atomic || b->g != g || b->verts != g->morphTarget->verts || b->tris != g->triangles ||
        memcmp(b->m, words, sizeof words)) {
        col_grid_free(b->grid), b->grid = 0;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        const V3 *v = g->morphTarget->verts;
        for (int i = 0; i < g->numVertices; i++) {
            const float p[3] = { v[i].x, v[i].y, v[i].z };
            for (int a = 0; a < 3; a++) {
                if (p[a] < lo[a]) lo[a] = p[a];
                if (p[a] > hi[a]) hi[a] = p[a];
            }
        }
        /* the box through the matrix: centre moved, half extents through |M| */
        const float c[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
        const float e[3] = { (hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f };
        const float *r = &m->right.x, *u = &m->up.x, *t = &m->at.x, *p = &m->pos.x;
        for (int a = 0; a < 3; a++) {
            float wc = c[0] * r[a] + c[1] * u[a] + c[2] * t[a] + p[a];
            float we = e[0] * fabsf(r[a]) + e[1] * fabsf(u[a]) + e[2] * fabsf(t[a]);
            we += (fabsf(wc) + we) * 0.0001f + 0.01f; /* rounding margin */
            b->lo[a] = wc - we, b->hi[a] = wc + we;
        }
        b->atomic = atomic, b->g = g, b->verts = g->morphTarget->verts, b->tris = g->triangles;
        memcpy(b->m, words, sizeof words);
        if (g->numTriangles >= COL_GRID_MIN)
            b->grid = col_grid_build(g, m, b);
        col_built++;
    }
    const float *s = (const float *)intersection; /* RwSphere: centre, radius; RwLine: start, end */
    float d2 = 0;
    if (type == 1)
        d2 = col_seg_box(s, s + 3, b->lo, b->hi) ? 0 : 1e30f;
    else
    for (int a = 0; a < 3; a++) {
        float d = s[a] < b->lo[a] ? b->lo[a] - s[a] : s[a] > b->hi[a] ? s[a] - b->hi[a] : 0;
        d2 += d * d;
    }
    float reach = type == 1 ? 0 : s[3] * 1.001f + 0.01f;
    prof_end(PROF_CBOX);
    if (d2 > reach * reach) {
        col_skipped++;
        if (p4_col_cull_check) {
            unsigned found = 0;
            p4r_00394e70(atomic, intersection, (void *)col_check_cb, &found);
            col_diffs += found;
        }
        return atomic;
    }
    col_passed++;
    if (b->grid) {
        if (p4_col_cull_check) { /* the full call reaches the game; the subset's triangles are compared with it */
            static ColTrace full, part;
            full.n = part.n = 0;
            p4r_00394e70(atomic, intersection, (void *)col_trace_cb, &full);
            if (col_subset(b, g, atomic, intersection, (void *)col_trace_cb, &part) &&
                (full.n != part.n || memcmp(full.v, part.v, (full.n < 64 ? full.n : 64) * sizeof full.v[0])))
                col_sub_diffs++;
            return p4r_00394e70(atomic, intersection, callback, data);
        }
        prof_begin(PROF_CGRID);
        int done = col_subset(b, g, atomic, intersection, callback, data);
        prof_end(PROF_CGRID);
        if (done)
            return atomic;
    }
    return p4r_00394e70(atomic, intersection, callback, data);
}

void p4_col_report(void)
{
    if (col_skipped || col_passed)
        p4_log("collision cull %d check %d per 256 frames: skipped %u passed %u boxes built %u diffs %u; subset calls %u (RW %u) triangles %u of %u diffs %u\n",
               p4_col_cull, p4_col_cull_check, col_skipped, col_passed, col_built, col_diffs, col_subset_calls, col_rw_calls, col_subset_tris,
               col_full_tris, col_sub_diffs);
    if (col_other[0] + col_other[1] + col_other[2] + col_other[3] + col_other[4] + col_other[5] + col_other[6])
        p4_log("collision to RW whole: line %u other type %u; sphere without geometry %u frame %u morph %u verts %u count %u\n", col_other[0],
               col_other[1], col_other[2], col_other[3], col_other[4], col_other[5], col_other[6]);
    memset(col_other, 0, sizeof col_other);
    col_skipped = col_passed = col_built = col_diffs = col_subset_calls = col_subset_tris = col_full_tris = col_sub_diffs = col_rw_calls = 0;
}

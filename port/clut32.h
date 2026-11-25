/* b370 p4_clut32: CLUT8 copies of big 32-bit textures, for the GE's texture fetch (1 byte a texel instead of 4).
   b360 Margaret dialogue, GE timer: the full-screen Heaven cloud plane f028_002_cloud02 (256x256, 32-bit, blended)
   ~2.4 ms of GE a frame. A native 32-bit texture of at least p4_clut32_min texels gets, when it is loaded, a CLUT8
   copy (median cut to 256 RGBA entries, each the mean of its texels) the first time it is bound with p4_clut32 on
   (not at load: the cloud plane's raster does not come through the native texture read); the 32-bit original stays,
   so p4_clut32 is a live A/B: 1 binds the copy, 0 the original (the switch change remakes the prepared lists). Lossy:
   compare the picture. A lock moves the original's pixels: that raster then keeps its original for good. Making a
   copy takes ~0.4 s (256x256): a hitch once. Log: "clut32 made WxH colours N in T ms", "clut32 binds" a window. */
/* b375: on by default for every 32-bit texture of at least 32x32 (smaller ones fit the GE's texture cache anyway).
   A raster the game writes in place (tmxDmaCopy) keeps its original from then on: no copy is made again. */
/* b388: every 32-bit texture by default (p4_clut32_min 0; 1024 = the b375 rule). Up to 16x16 the copy is exact (a colour
   a texel); its 1 KB palette is larger than such a texture. 256 slots found by a hash of the raster and at most
   CLUT32_PROBE probes, so a bind does not walk every slot. */
int p4_clut32 = 1, p4_clut32_build = 1, p4_clut32_min = 0;
#include "paths.h"
int p4_clut_pack = 0;
enum { CLUT32_SLOTS = 256, CLUT32_PROBE = 8 };
struct SwzJob;
typedef struct { const Raster *of; const unsigned char *pixels; Raster copy; unsigned short pw, ph; struct SwzJob *job; } Clut32; /* pw, ph: b491 prescaled image size; job: b495 prescale in progress */
static void swz_job_free(struct SwzJob *j);
static Clut32 *clut32_slots; /* heap: .bss must end below the data image */
static unsigned clut32_binds, clut32_made, clut32_full, clut32_stale, clut32_ms;

typedef struct { unsigned hash, check, w, h, offset; } ClutPackEntry;
static ClutPackEntry *clut_pack_index;
static unsigned clut_pack_count, clut_pack_start;
static int clut_pack_tried;

static void clut32_install(const Raster *r, Clut32 *slot, int depth, unsigned char *indices, unsigned char *palette)
{
    Raster copy = *r;
    copy.depth = depth;
    copy.stride = (r->width * depth + 7) / 8;
    copy.originalPixels = copy.cpPixels = indices;
    copy.palette = palette;
    sceKernelDcacheWritebackRange(palette, 4 << depth);
    swizzle_to_arena = 1;
    swizzle_raster(&copy);
    swizzle_to_arena = 0;
    if (!swizzle_head(copy.originalPixels))
        sceKernelDcacheWritebackRange(copy.originalPixels, copy.stride * copy.height);
    slot->of = r, slot->pixels = r->originalPixels, slot->copy = copy;
}

static int clut_pack_read(const Raster *r, const unsigned char *rows, Clut32 *slot)
{
    if (!clut_pack_tried) {
        clut_pack_tried = 1;
        unsigned head[4];
        SceUID fd = sceIoOpen(p4_path("p4clut.bin"), PSP_O_RDONLY, 0);
        if (fd >= 0) {
            if (sceIoRead(fd, head, sizeof head) == sizeof head && head[0] == 0x4c433450 && head[1] == 2 &&
                head[2] <= 65536 && head[3] == 16 + head[2] * sizeof(ClutPackEntry)) {
                int bytes = head[2] * sizeof(ClutPackEntry);
                clut_pack_index = p4a_malloc(bytes);
                if (clut_pack_index && sceIoRead(fd, clut_pack_index, bytes) == bytes)
                    clut_pack_count = head[2], clut_pack_start = head[3];
                else
                    p4a_free(clut_pack_index), clut_pack_index = 0;
            }
            sceIoClose(fd);
        }
        p4_log("clut pack: %u entries\n", clut_pack_count);
    }
    if (!clut_pack_count)
        return 0;
    unsigned hash = 2166136261u, check = 5381;
    for (int y = 0; y < r->height; y++)
        for (int x = 0; x < r->width * 4; x++) {
            unsigned byte = rows[y * r->stride + x];
            hash = (hash ^ byte) * 16777619u;
            check = (check * 33) ^ byte;
        }
    unsigned lo = 0, hi = clut_pack_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (clut_pack_index[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < clut_pack_count && clut_pack_index[lo].hash == hash; lo++) {
        const ClutPackEntry *e = clut_pack_index + lo;
        if (e->check != check || e->w != r->width || e->h != r->height || e->offset < clut_pack_start || e->offset > 0x7ff00000u)
            continue;
        SceUID fd = sceIoOpen(p4_path("p4clut.bin"), PSP_O_RDONLY, 0);
        if (fd < 0)
            return 0;
        unsigned depth = 0;
        unsigned char *indices = 0, *palette = 0;
        int ok = sceIoLseek32(fd, e->offset, PSP_SEEK_SET) == (int)e->offset &&
            sceIoRead(fd, &depth, sizeof depth) == sizeof depth && (depth == 8 || (depth == 4 && !(r->width & 1)));
        if (ok) {
            int n = r->width * r->height * depth / 8, p = 4 << depth;
            indices = p4a_memalign(64, n);
            palette = p4a_memalign(64, p);
            ok = indices && palette && sceIoRead(fd, palette, p) == p && sceIoRead(fd, indices, n) == n;
        }
        sceIoClose(fd);
        if (ok) {
            clut32_install(r, slot, depth, indices, palette);
            p4_log("clut pack hit %dx%d T%u\n", r->width, r->height, depth);
            return 1;
        }
        p4a_free(indices), p4a_free(palette);
        return 0;
    }
    return 0;
}

static void clut32_free_copy(Clut32 *s)
{
    if (!s->of)
        return;
    swz_job_free(s->job); /* b495: CPU-only buffers, the GE never read them */
    unsigned char *p = s->copy.originalPixels;
    ge_free_later(swizzle_head(p) ? p - SWIZZLE_HEAD : p); /* b494: the list in flight may still read it */
    if (s->copy.palette != s->of->palette) /* b484: a swizzled copy shares the raster's own palette */
        ge_free_later(s->copy.palette);
    memset(s, 0, sizeof *s);
}

/* GE idle (p4_video_renderer_begin): every copy freed; made again at its next bind. b375 */
static unsigned clut32_release_all(void)
{
    unsigned bytes = 0;
    for (int i = 0; clut32_slots && i < CLUT32_SLOTS; i++)
        if (clut32_slots[i].of) {
            const Raster *copy = &clut32_slots[i].copy;
            if (clut32_slots[i].job) /* b495: a prescale in progress starts again at the next bind */
                clut32_free_copy(&clut32_slots[i]);
            else if (copy->originalPixels) {
                bytes += copy->stride * copy->height + (4 << copy->depth);
                clut32_free_copy(&clut32_slots[i]);
            }
        }
    return bytes;
}

/* the game copies texels into raster memory directly (tmxDmaCopy): a copy of that raster is stale; the raster keeps
   its original for good (an entry without a copy), so a raster written every frame is not converted every frame */
static void clut32_written(const unsigned char *at, unsigned bytes)
{
    for (int i = 0; clut32_slots && i < CLUT32_SLOTS; i++) {
        Clut32 *s = &clut32_slots[i];
        if (!s->of || !s->copy.originalPixels)
            continue;
        const unsigned char *p = s->pixels;
        unsigned size = (unsigned)s->of->stride * s->of->height * 2 + 64; /* level 0 and its mips, rounded up */
        if (at < p + size && at + bytes > p - 64) {
            const Raster *r = s->of;
            const unsigned char *pixels = s->pixels;
            clut32_free_copy(s);
            s->of = r, s->pixels = pixels; /* refusal: copy stays 0 */
            clut32_stale++;
        }
    }
}

/* b486: the game locks the raster's texels (RwRasterLock): a CLUT8 or swizzled copy of it is stale from now on, and
   a rows raster keeps its pixel pointer through a lock, so the copy would never be noticed as old (b484: the battle's
   HP gauges, redrawn into their texture, stayed empty). The raster keeps its original for good, as after a write. */
static void clut32_locked(const Raster *r)
{
    for (int i = 0; clut32_slots && i < CLUT32_SLOTS; i++) {
        Clut32 *s = &clut32_slots[i];
        if (s->of == r && s->copy.originalPixels) {
            const unsigned char *pixels = s->pixels;
            clut32_free_copy(s);
            s->of = r, s->pixels = pixels; /* refusal: copy stays 0 */
            clut32_stale++;
        }
    }
}

static void clut32_drop(const Raster *r)
{
    for (int i = 0; clut32_slots && i < CLUT32_SLOTS; i++)
        if (clut32_slots[i].of == r)
            clut32_free_copy(&clut32_slots[i]);
}

/* members [lo, hi) of the box: per channel min/max. The colours are kept sorted in place with their texel positions
   (two arrays moved together): every pass reads in order, no scattered loads (b373: 0.4 s a 256x256 texture). */
typedef struct { int lo, hi; unsigned char min[4], max[4]; } ClutBox;
static void clut32_box_range(ClutBox *b, const unsigned *col)
{
    unsigned char mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 };
    for (int i = b->lo; i < b->hi; i++) {
        unsigned c = col[i];
        for (int k = 0; k < 4; k++) {
            unsigned v = c >> (k * 8) & 255;
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }
    memcpy(b->min, mn, 4), memcpy(b->max, mx, 4);
}

/* rows: the 32-bit texels in rows (stride r->stride), alpha already 0..255 */
static void clut32_make(const Raster *r, const unsigned char *rows, Clut32 *slot)
{
    unsigned t0 = sceKernelGetSystemTimeLow();
    int w = r->width, h = r->height, n = w * h;
    unsigned *col = p4a_malloc(n * 4), *tmp = p4a_malloc(n * 4);
    unsigned short *pos = p4a_malloc(n * 2), *tpos = p4a_malloc(n * 2); /* n <= 512 x 512: positions need 18 bits */
    unsigned *pos_hi = 0;
    ClutBox *box = p4a_malloc(256 * sizeof *box);
    unsigned char *indices = p4a_memalign(64, n);
    unsigned *palette = p4a_memalign(64, 256 * 4);
    if (n > 65536)
        p4a_free(pos), p4a_free(tpos), pos = tpos = 0, pos_hi = p4a_malloc(n * 8);
    if (!col || !tmp || !(pos_hi || (pos && tpos)) || !box || !indices || !palette) {
        p4a_free(col), p4a_free(tmp), p4a_free(pos), p4a_free(tpos), p4a_free(pos_hi), p4a_free(box), p4a_free(indices), p4a_free(palette);
        return;
    }
    unsigned *pos32 = pos_hi, *tpos32 = pos_hi ? pos_hi + n : 0;
    for (int y = 0; y < h; y++)
        memcpy(col + y * w, rows + y * r->stride, w * 4);
    for (int i = 0; i < n; i++)
        pos32 ? (void)(pos32[i] = i) : (void)(pos[i] = (unsigned short)i);
    int boxes = 1;
    box[0].lo = 0, box[0].hi = n;
    clut32_box_range(&box[0], col);
    while (boxes < 256) {
        int best = -1, best_range = 0, channel = 0;
        for (int b = 0; b < boxes; b++)
            for (int k = 0; k < 4; k++) {
                int range = box[b].max[k] - box[b].min[k];
                if (box[b].hi - box[b].lo > 1 && range > best_range)
                    best = b, best_range = range, channel = k;
            }
        if (best < 0)
            break; /* every box holds one colour */
        ClutBox *b = &box[best];
        int shift = channel * 8, lo = b->lo, m = b->hi - b->lo;
        /* counting sort of the box by the channel, colours and positions together, then cut at the median */
        unsigned count[257] = { 0 };
        for (int i = lo; i < b->hi; i++)
            count[(col[i] >> shift & 255) + 1]++;
        for (int v = 0; v < 256; v++)
            count[v + 1] += count[v];
        for (int i = lo; i < b->hi; i++) {
            unsigned at = count[col[i] >> shift & 255]++;
            tmp[at] = col[i];
            if (pos32) tpos32[at] = pos32[i]; else tpos[at] = pos[i];
        }
        memcpy(col + lo, tmp, m * 4);
        if (pos32) memcpy(pos32 + lo, tpos32, m * 4); else memcpy(pos + lo, tpos, m * 2);
        int mid = lo + m / 2;
        /* keep equal values together where possible, so a cut falls between two values */
        unsigned at = col[mid] >> shift & 255;
        int up = mid, down = mid;
        while (up < b->hi && (col[up] >> shift & 255) == at) up++;
        while (down > lo && (col[down - 1] >> shift & 255) == at) down--;
        mid = down > lo && (mid - down <= up - mid || up >= b->hi) ? down : up;
        if (mid <= lo || mid >= b->hi)
            mid = lo + m / 2;
        box[boxes].lo = mid, box[boxes].hi = b->hi;
        b->hi = mid;
        clut32_box_range(b, col);
        clut32_box_range(&box[boxes], col);
        boxes++;
    }
    memset(palette, 0, 256 * 4);
    for (int b = 0; b < boxes; b++) {
        unsigned sum[4] = { 0, 0, 0, 0 }, m = box[b].hi - box[b].lo;
        for (int i = box[b].lo; i < box[b].hi; i++) {
            unsigned c = col[i];
            sum[0] += c & 255, sum[1] += c >> 8 & 255, sum[2] += c >> 16 & 255, sum[3] += c >> 24;
            indices[pos32 ? pos32[i] : pos[i]] = (unsigned char)b;
        }
        palette[b] = (sum[0] + m / 2) / m | (sum[1] + m / 2) / m << 8 | (sum[2] + m / 2) / m << 16 | (sum[3] + m / 2) / m << 24;
    }
    p4a_free(col), p4a_free(tmp), p4a_free(pos), p4a_free(tpos), p4a_free(pos_hi), p4a_free(box);
    clut32_install(r, slot, 8, indices, (unsigned char *)palette);
    clut32_made++;
    unsigned ms = (sceKernelGetSystemTimeLow() - t0) / 1000;
    clut32_ms += ms;
    if (w * h >= 128 * 128)
        p4_log("clut32 made %dx%d colours %d in %u ms\n", w, h, boxes, ms);
}

static Raster *clut32_find(Raster *r)
{
    if (r->width * r->height < p4_clut32_min || r->width > 512 || r->height > 512 || !r->originalPixels)
        return r;
    if (!clut32_slots && !(clut32_slots = p4a_calloc(CLUT32_SLOTS, sizeof *clut32_slots)))
        return r;
    Clut32 *free_slot = 0;
    unsigned home = ((unsigned)r >> 4) * 2654435761u >> 24; /* 8 bits: CLUT32_SLOTS 256 */
    for (int k = 0; k < CLUT32_PROBE; k++) {
        Clut32 *s = &clut32_slots[(home + k) & (CLUT32_SLOTS - 1)];
        if (s->of == r) {
            if (s->pixels != r->originalPixels || !s->copy.originalPixels)
                return r; /* locked or changed since, or no memory for it: the original */
            clut32_binds++;
            return &s->copy;
        }
        if (!s->of && !free_slot)
            free_slot = s;
    }
    if (!free_slot || !p4_clut32_build) {
        clut32_full += !free_slot;
        return r;
    }
    const SwizzleHead *h = swizzle_head(r->originalPixels);
    unsigned char *linear = 0;
    if (h) {
        if (!(linear = p4a_memalign(64, r->stride * r->height)))
            return r;
        ge_swizzle(r->originalPixels, linear, r->stride, r->height, h->wbytes, h->rows, 0);
    }
    const unsigned char *rows = linear ? linear : r->originalPixels;
    if (!p4_clut_pack || !clut_pack_read(r, rows, free_slot))
        clut32_make(r, rows, free_slot);
    p4a_free(linear);
    if (!free_slot->of) /* no memory: remember the refusal */
        free_slot->of = r, free_slot->pixels = r->originalPixels;
    return clut32_find(r);
}

/* b484 p4_swz_copy: swizzled copies of indexed (4/8-bit) textures the game writes itself and so never swizzles (TMX
   images: the title's 512x512 and 256x256 CLUT8 layers, sampled as rows: b483 title GE ~55 ms a frame, its 2D draws
   9.5 ms for 0.67 of a screen from one 512x512 image). The copy keeps the raster's texels in the GE's block order
   (and mip levels, as swizzle_raster makes them) and the raster's own palette; it is made at the first bind and kept
   in the clut32 slots, so a texel write into the raster (tmxDmaCopy, a lock) drops it for good, as with clut32.
   0: the rows, as before. */
int p4_swz_copy = 1;
static unsigned swz_made, swz_binds;
/* b491 p4_swz_prescale: an 8-bit copy is resampled once to the size the PSP shows it at: the game draws 2D for the PS2's
   640x448, the PSP screen is 480x272, so an image drawn 1:1 there lands at 0.75 x 0.607 of its texels here. The copy
   holds the image at that size (each texel the area average of the texels it covers, mapped back to the nearest entry
   of the raster's own palette), top-left in the same power-of-two buffer, the last row and column repeated past it.
   2D draws map their UVs onto that part (texture_w/h), so the GE reads about 1:1: sharp like level 0 (b490's levels
   blurred the battle menu), and about half the texels (the title stays fast without levels). 0: as b490. */
int p4_swz_prescale = 1;
/* b492: only images at least this big both ways are prescaled (the title's full-screen layers); text and menu sheets
   lost their letters' strokes to the area average (b491 save/menu/command lists), so smaller ones keep level 0 */
int p4_swz_prescale_min = 256;
static unsigned swz_prescaled;
/* b495 (issue 22): the nearest of n candidate palette entries cand[] (the 4x4 source texels around the output texel's
   centre: its footprint and the shades next to it) to an RGBA colour. b493 searched all 256 entries behind a memo per
   palette: ~0.2-0.3 s for one dialogue portrait, a hitch on every portrait that appeared. */
static int swz_nearest(const unsigned *pal, const unsigned char *cand, int n, unsigned r, unsigned g, unsigned b, unsigned a)
{
    int best = cand[0];
    unsigned best_d = ~0u;
    for (int j = 0; j < n; j++) {
        int i = cand[j];
        unsigned c = pal[i];
        int ca = c >> 24, dr = (int)(c & 255) - (int)r, dg = (int)(c >> 8 & 255) - (int)g, db = (int)(c >> 16 & 255) - (int)b,
            da = ca - (int)a;
        /* colour matters as much as it shows: weighted by the brighter alpha; alpha itself counts double */
        int vis = (ca > (int)a ? ca : (int)a) + 8;
        unsigned d = (unsigned)((2 * dr * dr + 4 * dg * dg + db * db) * vis >> 8) + 6u * da * da;
        if (d < best_d)
            best_d = d, best = i;
    }
    return best;
}
static float swz_lanczos2(float t)
{
    if (t < 0) t = -t;
    if (t < 1e-4f) return 1;
    if (t >= 2) return 0;
    float pt = 3.14159265f * t;
    return 2 * sinf(pt) * sinf(pt * 0.5f) / (pt * pt);
}
enum { SWZ_TAPS = 8, SWZ_RING = 16 };
/* taps of output o (of n_out) over n_in source texels: Lanczos-2 widened by the downscale, normalised */
static void swz_taps(int n_in, int n_out, int o, int *first, float *w)
{
    float scale = (float)n_out / n_in, center = (o + 0.5f) / scale - 0.5f, radius = 2 / scale;
    int f = (int)floorf(center - radius + 1);
    if (f < 0) f = 0;
    if (f > n_in - SWZ_TAPS) f = n_in - SWZ_TAPS > 0 ? n_in - SWZ_TAPS : 0;
    float sum = 0;
    for (int k = 0; k < SWZ_TAPS; k++)
        w[k] = f + k < n_in ? swz_lanczos2((f + k - center) * scale) : 0, sum += w[k];
    for (int k = 0; k < SWZ_TAPS; k++)
        w[k] /= sum;
    *first = f;
}
/* b493 rows (r's stride) into out (same stride): the image at pw x ph by a separable Lanczos-2 on premultiplied RGBA
   (thin strokes keep their contrast, edges get no dark fringe), then edge-repeated. b491 used an area average: text
   strokes thinner than a texel turned to faint grey, the save screen and menus read blurry. */
/* b495 (issue 22): a job stepped p4_swz_step_us a frame instead of all at the first bind: 0.15-0.3 s of CPU at once was
   a hitch on every dialogue portrait. The original raster is drawn until the copy is done. */
int p4_swz_step_us = 2000;
typedef struct SwzJob {
    float *ring, *xw;
    int *xf;
    short *hu; /* per ring row and output column: the source index when all its horizontal taps share it, else -1 */
    unsigned char *out;
    int y, pw, ph, ring_row[SWZ_RING];
    unsigned us, flat, frames;
} SwzJob;
static void swz_job_free(SwzJob *j)
{
    if (!j)
        return;
    p4a_free(j->ring), p4a_free(j->xw), p4a_free(j->xf), p4a_free(j->hu), p4a_free(j->out), p4a_free(j);
}
static SwzJob *swz_job_new(const Raster *r, unsigned char *out, int pw, int ph)
{
    SwzJob *j = p4a_calloc(1, sizeof *j);
    if (!j)
        return 0;
    j->pw = pw, j->ph = ph;
    j->ring = p4a_memalign(16, SWZ_RING * pw * 4 * sizeof(float));
    j->xf = p4a_memalign(16, pw * sizeof(int));
    j->xw = p4a_memalign(16, pw * SWZ_TAPS * sizeof(float));
    j->hu = p4a_memalign(16, SWZ_RING * pw * sizeof(short));
    if (!j->ring || !j->xf || !j->xw || !j->hu) {
        swz_job_free(j); /* out stays the caller's: not in j yet */
        return 0;
    }
    j->out = out;
    for (int i = 0; i < SWZ_RING; i++) j->ring_row[i] = -1;
    for (int x = 0; x < pw; x++)
        swz_taps(r->width, pw, x, &j->xf[x], &j->xw[x * SWZ_TAPS]);
    return j;
}
static void swz_edges(const Raster *r, unsigned char *out, int pw, int ph)
{
    int W = r->width, H = r->height, stride = r->stride;
    for (int y = 0; y < ph; y++)
        for (int x = pw; x < W; x++)
            out[y * stride + x] = out[y * stride + pw - 1];
    for (int y = ph; y < H; y++)
        memcpy(out + y * stride, out + (ph - 1) * stride, stride);
}
/* output rows of j until budget_us has passed; 1 when the image is done (edges included) */
static int swz_step(const Raster *r, SwzJob *j, unsigned budget_us)
{
    const unsigned char *rows = r->originalPixels;
    const unsigned *pal = (const unsigned *)r->palette;
    int W = r->width, H = r->height, stride = r->stride, pw = j->pw, ph = j->ph;
    unsigned t0 = sceKernelGetSystemTimeLow();
    static float pre[256][4]; /* premultiplied palette */
    for (int i = 0; i < 256; i++) {
        unsigned c = pal[i];
        float al = (c >> 24) / 255.0f;
        pre[i][0] = (c & 255) * al, pre[i][1] = (c >> 8 & 255) * al, pre[i][2] = (c >> 16 & 255) * al, pre[i][3] = c >> 24;
    }
    float *ring = j->ring;
    short *hu = j->hu;
    unsigned char *out = j->out;
    for (; j->y < ph && sceKernelGetSystemTimeLow() - t0 < budget_us; j->y++) {
        int y = j->y, yf;
        float yw[SWZ_TAPS], acc[4];
        swz_taps(H, ph, y, &yf, yw);
        for (int k = 0; k < SWZ_TAPS; k++) { /* horizontal pass of the source rows this output row needs */
            int sy = yf + k;
            if (sy >= H || j->ring_row[sy % SWZ_RING] == sy) continue;
            float *h = ring + (sy % SWZ_RING) * pw * 4;
            short *u = hu + (sy % SWZ_RING) * pw;
            const unsigned char *src = rows + sy * stride;
            for (int x = 0; x < pw; x++) {
                const unsigned char *s = src + j->xf[x];
                int t = 1;
                while (t < SWZ_TAPS && j->xf[x] + t < W && s[t] == s[0])
                    t++;
                if (t == SWZ_TAPS || j->xf[x] + t >= W) { /* one index: its own colour, no taps */
                    u[x] = s[0];
                    continue;
                }
                u[x] = -1;
                const float *w = &j->xw[x * SWZ_TAPS];
                float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                for (int t = 0; t < SWZ_TAPS; t++) {
                    if (j->xf[x] + t >= W) break;
                    const float *c = pre[src[j->xf[x] + t]];
                    a0 += w[t] * c[0], a1 += w[t] * c[1], a2 += w[t] * c[2], a3 += w[t] * c[3];
                }
                h[x * 4] = a0, h[x * 4 + 1] = a1, h[x * 4 + 2] = a2, h[x * 4 + 3] = a3;
            }
            j->ring_row[sy % SWZ_RING] = sy;
        }
        for (int x = 0; x < pw; x++) {
            int one = hu[(yf % SWZ_RING) * pw + x];
            for (int k = 1; one >= 0 && k < SWZ_TAPS && yf + k < H; k++)
                if (hu[((yf + k) % SWZ_RING) * pw + x] != one)
                    one = -1;
            if (one >= 0) { /* the whole window is one index: copied (a portrait is mostly transparency and flat fills) */
                out[y * stride + x] = (unsigned char)one;
                j->flat++;
                continue;
            }
            acc[0] = acc[1] = acc[2] = acc[3] = 0;
            for (int k = 0; k < SWZ_TAPS && yf + k < H; k++) {
                int u = hu[((yf + k) % SWZ_RING) * pw + x];
                const float *h = u >= 0 ? pre[u] : ring + ((yf + k) % SWZ_RING) * pw * 4 + x * 4;
                acc[0] += yw[k] * h[0], acc[1] += yw[k] * h[1], acc[2] += yw[k] * h[2], acc[3] += yw[k] * h[3];
            }
            float al = acc[3] < 0 ? 0 : acc[3] > 255 ? 255 : acc[3];
            unsigned c[3];
            for (int i = 0; i < 3; i++) {
                float v = al > 0.5f ? acc[i] * 255 / al : 0;
                c[i] = v < 0 ? 0 : v > 255 ? 255 : (unsigned)(v + 0.5f);
            }
            unsigned char cand[16];
            int nc = 0, sx = (x * 2 + 1) * W / (2 * pw) - 1, sy = (y * 2 + 1) * H / (2 * ph) - 1; /* the centre's texel - 1 */
            sx = sx < 0 ? 0 : sx > W - 4 ? W - 4 : sx, sy = sy < 0 ? 0 : sy > H - 4 ? H - 4 : sy;
            for (int k = 0; k < 4; k++)
                for (int t = 0; t < 4; t++)
                    cand[nc++] = rows[(sy + k) * stride + sx + t];
            out[y * stride + x] = (unsigned char)swz_nearest(pal, cand, nc, c[0], c[1], c[2], (unsigned)(al + 0.5f));
        }
    }
    j->us += sceKernelGetSystemTimeLow() - t0, j->frames++;
    if (j->y < ph)
        return 0;
    swz_edges(r, out, pw, ph);
    return 1;
}
/* indices (r's size, the slot's own now) swizzled into the slot's copy; r itself when there is no memory to swizzle */
static Raster *swz_install(Clut32 *slot, Raster *r, unsigned char *indices, int pw, int ph, unsigned us, unsigned flat,
                           unsigned frames)
{
    slot->pw = pw, slot->ph = ph;
    Raster copy = *r;
    copy.originalPixels = copy.cpPixels = indices;
    /* b489: no mip levels. A 2D gauge (the battle HP bar) is drawn narrower as HP falls: the GE took a smaller level,
       and a point-decimated level of a thin bar held only empty texels (the bar read as 0). Level 0 is what the rows
       draw. */
    /* b490: levels again for big images (the title's 128..512 layers: 30 FPS with levels, ~20 without), each level
       keeping its blocks' most opaque texel; small UI images (gauges, icons) stay at level 0 */
    int big = 0; /* b492: no levels at all; big layers are prescaled instead (b490's levels blurred the menus) */
    /* b495: us is the CPU the prescale took over its frames, flat its texels copied without filtering */
    p4_log("swz new %p %dx%d d%d prescaled %dx%d in %u us over %u frames flat %u\n", (void *)r, r->width, r->height, r->depth,
           pw, ph, us, frames, flat);
    swizzle_to_arena = 1, swizzle_mips_alpha = big, swizzle_mips_off = !big;
    swizzle_raster(&copy);
    swizzle_to_arena = 0, swizzle_mips_alpha = 0, swizzle_mips_off = 0;
    if (!swizzle_head(copy.originalPixels)) { /* no memory to swizzle: keep the rows */
        p4a_free(copy.originalPixels);
        return r;
    }
    slot->copy = copy;
    swz_made++;
    return &slot->copy;
}
static Raster *swz_find(Raster *r)
{
    if (r->width < 64 || r->height < 8 || r->width > 512 || r->height > 512 || !r->originalPixels || swizzle_head(r->originalPixels))
        return r;
    if (!clut32_slots && !(clut32_slots = p4a_calloc(CLUT32_SLOTS, sizeof *clut32_slots)))
        return r;
    Clut32 *free_slot = 0;
    unsigned home = ((unsigned)r >> 4) * 2654435761u >> 24;
    for (int k = 0; k < CLUT32_PROBE; k++) {
        Clut32 *s = &clut32_slots[(home + k) & (CLUT32_SLOTS - 1)];
        if (s->of == r) {
            if (s->job) { /* b495: one step a frame (ge_queued: the list being written), the original drawn meanwhile */
                static unsigned stepped = ~0u;
                SwzJob *j = s->job;
                if (s->pixels != r->originalPixels || r->depth != 8 || stepped == ge_queued)
                    return r;
                stepped = ge_queued;
                if (!swz_step(r, j, p4_swz_step_us))
                    return r;
                unsigned char *out = j->out;
                int pw = j->pw, ph = j->ph;
                unsigned us = j->us, flat = j->flat, frames = j->frames;
                j->out = 0, s->job = 0;
                swz_job_free(j);
                return swz_install(s, r, out, pw, ph, us, flat, frames);
            }
            if (s->pixels != r->originalPixels || !s->copy.originalPixels || s->copy.depth != r->depth)
                return r;
            swz_binds++;
            return &s->copy;
        }
        if (!s->of && !free_slot)
            free_slot = s;
    }
    if (!free_slot)
        return r;
    unsigned bytes = r->stride * r->height;
    unsigned char *indices = p4a_memalign(64, bytes);
    free_slot->of = r, free_slot->pixels = r->originalPixels;
    if (!indices)
        return r; /* refusal remembered: copy stays 0 */
    if (p4_swz_prescale && r->depth == 8 && r->palette && r->width >= p4_swz_prescale_min && r->height >= p4_swz_prescale_min) {
        int pw = (r->width * 3 + 3) / 4, ph = (r->height * 17 + 27) / 28; /* 480/640, 272/448 */
        swz_prescaled++;
        if ((free_slot->job = swz_job_new(r, indices, pw, ph)))
            return r; /* made over the next frames */
        for (int y = 0; y < ph; y++) /* no memory for the filter: nearest texel */
            for (int x = 0; x < pw; x++)
                indices[y * r->stride + x] = r->originalPixels[(y * r->height / ph) * r->stride + x * r->width / pw];
        swz_edges(r, indices, pw, ph);
        return swz_install(free_slot, r, indices, pw, ph, 0, 0, 0);
    }
    memcpy(indices, r->originalPixels, bytes);
    return swz_install(free_slot, r, indices, 0, 0, 0, 0, 0);
}

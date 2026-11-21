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
typedef struct { const Raster *of; const unsigned char *pixels; Raster copy; } Clut32;
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
    unsigned char *p = s->copy.originalPixels;
    p4a_free(swizzle_head(p) ? p - SWIZZLE_HEAD : p);
    p4a_free(s->copy.palette);
    memset(s, 0, sizeof *s);
}

/* GE idle (p4_video_renderer_begin): every copy freed; made again at its next bind. b375 */
static unsigned clut32_release_all(void)
{
    unsigned bytes = 0;
    for (int i = 0; clut32_slots && i < CLUT32_SLOTS; i++)
        if (clut32_slots[i].of) {
            const Raster *copy = &clut32_slots[i].copy;
            if (copy->originalPixels) {
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

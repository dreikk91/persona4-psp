/* Boot: map the rebased retail data at its fixed PSP address, prove the pointers landed, enter the game. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspsdk.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspge.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("P4", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-1); /* the heap is claimed before main */
/* left free: thread stacks and modules loaded later. b518: 4096 -> 2048; the b495 PSP-3000 log still had 4336 KB free
   at every movie (MPEG library in the low hole, SAS and AVCODEC loaded), PPSSPP's lowest 4128 KB; no utility dialogs */
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

#define BUILD 529
#define P4_IMAGE_SIZE 0x40B000 /* retail 0x52da00..0x938a00: data and bss */
#include "paths.h"
#define GAME_DIR "ms0:/PSP/GAME/P4/" /* only until p4_paths_init: the log before it */
#ifdef P4_HW_PROFILE
#define DATA_FILE "p4data.hwdbg.bin"
#define LOG_FILE "p4_hwdbg.log"
#else
#define DATA_FILE "p4data.bin"
#define LOG_FILE "p4.log"
#endif
#define MAX_MISSING 512
#define P4_STR2(x) #x
#define P4_STR(x) P4_STR2(x)

/* The data image is part of the ELF, at P4_IMAGE, so the loader reserves it with the code. A fixed address high in
   the partition exists only on PPSSPP: hardware's 64 MB mode ends near 0x0BB00000, with the main stack below that. */
__asm__(".section .p4image,\"aw\",@nobits\n.globl p4_image\np4_image:\n.space " P4_STR(P4_IMAGE_SIZE) "\n.text\n");

int kwlnBoot(void);

int p4_quiet; /* the game owns the screen once the GE is up */

/* A log that is only flushed is empty after a crash: open, append, close for every commit. That costs ~17 ms per
   sceIoOpen on hardware, so the game's threads only copy lines into a ring and a writer thread commits it every
   100 ms. Before the writer starts (boot) and with p4_log_sync set, lines are committed at once as before.
   A crash loses at most the last 100 ms; the ring (p4_log_ring) can still be read with savemem. */
#define LOG_RING (64 * 1024)
void p4_log(const char *fmt, ...);
char p4_log_ring[LOG_RING];
static volatile unsigned log_head, log_tail, log_dropped;
static int log_thread_running;
int p4_log_sync;
const int p4_build_number = BUILD; /* b495 crash.c */
unsigned p4_log_ring_state(const char **ring, unsigned *size) /* b495 crash.c: the ring's end goes into p4crash.txt */
{
    *ring = p4_log_ring, *size = LOG_RING;
    return log_head;
}

/* The file first: a PSPLink stdout write blocks while the USB cable is out (b111: thousands of reconnects an evening),
   and the log on the memory stick must keep going meanwhile. */
/* b321 (from MeMeNT0xMoRii's fork): files live in the folder the EBOOT was launched from (paths.c), so the game
   also runs from CAT_ folders; the log starts in the default folder until that is known */
static char log_path[P4_PATH_MAX] = GAME_DIR LOG_FILE;
static void log_commit(const char *a, int n, const char *b, int m)
{
    SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        if (n) sceIoWrite(fd, a, n);
        if (m) sceIoWrite(fd, b, m);
        sceIoClose(fd);
    }
#ifdef P4_HW_PROFILE
    if (n) sceIoWrite(1, a, n);
    if (m) sceIoWrite(1, b, m);
#endif
}

/* Writes everything in the ring up to the head seen on entry; only the writer (or a sync caller) moves the tail. */
static void log_drain(void)
{
    static int busy;
    int intr = pspSdkDisableInterrupts();
    int mine = !busy;
    busy = 1;
    pspSdkEnableInterrupts(intr);
    if (!mine)
        return; /* the writer is on it */
    unsigned head = log_head, tail = log_tail;
    if (head == tail) {
        busy = 0;
        return;
    }
    unsigned from = tail % LOG_RING, n = head - tail;
    unsigned first = n < LOG_RING - from ? n : LOG_RING - from;
    log_commit(p4_log_ring + from, first, p4_log_ring, n - first);
    log_tail = head;
    if (log_dropped) {
        char line[64];
        int k = snprintf(line, sizeof line, "b%d log ring full, dropped %u lines\n", BUILD, log_dropped);
        log_dropped = 0;
        log_commit(line, k, 0, 0);
    }
    busy = 0;
}

static int log_writer(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(100 * 1000);
        log_drain();
    }
    return 0;
}

static void log_start(void)
{
    /* above the game's threads so a busy frame cannot starve it; it sleeps or waits on the stick otherwise */
    SceUID id = sceKernelCreateThread("p4_log", log_writer, 0x18, 16 * 1024, PSP_THREAD_ATTR_USER, 0);
    if (id >= 0 && sceKernelStartThread(id, 0, 0) >= 0)
        log_thread_running = 1;
    else
        p4_log("log writer thread failed %x, logging stays synchronous\n", id);
}

void p4_log_flush(void)
{
    log_drain();
}

/* Memory census (b166): psp_link wraps malloc/calloc/realloc/memalign/free; live bytes are counted per caller (return
   address, named by psp_log.py). The game's own allocations arrive through chkmemAllocate and RenderWare's malloc
   wrapper, the port's from its own functions. Blocks libc allocates internally are not seen. */
#include <malloc.h>
#include <unistd.h>
int p4_memstat = 0; /* b372-b376 census builds had it on; 1 for a census build: it must be on from boot, the table takes 768 KB of heap */
void *__real_malloc(size_t); void *__real_calloc(size_t, size_t); void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t); void __real_free(void *);
enum { MS_SLOTS = 1 << 16, MS_SITES = 1024 };
typedef struct { void *p; unsigned size, site; } MsBlock;
typedef struct { unsigned ra, live, peak, blocks, game; } MsSite;
static MsBlock *ms_table; /* open addressing, backward-shift deletion */
static MsSite ms_site[MS_SITES], ms_copy[MS_SITES];
static unsigned ms_used, ms_live, ms_peak, ms_lost, ms_allocs;
/* b518: the game's share. tools/psp_hw_debug.py puts these two labels around the game's objects (stubs, reasm and
   the census), so a caller between them is the game's code (chkmem*, RenderWare...), any other the port's or libc's.
   Weak: a link without them (psp_link.py) leaves both 0 and every caller counts as the port's. */
extern char p4_game_text_begin[] __attribute__((weak)), p4_game_text_end[] __attribute__((weak));
static unsigned ms_game_live, ms_game_peak, ms_port_live, ms_port_peak;

static unsigned ms_hash(unsigned v, unsigned bits) { return (v >> 3) * 2654435761u >> (32 - bits); }

static void ms_add(void *p, unsigned size, unsigned ra)
{
    if (!p || !p4_memstat)
        return;
    if (!ms_table && !(ms_table = __real_calloc(MS_SLOTS, sizeof *ms_table))) /* not under the interrupt lock: malloc may wait */
        p4_memstat = 0;
    int intr = pspSdkDisableInterrupts();
    if (!ms_table || ms_used >= MS_SLOTS / 8 * 7) {
        ms_lost++;
        pspSdkEnableInterrupts(intr);
        return;
    }
    unsigned s = ms_hash(ra, 10);
    while (ms_site[s].ra && ms_site[s].ra != ra)
        s = (s + 1) & (MS_SITES - 1);
    if (!ms_site[s].ra)
        ms_site[s].game = ra >= (unsigned)p4_game_text_begin && ra < (unsigned)p4_game_text_end;
    ms_site[s].ra = ra;
    if ((ms_site[s].live += size) > ms_site[s].peak)
        ms_site[s].peak = ms_site[s].live;
    ms_site[s].blocks++;
    if ((ms_live += size) > ms_peak)
        ms_peak = ms_live;
    if (ms_site[s].game) {
        if ((ms_game_live += size) > ms_game_peak)
            ms_game_peak = ms_game_live;
    } else if ((ms_port_live += size) > ms_port_peak)
        ms_port_peak = ms_port_live;
    ms_allocs++;
    unsigned i = ms_hash((unsigned)p, 16);
    while (ms_table[i].p)
        i = (i + 1) & (MS_SLOTS - 1);
    ms_table[i] = (MsBlock){ p, size, s };
    ms_used++;
    pspSdkEnableInterrupts(intr);
}

static void ms_remove(void *p)
{
    if (!p || !ms_table)
        return;
    int intr = pspSdkDisableInterrupts();
    unsigned i = ms_hash((unsigned)p, 16);
    while (ms_table[i].p && ms_table[i].p != p)
        i = (i + 1) & (MS_SLOTS - 1);
    if (ms_table[i].p) {
        MsSite *site = &ms_site[ms_table[i].site];
        site->live -= ms_table[i].size;
        site->blocks--;
        ms_live -= ms_table[i].size;
        *(site->game ? &ms_game_live : &ms_port_live) -= ms_table[i].size;
        ms_used--;
        for (unsigned j = i;;) { /* pull later entries of the run back over the hole */
            j = (j + 1) & (MS_SLOTS - 1);
            if (!ms_table[j].p)
                break;
            unsigned k = ms_hash((unsigned)ms_table[j].p, 16);
            if (i <= j ? i < k && k <= j : i < k || k <= j)
                continue;
            ms_table[i] = ms_table[j];
            i = j;
        }
        ms_table[i].p = 0;
    }
    pspSdkEnableInterrupts(intr);
}

static unsigned ms_site_ra(const void *p) /* the memstat caller of a block (0: not tracked) */
{
    if (!ms_table)
        return 0;
    unsigned i = ms_hash((unsigned)p, 16);
    while (ms_table[i].p && ms_table[i].p != p)
        i = (i + 1) & (MS_SLOTS - 1);
    return ms_table[i].p ? ms_site[ms_table[i].site].ra : 0;
}

#define MS_RA ((unsigned)__builtin_return_address(0))
/* b272: an allocation that fails gets one retry after the animation tracks (rwsky.c anim_tracks.h) give their memory
   back: the game writes through the null block of a failed chkmemAllocate (b271 bus error loading a dialogue) */
unsigned p4_anim_tracks_release(void);
unsigned p4_mesh_cache_release(void);
/* b274: the mesh cache too (rwsky.c). b275: logs the heap before and after (free total and the largest block). */
unsigned p4_heap_unclaimed(void);
void __real_free(void *);
/* b275: the largest block malloc can give now (binary search, 64 KB steps) */
static unsigned heap_largest(void)
{
    unsigned lo = 0, hi = 32u << 20;
    while (hi - lo > 65536) {
        unsigned mid = (lo + hi) / 2;
        void *p = __real_malloc(mid);
        if (p)
            __real_free(p), lo = mid;
        else
            hi = mid;
    }
    return lo;
}
void p4_heap_map(const char *why);
void p4_memstat_report(void);
/* b520 (issue #23): one 2.5 MB run held from boot. A big block the heap cannot give (F023_003.P01 wanted 1.98 MB with
   14 MB free in pieces, largest 1.9 MB) gets the reserve back first, on any thread (the caches only go on the drawing
   thread). The drawing thread takes it again when the heap has such a run (p4_heap_reserve_tick); taking it may leave a
   later load short, which then releases it again. 0 KB: off. */
#ifdef P4_HW_PROFILE
int p4_poison_check = 0; /* b529: 1 fills every freed heap block (tools/psp_stress.py turns it on) */
static unsigned poison_blocks, poison_said;
#endif
int p4_heap_reserve_kb = 2560;
static void *heap_reserve;
static unsigned heap_reserve_bytes(void) { return (unsigned)p4_heap_reserve_kb << 10; }
void p4_heap_reserve_tick(void)
{
#ifdef P4_HW_PROFILE
    if (p4_poison_check && (!poison_said++ || poison_blocks))
        p4_log("poison check: %u freed blocks filled\n", poison_blocks), poison_blocks = 0;
#endif
    if (heap_reserve || p4_heap_reserve_kb <= 0)
        return;
    struct mallinfo m = mallinfo(); /* 2 MB left besides it, or the game's next blocks give it straight back */
    if (m.fordblks + p4_heap_unclaimed() < heap_reserve_bytes() + (2u << 20))
        return;
    static int announced;
    if ((heap_reserve = __real_malloc(heap_reserve_bytes())) && announced++ < 8)
        p4_log("heap reserve: %u KB held at %x\n", heap_reserve_bytes() >> 10, (unsigned)heap_reserve);
}
static unsigned heap_release(unsigned n)
{
    if (n >= 256 * 1024 && n < 64u << 20) { /* b372: who holds the heap when a big block is refused */
        p4_memstat_report();
        p4_heap_map("before release");
    }
    struct mallinfo a = mallinfo();
    unsigned free_before = a.fordblks + p4_heap_unclaimed(), big_before = heap_largest();
    if (heap_reserve && n && n < 64u << 20) { /* b520: the reserve first, on any thread, for any size */
        __real_free(heap_reserve);
        heap_reserve = 0;
        void *p = __real_malloc(n);
        p4_log("heap reserve released for a request of %u (thread %x): %s\n", n, sceKernelGetThreadId(), p ? "fits" : "still short");
        p4_log_flush();
        if (p) {
            __real_free(p);
            return heap_reserve_bytes();
        }
    }
    extern int p4_arena_active(void);
    unsigned freed = 0;
    void *fits = 0;
    if (p4_arena_active()) { /* b376: the caches live in their arena: freeing them gives the heap nothing */
        fits = (void *)1;
    } else {
        freed = p4_anim_tracks_release() + p4_mesh_cache_release();
        fits = __real_malloc(n);
    }
    if (fits) {
        if (fits != (void *)1)
            __real_free(fits);
    } else { /* b340 (issue #12): still no run that size: every cache. b375: any size (b373: 545 KB refused, 9.5 MB
                of static lists and skins kept since the rule took only 1 MB and more) */
        extern unsigned p4_heap_release_all(void);
        unsigned all = p4_heap_release_all();
        if (!all)
            p4_log("heap short on thread %x: the caches can only be released on the drawing thread\n", sceKernelGetThreadId());
        freed += all;
    }
    struct mallinfo b = mallinfo();
    p4_log("heap short: request %u, free %u largest %u; released %u, now free %u largest %u, arena %u\n", n, free_before,
           big_before, freed, b.fordblks + p4_heap_unclaimed(), heap_largest(), (unsigned)b.arena);
    if (n >= 256 * 1024 && n < 64u << 20)
        p4_heap_map("after release");
    p4_log_flush();
    return freed;
}
void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (!p && n && heap_release(n))
        p = __real_malloc(n);
    ms_add(p, n, MS_RA);
    return p;
}
void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __real_calloc(a, b);
    if (!p && a && b && heap_release(a * b))
        p = __real_calloc(a, b);
    ms_add(p, a * b, MS_RA);
    return p;
}
void *__wrap_memalign(size_t a, size_t n)
{
    void *p = __real_memalign(a, n);
    if (!p && n && heap_release(n))
        p = __real_memalign(a, n);
    ms_add(p, n, MS_RA);
    return p;
}
int p4_arena_owns(const void *p); void p4a_free(void *p); void *p4a_realloc(void *old, unsigned n);
/* b523 heap guard. b522 and b523 on the PSP crashed in newlib's free (_free_r consolidating backwards) freeing a
   swizzled texture (rwsky.c unswizzle_raster, under tmxUploadStream): the block's two header words were 0xFFFFFFFF
   (b522) and 0xFF000000 (b523), texel-like values, so the block in front of it was written past its end. Every free
   of a heap block now checks the chunk (newlib mallocr.c, dlmalloc 2.6: prev_size, size | PREV_INUSE; in use when the
   next chunk has PREV_INUSE; a free one in front must have the size prev_size says). A broken one is logged with the
   chunk in front of it (the writer's block, found by walking the heap) and kept: a leak instead of the crash. */
extern char *__malloc_sbrk_base;
extern void *__malloc_av_[];
void __malloc_lock(struct _reent *); void __malloc_unlock(struct _reent *);
unsigned p4_heap_guard_hits;
static int heap_chunk_ok(const unsigned char *mem)
{
    const unsigned char *base = (const unsigned char *)__malloc_sbrk_base, *top = (const unsigned char *)__malloc_av_[2];
    const unsigned char *c = mem - 8;
    if (!base || base == (const unsigned char *)-1 || !top || c < base || c >= top || ((unsigned)mem & 7))
        return 0;
    unsigned hdr = ((const unsigned *)c)[1], size = hdr & ~3u;
    if (size < 16 || (size & 7) || c + size > top || !(((const unsigned *)(c + size))[1] & 1))
        return 0; /* the size runs out of the heap, or the next chunk says this one is free */
    if (!(hdr & 1)) { /* a free chunk in front: free() merges with it */
        unsigned prev = ((const unsigned *)c)[0];
        if (prev < 16 || (prev & 7) || prev > (unsigned)(c - base) || (((const unsigned *)(c - prev))[1] & ~3u) != prev)
            return 0;
    }
    return 1;
}
/* usable bytes of a heap block from malloc/memalign (0: not a sound chunk start); for rwsky.c tmxDmaCopy */
unsigned p4_heap_usable(const void *mem)
{
    return mem && !p4_arena_owns(mem) && heap_chunk_ok(mem) ? (((const unsigned *)mem)[-1] & ~3u) - 4 : 0;
}
static void heap_guard_report(const unsigned char *mem, unsigned ra)
{
    const unsigned *h = (const unsigned *)(mem - 8);
    p4_log("HEAP GUARD %u: free(%x) from %x (p4+%x): chunk header %08x %08x, words in front %08x %08x %08x %08x; kept\n",
           p4_heap_guard_hits, (unsigned)mem, ra, ra - 0x08804000, h[0], h[1], h[-4], h[-3], h[-2], h[-1]);
    /* the chunk in front: walk from the heap's base to the last sound chunk below mem */
    const unsigned char *base = (const unsigned char *)__malloc_sbrk_base, *top = (const unsigned char *)__malloc_av_[2];
    const unsigned char *c = base, *before = 0;
    while (((unsigned)(c + 8) & 15))
        c += 4;
    unsigned steps = 0;
    while (c < top && c < mem - 8 && steps++ < 400000) {
        unsigned size = ((const unsigned *)c)[1] & ~3u;
        if (size < 16 || (size & 7) || c + size > top + 16)
            break;
        before = c, c += size;
    }
    if (!before) {
        p4_log("HEAP GUARD: no chunk below %x (heap %x..%x)\n", (unsigned)mem, (unsigned)base, (unsigned)top);
        return;
    }
    const unsigned *b = (const unsigned *)(before + 8);
    unsigned bsize = ((const unsigned *)before)[1] & ~3u;
    p4_log("HEAP GUARD: walk %s at %x; chunk in front %x size %u (ends %x, %d bytes from the broken header), "
           "site %x, first words %08x %08x %08x %08x, last %08x %08x %08x %08x\n",
           c == mem - 8 ? "reached the broken chunk" : c < mem - 8 ? "stopped (bad chunk)" : "passed it: not a chunk start",
           (unsigned)c, (unsigned)(before + 8), bsize, (unsigned)(before + bsize), (int)((mem - 8) - (before + bsize)),
           ms_site_ra(before + 8), b[0], b[1], b[2], b[3], h[-4], h[-3], h[-2], h[-1]);
}
void __wrap_free(void *p)
{
    if (p4_arena_owns(p)) { /* b376: a cache block (rwsky.c arena.h), whoever frees it */
        p4a_free(p);
        return;
    }
    if (p && !heap_chunk_ok(p)) {
        __malloc_lock(_REENT);
        int ok = heap_chunk_ok(p); /* again under the lock: another thread may have been mid-free */
        if (!ok && p4_heap_guard_hits++ < 16)
            heap_guard_report(p, MS_RA);
        __malloc_unlock(_REENT);
        if (!ok) {
            p4_log_flush();
            return;
        }
    }
    ms_remove(p);
#ifdef P4_HW_PROFILE
    /* b529 (issue #24): a freed block filled with an odd, out-of-RAM word (a NaN as a float), so a stale pointer read
       out of it faults at once, in PPSSPP too, instead of whatever the next owner of the address left there */
    if (p4_poison_check && p) {
        unsigned *w = p, n = ((((const unsigned *)p)[-1] & ~3u) - 4) / 4;
        for (unsigned i = 0; i < n; i++)
            w[i] = 0xFFEEFEEFu;
        poison_blocks++;
    }
#endif
    __real_free(p);
}
void *__wrap_realloc(void *old, size_t n)
{
    if (p4_arena_owns(old))
        return p4a_realloc(old, n);
    void *p = __real_realloc(old, n);
    if (!p && n && heap_release(n))
        p = __real_realloc(old, n);
    if (p || !n) {
        ms_remove(old);
        ms_add(p, n, MS_RA);
    }
    return p;
}

/* Heap the allocator has not taken from sbrk yet. mallinfo's fordblks counts only the arena already taken: in the b166
   census the battle had 88 KB "free" there with ~5.7 MB of the heap block never touched. libcglue keeps the block's top
   in a local static, so it is found once by asking sbrk (which refuses past the top), under malloc's lock. */
unsigned p4_heap_unclaimed(void)
{
    static char *top;
    __malloc_lock(_REENT);
    char *now = sbrk(0);
    if (!top) {
        unsigned lo = 0, hi = 64u << 20; /* lo fits, hi does not */
        while (hi - lo > 4096) {
            unsigned mid = (lo + hi) / 2 & ~15u;
            if (sbrk(mid) != (void *)-1) {
                sbrk(-(int)mid);
                lo = mid;
            } else
                hi = mid;
        }
        top = now + lo;
    }
    __malloc_unlock(_REENT);
    return top > now ? top - now : 0;
}

/* b372 heap map: newlib's malloc (mallocr.c, dlmalloc 2.6) chunks walked from __malloc_sbrk_base to the top chunk.
   Each chunk: prev_size, size | PREV_INUSE (bit 0); in use when the next chunk has PREV_INUSE. A free run of at least
   min bytes ends a "gap"; for every gap the blocks in it and the three call sites holding the most of it are logged
   (sites from the memstat table: a census build), so the blocks that split the free memory have names
   (psp-addr2line). Logged when a large allocation fails, and on a poke of p4_heapmap (at the next memstat report). */
int p4_heapmap = 0, p4_heapmap_min = 64 * 1024;
void p4_heap_map(const char *why)
{
    __malloc_lock(_REENT);
    unsigned char *base = (unsigned char *)__malloc_sbrk_base, *top = (unsigned char *)__malloc_av_[2];
    if (!base || base == (unsigned char *)-1 || !top || top < base) {
        __malloc_unlock(_REENT);
        p4_log("heapmap %s: no heap (base %x top %x)\n", why, (unsigned)base, (unsigned)top);
        return;
    }
    unsigned char *c = base;
    while (((unsigned)(c + 8) & 15)) /* the first chunk's payload is 16-aligned (PSP newlib: base + 8 holds it) */
        c += 4;
    unsigned min = p4_heapmap_min > 4096 ? (unsigned)p4_heapmap_min : 4096;
    unsigned gap_blocks = 0, gap_bytes = 0, gaps = 0, free_big = 0, free_small = 0, used = 0, unknown = 0;
    unsigned site[3] = { 0 }, site_bytes[3] = { 0 }, largest = 0;
    unsigned char *gap_start = c;
    p4_log("heapmap %s: heap %x..%x (%u KB), runs of free >= %u KB end a gap\n", why, (unsigned)c, (unsigned)top,
           (unsigned)(top - c) >> 10, min >> 10);
    while (c < top) {
        unsigned size = ((unsigned *)c)[1] & ~3u;
        if (size < 16 || (size & 7) || c + size > top + 16) {
            p4_log("heapmap: bad chunk at %x size %x: walk stopped\n", (unsigned)c, size);
            break;
        }
        unsigned char *next = c + size;
        int in_use = next >= top ? ((unsigned *)top)[1] & 1 : ((unsigned *)next)[1] & 1;
        if (in_use) {
            unsigned ra = ms_site_ra(c + 8);
            used += size, gap_blocks++, gap_bytes += size;
            unknown += ra ? 0 : size;
            int k = 0;
            while (k < 3 && site[k] && site[k] != ra)
                k++;
            if (k < 3) {
                site[k] = ra, site_bytes[k] += size;
            } else { /* replace the smallest of the three when this site already outweighs it: approximate top 3 */
                int s = site_bytes[0] < site_bytes[1] ? (site_bytes[0] < site_bytes[2] ? 0 : 2) : (site_bytes[1] < site_bytes[2] ? 1 : 2);
                if (size > site_bytes[s])
                    site[s] = ra, site_bytes[s] = size;
            }
        } else if (size >= min) {
            if (gap_blocks)
                p4_log("heapmap gap @%5u KB: %5u blocks %6u KB; %07x %u KB, %07x %u KB, %07x %u KB\n",
                       (unsigned)(gap_start - base) >> 10, gap_blocks, gap_bytes >> 10, site[0] ? site[0] - 8 : 0, site_bytes[0] >> 10,
                       site[1] ? site[1] - 8 : 0, site_bytes[1] >> 10, site[2] ? site[2] - 8 : 0, site_bytes[2] >> 10);
            p4_log("heapmap free @%5u KB: %u KB\n", (unsigned)(c - base) >> 10, size >> 10);
            gaps++, free_big += size;
            if (size > largest)
                largest = size;
            gap_blocks = gap_bytes = 0;
            memset(site, 0, sizeof site), memset(site_bytes, 0, sizeof site_bytes);
            gap_start = next;
        } else {
            free_small += size, gap_bytes += size;
        }
        c = next;
    }
    unsigned top_size = ((unsigned *)top)[1] & ~3u;
    if (gap_blocks)
        p4_log("heapmap gap @%5u KB: %5u blocks %6u KB; %07x %u KB, %07x %u KB, %07x %u KB\n",
               (unsigned)(gap_start - base) >> 10, gap_blocks, gap_bytes >> 10, site[0] ? site[0] - 8 : 0, site_bytes[0] >> 10,
               site[1] ? site[1] - 8 : 0, site_bytes[1] >> 10, site[2] ? site[2] - 8 : 0, site_bytes[2] >> 10);
    __malloc_unlock(_REENT);
    p4_log("heapmap end: used %u KB (untracked %u KB), free in runs >= %u KB: %u KB in %u (largest %u KB), in smaller "
           "pieces %u KB; top chunk %u KB + unclaimed %u KB\n", used >> 10, unknown >> 10, min >> 10, free_big >> 10, gaps,
           largest >> 10, free_small >> 10, top_size >> 10, p4_heap_unclaimed() >> 10);
    p4_log_flush();
}

/* every 256 frames (rwsky.c): totals, then the 24 callers holding the most */
void p4_memstat_report(void)
{
    if (!ms_table)
        return;
    struct mallinfo m = mallinfo();
    int intr = pspSdkDisableInterrupts();
    memcpy(ms_copy, ms_site, sizeof ms_copy);
    unsigned live = ms_live, peak = ms_peak, used = ms_used, lost = ms_lost, allocs = ms_allocs;
    unsigned glive = ms_game_live, gpeak = ms_game_peak, plive = ms_port_live, ppeak = ms_port_peak;
    pspSdkEnableInterrupts(intr);
    if (p4_game_text_end)
        p4_log("memstat game live %u KB peak %u KB | port live %u KB peak %u KB\n", glive >> 10, gpeak >> 10, plive >> 10, ppeak >> 10);
    if (p4_heapmap)
        p4_heapmap = 0, p4_heap_map("poke");
    p4_log("memstat live %u KB peak %u KB blocks %u allocs %u lost %u table %u KB\n", live >> 10, peak >> 10, used, allocs, lost,
           (unsigned)(MS_SLOTS * sizeof(MsBlock)) >> 10);
    p4_log("memstat heap arena %d KB used %d KB free %d KB unclaimed %u KB | sys free %d KB\n", m.arena >> 10, m.uordblks >> 10,
           m.fordblks >> 10, p4_heap_unclaimed() >> 10, (int)sceKernelTotalFreeMemSize() >> 10);
    for (int n = 0; n < 24; n++) {
        int best = -1;
        for (int s = 0; s < MS_SITES; s++)
            if (ms_copy[s].ra && (best < 0 || ms_copy[s].live > ms_copy[best].live))
                best = s;
        if (best < 0 || !ms_copy[best].live)
            break;
        p4_log("memsite %6u KB peak %6u KB blocks %5u from %07x\n", ms_copy[best].live >> 10, ms_copy[best].peak >> 10,
               ms_copy[best].blocks, ms_copy[best].ra - 8);
        ms_copy[best].live = 0;
    }
}

void p4_log(const char *fmt, ...)
{
    char line[160];
    va_list args;
    int n = snprintf(line, sizeof line, "b%d ", BUILD);
    va_start(args, fmt);
    int body = vsnprintf(line + n, sizeof line - n, fmt, args);
    va_end(args);
    if (body < 0)
        return;
    if (body >= (int)sizeof line - n) { /* cut: still end the line (from the persona4-portable fork, e10a33f) */
        n = sizeof line - 1;
        line[n - 1] = '\n';
    } else {
        n += body;
    }
    if (!p4_quiet)
        pspDebugScreenPrintf("%s", line);
    int intr = pspSdkDisableInterrupts();
    unsigned head = log_head;
    int fits = head + n - log_tail <= LOG_RING;
    if (fits) {
        for (int i = 0; i < n; i++)
            p4_log_ring[(head + i) % LOG_RING] = line[i];
        log_head = head + n;
    } else {
        log_dropped++;
    }
    pspSdkEnableInterrupts(intr);
    if (!log_thread_running || p4_log_sync)
        log_drain();
}

static void stop(void)
{
    p4_log_flush();
    sceKernelDelayThread(2 * 1000 * 1000);
    sceKernelExitGame();
}

/* A function the port does not have yet: log it once with its caller, keep going while the list is short.
   The stubs tail-call this, so the return address is the game's. */
void p4_backtrace(void);
int p4_missing(const char *name)
{
    void *from = __builtin_return_address(0);
    static const char *seen[MAX_MISSING];
    static int count;
    for (int i = 0; i < count; i++)
        if (seen[i] == name)
            return 0;
    p4_log("missing %s from %x\n", name, (unsigned)from);
    p4_backtrace();
    seen[count++] = name;
    if (count == MAX_MISSING)
        count--;
    return 0;
}

/* Return addresses left on the stack, newest first: reassembled frames defeat PPSSPP's walker. */
void p4_backtrace(void)
{
    extern char __executable_start[], __etext[];
    unsigned *sp = __builtin_frame_address(0);
    for (int i = 0, n = 0; i < 2048 && n < 16 && (unsigned)(sp + i) < 0x0BFFFF00u; i++) {
        unsigned w = sp[i];
        if (w > (unsigned)__executable_start && w < (unsigned)__etext && !(w & 3) && *(unsigned *)(w - 8) >> 26 == 3) {
            p4_log("  from %x\n", w);
            n++;
        }
    }
}

/* ponytail: the statement is skipped, what it would have written stays stale; each one is logged once to be translated. */
void p4_ee_asm_trap(const char *file, int line)
{
    static const char *seen[64];
    static int seen_line[64], count;
    for (int i = 0; i < count; i++)
        if (seen_line[i] == line && seen[i] == file)
            return;
    p4_log("skipped asm %s:%d\n", file, line);
    if (count < 64) {
        seen[count] = file;
        seen_line[count++] = line;
    }
}

static int load(const char *path, void *dst, int max)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    /* the whole file, in as many reads as it takes (a read may return less, e.g. from the PSP Go's internal storage;
       from the persona4-portable fork, 2ad48b3); a file larger than max is refused */
    SceOff length = sceIoLseek(fd, 0, PSP_SEEK_END);
    int n = 0;
    if (length <= 0 || length > max || sceIoLseek(fd, 0, PSP_SEEK_SET) != 0)
        n = -1;
    while (n >= 0 && n < length) {
        int chunk = sceIoRead(fd, (char *)dst + n, (int)length - n);
        if (chunk <= 0) {
            n = -1;
            break;
        }
        n += chunk;
    }
    sceIoClose(fd);
    return n;
}

/* Every checked word must point inside the image, at text that follows a NUL or a rebased pointer. */
static int check_strings(const unsigned *list, int count)
{
    const unsigned end = P4_IMAGE + P4_IMAGE_SIZE;
    int bad = 0, first_bad = -1;
    for (int i = 0; i < count; i++) {
        /* a p4str.bin from another build may point outside the image (persona4-portable 2ad48b3) */
        if ((list[i] & 3) || list[i] > P4_IMAGE_SIZE - sizeof(unsigned)) {
            if (first_bad < 0)
                first_bad = (int)(0x52DA00 + list[i]);
            bad++;
            continue;
        }
        unsigned p = *(unsigned *)(P4_IMAGE + list[i]);
        const unsigned char *s = (const unsigned char *)p;
        /* s[-1] may be the top byte of a rebased pointer that sits right before the text (b517: the image at
           0x08F5DA00 crosses 0x09000000, so either top byte) */
        if (p <= P4_IMAGE || p >= end || (s[-1] != 0 && s[-1] != P4_IMAGE >> 24 && s[-1] != (end - 1) >> 24) || s[0] == 0) {
            if (first_bad < 0)
                first_bad = (int)(0x52DA00 + list[i]);
            bad++;
        }
    }
    p4_log("strings checked=%d bad=%d first_bad=%x\n", count, bad, first_bad);
    return bad;
}

/* HOME (PS) button: the system menu offers "Quit game" only once an exit callback is registered. The firmware stops
   the game's threads, audio and the Media Engine itself; the callback only lets a save in progress finish (up to 5 s)
   and commits the log. */
extern volatile int p4_mc_writing;

static int exit_callback(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    for (int i = 0; p4_mc_writing && i < 500; i++)
        sceKernelDelayThread(10 * 1000);
    /* b321 (from MeMeNT0xMoRii's fork, HOME froze on a PSP Go): HOME can pause the game with a frame stopped at its
       GE stall address; a list that is not done is broken off before the exit */
    int ge = sceGeDrawSync(1), broke = ge > PSP_GE_LIST_DONE ? sceGeBreak(1, 0) : 0;
    p4_log("HOME: exit to XMB (GE state %d, break %d)\n", ge, broke);
    p4_log_flush();
    sceKernelExitGame();
    return 0;
}

/* b499: sleep needs the volatile block the frame lists hold (rwsky.c p4_lists_volatile) */
extern volatile int p4_power_switch, p4_power_resumed;
static int power_callback(int unknown, int flags, void *arg)
{
    (void)unknown; (void)arg;
    if (flags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY))
        p4_power_switch = 1;
    if (flags & PSP_POWER_CB_RESUME_COMPLETE)
        p4_power_resumed = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("p4_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    int pcb = sceKernelCreateCallback("p4_power", power_callback, NULL);
    int pr = scePowerRegisterCallback(-1, pcb);
    if (pr < 0)
        p4_log("power callback %08x: sleep stays blocked while the lists hold the volatile block\n", pr);
    sceKernelSleepThreadCB();
    return 0;
}

static void exit_callback_start(void)
{
    SceUID id = sceKernelCreateThread("p4_exit", callback_thread, 0x11, 4 * 1024, PSP_THREAD_ATTR_USER, 0);
    if (id < 0 || sceKernelStartThread(id, 0, 0) < 0)
        p4_log("exit callback thread failed %x, HOME will not quit\n", id);
}

int main(int argc, char **argv)
{
    static unsigned list[4096]; /* b250: 8192 -> 4096 words (the image was 856 bytes over); p4str.bin is 2476 words, a full buffer stops the boot below */
    __asm__ volatile("ctc1 $0, $31"); /* FPU traps off: on hardware a NaN compare powers the console off */
    __asm__ volatile("move $gp, %0" : : "r"(P4_GP)); /* reassembled code reads small data off $gp; threads inherit it */
    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenInit();
    {
        char cwd[P4_PATH_MAX];
        /* a launch from elsewhere (PSPLink's host0:) keeps the old fixed folder */
        if (p4_paths_init(argc > 0 ? argv[0] : NULL, getcwd(cwd, sizeof cwd)) < 0 && p4_paths_init(NULL, "ms0:/PSP/GAME/P4") < 0) {
            pspDebugScreenPrintf("Cannot resolve the game folder from the launch path.\n");
            sceKernelDelayThread(5 * 1000 * 1000);
            sceKernelExitGame();
            return 1;
        }
        snprintf(log_path, sizeof log_path, "%s%s", p4_game_dir, LOG_FILE);
        p4_log("game folder %s\n", p4_game_dir);
        SceIoStat st;
        if (sceIoGetstat(p4_path("p4files.idx"), &st) < 0) { /* issue #20: an "EBOOT only" build has no game archives */
            p4_log("p4files.idx missing: game archives were not prepared\n");
            pspDebugScreenPrintf("Game files are missing (p4files.idx, DATA.*, SOUND).\n\n"
                                 "Run the full preparation (without \"Build EBOOT only\")\n"
                                 "and copy the whole P4 folder to PSP/GAME/.\n");
            sceKernelDelayThread(10 * 1000 * 1000);
            sceKernelExitGame();
            return 1;
        }
    }
    { /* b376: the caches' arena, before the game takes its first block */
        extern void p4_arena_init(void);
        p4_arena_init();
        p4_heap_reserve_tick(); /* b520: the reserve, low in the heap before the game's blocks */
    }
    { /* b495 (issue #21): the crash catcher (crash.c); an empty CRASHTEST file next to the EBOOT then stores to
         address 4, to see p4crash.txt written and the return to the XMB on a device */
        extern void p4_crash_init(void);
        p4_crash_init();
        SceIoStat st;
        if (sceIoGetstat(p4_path("CRASHTEST"), &st) >= 0) {
            p4_log("CRASHTEST: a store to address 4 now\n");
            p4_log_flush();
            *(volatile int *)4 = 0;
        }
    }
    { /* b321: an empty FPS file next to the EBOOT turns the frame-rate overlay on (rwsky.c) */
        extern int p4_fps_overlay;
        SceIoStat st;
        p4_fps_overlay = sceIoGetstat(p4_path("FPS"), &st) >= 0;
    }
    { /* b410: an empty ANIMTRACE file next to the EBOOT turns the animation trace on (skin.h, mode 3), into the log */
        extern int p4_anim_trace;
        SceIoStat st;
        if (sceIoGetstat(p4_path("ANIMTRACE"), &st) >= 0) {
            p4_anim_trace = 3;
            p4_log("ANIMTRACE: animation trace on (clip changes, backward times, poses after each change)\n");
        }
    }
    { /* b424: an empty FLATALL file next to the EBOOT turns on p4_flat_all (b419, indexed draws drawn from flat copies),
         for a device A/B without PSPLink */
        extern int p4_flat_all;
        SceIoStat st;
        p4_flat_all = sceIoGetstat(p4_path("FLATALL"), &st) >= 0;
        p4_log("switch files: FLATALL %d\n", p4_flat_all);
    }
#ifdef P4_HW_PROFILE
    { /* b423: an empty PROFCLOCK file next to the hwdbg EBOOT: section profile by the system clock and the model census,
         for a device run without PSPLink (on the PSP the clock is real time: cache and bus stalls included) */
        extern int p4_prof_clock, p4_model_census;
        SceIoStat st;
        if (sceIoGetstat(p4_path("PROFCLOCK"), &st) >= 0) {
            p4_prof_clock = p4_model_census = 1;
            p4_log("PROFCLOCK: clock profile and model census on\n");
        }
    }
#endif
#ifdef P4_HW_PROFILE
    extern char p4_image[];
    if ((unsigned)p4_image != P4_IMAGE) {
        p4_log("hwdbg wrong load base: image=%08x expected=%08x\n", (unsigned)p4_image, P4_IMAGE);
        stop();
        return 1;
    }
    p4_log("hwdbg b%d PRX image=%08x\n", BUILD, (unsigned)p4_image);
#endif
    void *got = (void *)P4_IMAGE;
    unsigned stack = (unsigned)__builtin_frame_address(0);
    p4_log("P4 boot free=%d KB maxfree=%d KB stack=%x\n", (int)sceKernelTotalFreeMemSize() / 1024, (int)sceKernelMaxFreeMemSize() / 1024, stack);
    { extern unsigned p4_video_low_hole(void); p4_log("P4 low hole for the MPEG library at %x\n", p4_video_low_hole()); } /* b1402 */
    if (stack < 0x0A000000u) { /* the main stack sits at the top of the partition: this is the 24 MB one */
        p4_log("Extra memory is off. Enable the custom firmware's extra memory setting and check the memory stick.\n");
        p4_log("The PSP-1000 has no extra memory and is not supported.\n");
        p4_log_flush();
        sceKernelDelayThread(8 * 1000 * 1000);
        sceKernelExitGame();
        return 1;
    }
    memset(got, 0, P4_IMAGE_SIZE);
    int data = load(p4_path(DATA_FILE), got, P4_IMAGE_SIZE);
    int count = load(p4_path("p4str.bin"), list, sizeof list) / 4;
    p4_log("loaded data=%d list=%d\n", data, count);
    if (count >= (int)(sizeof list / 4))
        p4_log("p4str.bin fills the list of %d words: enlarge it in main()\n", (int)(sizeof list / 4));
    if (data <= 0 || count <= 0 || count >= (int)(sizeof list / 4) || check_strings(list, count)) {
        stop();
        return 1;
    }
    sceKernelDcacheWritebackAll();
    log_start();
    exit_callback_start();
    p4_log("entering kwlnBoot free=%d KB\n", (int)sceKernelTotalFreeMemSize() / 1024);
    int result = kwlnBoot();
    void p4_video_shutdown(void);
    p4_video_shutdown();
    p4_log("kwlnBoot returned %d\n", result);
    stop();
    return 0;
}

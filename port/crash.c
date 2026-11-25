/* b495 crash catcher, the EBOOT half (issue #21: a hard power-off with nothing in the log). On a CFW with KUBridge
   the kernel half (p4crash_prx.c, embedded here as p4crash_prx[]) is written next to the EBOOT and loaded; it sends
   a faulting thread, in user mode, to p4_crash_trap with the registers. The trap writes p4crash.txt in the game folder
   (registers, the code addresses on the stack, the last 16 KB of the log ring, which a crash otherwise loses), then
   returns to the XMB instead of the power-off. The file is written in three commits (open, write, close each), so a
   second fault while reading a broken stack still leaves the registers on the stick.
   Map an address back with psp-addr2line -e p4.elf <p4+offset> (the hwdbg ELF has -g), or psp-nm. */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>
#include "paths.h"

void p4_log(const char *fmt, ...);
void p4_log_flush(void);
unsigned p4_log_ring_state(const char **ring, unsigned *size); /* main.c: the head, the ring and its size */
extern const int p4_build_number;
extern const unsigned char p4crash_prx[];
extern const unsigned p4crash_prx_size;
SceUID kuKernelLoadModule(const char *path, int flags, SceKernelLMOption *option); /* kubridge.S */

enum { R_GPR = 0, R_LO = 32, R_HI, R_BADVADDR, R_CAUSE, R_EPC, R_STATUS, R_FCR31, R_ENTRIES, R_FOREIGN, R_FOREIGN_EPC,
       R_INSTALL, R_WORDS }; /* p4crash_prx.c */
static volatile unsigned crash_regs[R_WORDS];
static unsigned char crash_stack[32 * 1024] __attribute__((aligned(16)));
static char report[4096];
enum { LOAD_BASE = 0x08804000 }; /* tools/psp_hw_debug.py: the EBOOT's fixed text base */

static char *put(char *p, const char *s)
{
    while (*s && p < report + sizeof report - 1)
        *p++ = *s++;
    return p;
}
static char *hex(char *p, unsigned v)
{
    char s[9];
    for (int i = 0; i < 8; i++)
        s[i] = "0123456789abcdef"[v >> (28 - 4 * i) & 15];
    s[8] = 0;
    return put(p, s);
}
static int in_code(unsigned a)
{
    return a >= LOAD_BASE && a < P4_IMAGE && !(a & 3);
}
static char *addr(char *p, unsigned a) /* "0889abcd (p4+85abcd)" */
{
    p = hex(p, a);
    if (in_code(a))
        p = put(p, " (p4+"), p = hex(p, a - LOAD_BASE), p = put(p, ")");
    return p;
}
static void commit(const char *path, const char *text, unsigned n, int append)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, n);
        sceIoClose(fd);
    }
}

static const char *const exc_names[32] = {
    "interrupt", "TLB modified", "address error or TLB miss on load or fetch", "TLB miss on store",
    "address error on load or fetch (unaligned or kernel address)", "address error on store (unaligned or kernel address)",
    "bus error on fetch", "bus error on load or store", "syscall", "break", "reserved instruction",
    "coprocessor unusable", "overflow", "trap", "?", "floating point", [16 ... 31] = "?"};

void p4_crash_trap(volatile unsigned *r)
{
    if (r[R_ENTRIES] > 1) /* the report itself faulted, or a second thread: park it, the first report goes on */
        for (;;)
            sceKernelSleepThread();
    char path[P4_PATH_MAX];
    strncpy(path, p4_path("p4crash.txt"), sizeof path - 1);
    path[sizeof path - 1] = 0;
    static const char *const names[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
                                          "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9",
                                          "k0", "k1", "gp", "sp", "fp", "ra"};
    unsigned cause = r[R_CAUSE], code = cause >> 2 & 31;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    sceKernelReferThreadStatus(sceKernelGetThreadId(), &info);
    char *p = report;
    p = put(p, "P4 crash, build "), p = hex(p, (unsigned)p4_build_number), p = put(p, " (hex)\nthread ");
    p = put(p, info.name[0] ? info.name : "?"), p = put(p, " id "), p = hex(p, (unsigned)sceKernelGetThreadId());
    p = put(p, "\nexception "), p = hex(p, code), p = put(p, ": "), p = put(p, exc_names[code]);
    p = put(p, "\nEPC      "), p = addr(p, r[R_EPC]);
    p = put(p, "\nBadVAddr "), p = hex(p, r[R_BADVADDR]);
    p = put(p, "\nra       "), p = addr(p, r[31]);
    p = put(p, "\ncause "), p = hex(p, cause), p = put(p, " status "), p = hex(p, r[R_STATUS]);
    p = put(p, " fcr31 "), p = hex(p, r[R_FCR31]), p = put(p, " lo "), p = hex(p, r[R_LO]), p = put(p, " hi "), p = hex(p, r[R_HI]);
    p = put(p, "\nkernel contexts parked before: "), p = hex(p, r[R_FOREIGN]), p = put(p, " last at "), p = hex(p, r[R_FOREIGN_EPC]);
    for (int i = 0; i < 32; i++) {
        p = put(p, i % 4 ? "  " : "\n"), p = put(p, names[i]), p = put(p, " "), p = hex(p, r[i]);
    }
    p = put(p, "\n");
    commit(path, report, p - report, 0); /* first commit: the registers */
    p4_log("CRASH exception %x at %08x (p4+%x) ra %08x badvaddr %08x thread %s: see p4crash.txt\n", code, r[R_EPC],
           r[R_EPC] - LOAD_BASE, r[31], r[R_BADVADDR], info.name);

    /* second: code addresses on the stack (candidate return addresses, newest first), then the raw words */
    unsigned sp = r[29];
    p = report;
    p = put(p, "\nstack from sp "), p = hex(p, sp), p = put(p, ", code addresses (callers, newest first):\n");
    if (sp >= 0x08800000 && sp < 0x0bfff000 && !(sp & 3)) {
        const unsigned *s = (const unsigned *)sp;
        unsigned words = (0x0c000000 - sp) / 4 < 1024 ? (0x0c000000 - sp) / 4 : 1024;
        for (unsigned i = 0, shown = 0; i < words && shown < 32; i++)
            if (in_code(s[i]))
                p = put(p, "  sp+"), p = hex(p, i * 4), p = put(p, " "), p = addr(p, s[i]), p = put(p, "\n"), shown++;
        p = put(p, "raw:");
        for (unsigned i = 0; i < 64 && i < words; i++)
            p = put(p, i % 8 ? " " : "\n"), p = hex(p, s[i]);
        p = put(p, "\n");
    } else
        p = put(p, "  (sp outside user memory)\n");
    commit(path, report, p - report, 1);

    /* third: the end of the log ring, oldest first: the lines the 100 ms writer had not committed yet */
    const char *ring;
    unsigned size, head = p4_log_ring_state(&ring, &size), n = size < 16384 ? size : 16384;
    commit(path, "\n--- the last log lines (ring) ---\n", 35, 1);
    unsigned from = (head - n) % size, first = n < size - from ? n : size - from;
    commit(path, ring + from, first, 1);
    if (n > first)
        commit(path, ring, n - first, 1);
    p4_log_flush();
    sceKernelExitGame(); /* back to the XMB instead of the hang and power-off */
    for (;;)
        sceKernelSleepThread();
}

/* Boot: write p4crash.prx next to the EBOOT when it is missing or different, load and start it through KUBridge. */
void p4_crash_init(void)
{
    const unsigned *stub = (const unsigned *)(void *)kuKernelLoadModule;
    if ((stub[0] >> 26) != 2 && (stub[1] & 0xfc00003f) != 0x0c) { /* still pspsdk's jr ra: no KUBridge here */
        p4_log("crash catcher: no KUBridge (not a CFW that has it), off\n");
        return;
    }
    char path[P4_PATH_MAX];
    strncpy(path, p4_path("p4crash.prx"), sizeof path - 1);
    path[sizeof path - 1] = 0;
    int same = 0;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        unsigned char chunk[512];
        unsigned at = 0;
        int got;
        same = 1;
        while (same && (got = sceIoRead(fd, chunk, sizeof chunk)) > 0) {
            same = at + got <= p4crash_prx_size && !memcmp(chunk, p4crash_prx + at, got);
            at += got;
        }
        same = same && at == p4crash_prx_size;
        sceIoClose(fd);
    }
    if (!same)
        commit(path, (const char *)p4crash_prx, p4crash_prx_size, 0);
    SceUID mod = kuKernelLoadModule(path, 0, NULL);
    if (mod < 0) {
        p4_log("crash catcher: load %s failed %08x, off\n", path, mod);
        return;
    }
    struct { volatile unsigned *regs; unsigned trap, stack, gp; } args;
    args.regs = crash_regs, args.trap = (unsigned)p4_crash_trap, args.stack = (unsigned)(crash_stack + sizeof crash_stack - 16);
    __asm__ volatile("move %0, $gp" : "=r"(args.gp));
    int status = 0, r = sceKernelStartModule(mod, sizeof args, &args, &status, NULL);
    /* R_INSTALL: 0 the module never ran, 1 the handler is registered, else the firmware's error + 1 */
    p4_log("crash catcher: module %08x start %08x status %d handler %08x%s\n", mod, r, status, crash_regs[R_INSTALL] - 1,
           crash_regs[R_INSTALL] == 1 ? " (on)" : " (off)");
}

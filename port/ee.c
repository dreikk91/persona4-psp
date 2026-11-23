/* The EE kernel calls the game makes directly, on the PSP kernel. EE priorities run 0-127 with 0 highest, like the PSP's. */
#include <pspkernel.h>

#define MAX_THREADS 16
#define MIN_STACK (64 * 1024)
#define DUMMY "p4"

typedef struct { int status; void *func, *stack; int stackSize; void *gp; int initPriority, currentPriority; unsigned attr, option; } ThreadParam;
typedef struct { int count, maxCount, initCount, waitThreads; unsigned attr, option; } SemaParam;

static struct { SceUID id; void (*func)(void *); } threads[MAX_THREADS];

static int psp_priority(int ee)
{
    int p = 16 + ee / 2;
    return p > 111 ? 111 : p;
}

static int trampoline(SceSize size, void *arg)
{
    SceUID self = sceKernelGetThreadId();
    __asm__ volatile("move $gp, %0" : : "r"(P4_GP)); /* a new thread may start with the module's $gp */
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].id == self)
            threads[i].func(*(void **)arg);
    return 0;
}

int CreateThread(ThreadParam *param)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (!threads[i].id) {
            int stack = param->stackSize < MIN_STACK ? MIN_STACK : param->stackSize;
            SceUID id = sceKernelCreateThread(DUMMY, trampoline, psp_priority(param->initPriority), stack, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, 0);
            threads[i].id = id;
            threads[i].func = param->func;
            return id;
        }
    return -1;
}

int StartThread(int id, void *arg) { return sceKernelStartThread(id, sizeof arg, &arg) >= 0 ? id : -1; }
int DeleteThread(int id)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].id == id)
            threads[i].id = 0;
    return sceKernelDeleteThread(id);
}
int TerminateThread(int id) { return sceKernelTerminateThread(id); }
int GetThreadId(void) { return sceKernelGetThreadId(); }
int ChangeThreadPriority(int id, int priority) { return sceKernelChangeThreadPriority(id, psp_priority(priority)); }
/* b416 p4_ee_watch: the game's sleep/wakeup calls in a ring (time, thread, target, caller, result); a watchdog thread
   logs the ring and every game thread's state when the game's tick has not moved for 5 s (b415 PSP: floor 63 from
   Heaven, user_main and p4 both asleep in SleepThread after F063_001.P01, no frame since). */
int p4_ee_watch = 1;
void p4_log(const char *fmt, ...);
extern unsigned p4_anim_tick;
enum { EW_RING = 64 };
static struct { unsigned t, self, target, ra; int op, r; } ew_ring[EW_RING];
static volatile unsigned ew_next;
static SceUID ew_thread = -1;
static void ew_note(int op, unsigned target, unsigned ra, int r)
{
    unsigned i = ew_next++ % EW_RING;
    ew_ring[i].t = sceKernelGetSystemTimeLow(), ew_ring[i].self = sceKernelGetThreadId(), ew_ring[i].target = target;
    ew_ring[i].ra = ra, ew_ring[i].op = op, ew_ring[i].r = r;
}
static const char *const ew_ops[] = { "sleep in", "sleep out", "wakeup", "cancel wakeup", "suspend", "resume", "direct sleep in", "direct sleep out",
                                      "cb sleep in", "cb sleep out" };
static void ew_thread_state(SceUID id, const char *what)
{
    SceKernelThreadInfo info;
    info.size = sizeof info;
    if (sceKernelReferThreadStatus(id, &info) >= 0)
        p4_log("EEWATCH thread %x %s %s: status %x wait type %d id %x wakeups %d pri %d\n", (unsigned)id, info.name, what, info.status,
               info.waitType, (unsigned)info.waitId, info.wakeupCount, info.currentPriority);
}
static int ew_main(SceSize args, void *argp)
{
    SceUID main_thread = *(SceUID *)argp;
    unsigned last = p4_anim_tick, still = 0, dumped = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (p4_anim_tick != last) {
            last = p4_anim_tick, still = 0, dumped = 0;
            continue;
        }
        if (++still < 5 || dumped || !p4_ee_watch)
            continue;
        dumped = 1;
        unsigned now = sceKernelGetSystemTimeLow(), n = ew_next;
        p4_log("EEWATCH no game tick for %u s (tick %u): the last sleep/wakeup calls, oldest first\n", still, last);
        for (unsigned k = n > EW_RING ? n - EW_RING : 0; k < n; k++) {
            unsigned i = k % EW_RING;
            p4_log("EEWATCH %u ms ago: thread %x %s %x from %08x -> %d\n", (now - ew_ring[i].t) / 1000, ew_ring[i].self,
                   ew_ops[ew_ring[i].op], ew_ring[i].target, ew_ring[i].ra, ew_ring[i].r);
        }
        (void)main_thread;
        SceUID ids[64];
        int count = 0;
        if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count) >= 0)
            for (int i = 0; i < count && i < 64; i++)
                ew_thread_state(ids[i], "");
    }
    return 0;
}
static void ew_start(void)
{
    if (ew_thread >= 0 || !p4_ee_watch)
        return;
    SceUID self = sceKernelGetThreadId();
    ew_thread = sceKernelCreateThread("p4_eewatch", ew_main, 0x70, 0x2000, PSP_THREAD_ATTR_USER, 0);
    if (ew_thread >= 0)
        sceKernelStartThread(ew_thread, sizeof self, &self);
}
int __real_sceKernelSleepThread(void);
int SleepThread(void)
{
    unsigned ra = (unsigned)__builtin_return_address(0);
    ew_start();
    ew_note(0, 0, ra, 0);
    int r = __real_sceKernelSleepThread();
    ew_note(1, 0, ra, r);
    return 0;
}
/* b416: a sleep that does not come through the EE call (linker --wrap): who else puts a thread to sleep */
int __wrap_sceKernelSleepThread(void)
{
    unsigned ra = (unsigned)__builtin_return_address(0);
    ew_start();
    ew_note(6, 0, ra, 0);
    int r = __real_sceKernelSleepThread();
    ew_note(7, 0, ra, r);
    return r;
}
/* b486: the callback variant too (battle results freeze, b485: user_main asleep in wait type 1 with no ring entry) */
int __real_sceKernelSleepThreadCB(void);
int __wrap_sceKernelSleepThreadCB(void)
{
    unsigned ra = (unsigned)__builtin_return_address(0);
    ew_start();
    ew_note(8, 0, ra, 0);
    int r = __real_sceKernelSleepThreadCB();
    ew_note(9, 0, ra, r);
    return r;
}
int WakeupThread(int id)
{
    int r = sceKernelWakeupThread(id);
    ew_note(2, (unsigned)id, (unsigned)__builtin_return_address(0), r);
    return r;
}
int CancelWakeupThread(int id)
{
    int r = sceKernelCancelWakeupThread(id);
    ew_note(3, (unsigned)id, (unsigned)__builtin_return_address(0), r);
    return r;
}
int SuspendThread(int id) { int r = sceKernelSuspendThread(id); ew_note(4, (unsigned)id, (unsigned)__builtin_return_address(0), r); return r; }
int ResumeThread(int id) { int r = sceKernelResumeThread(id); ew_note(5, (unsigned)id, (unsigned)__builtin_return_address(0), r); return r; }

/* Enough for the game: "is it running", its priority. */
int ReferThreadStatus(int id, ThreadParam *out)
{
    SceKernelThreadInfo info;
    info.size = sizeof info;
    if (sceKernelReferThreadStatus(id, &info) < 0)
        return -1;
    if (out) {
        out->status = info.status & PSP_THREAD_WAITING ? 2 : 1; /* EE: running or ready, waiting */
        out->currentPriority = info.currentPriority;
    }
    return id;
}

int CreateSema(SemaParam *param)
{
    return sceKernelCreateSema(DUMMY, 0, param->initCount, param->maxCount > 0 ? param->maxCount : 1, 0);
}
int DeleteSema(int id) { return sceKernelDeleteSema(id); }
int WaitSema(int id) { return sceKernelWaitSema(id, 1, 0) < 0 ? -1 : id; }
int SignalSema(int id) { return sceKernelSignalSema(id, 1) < 0 ? -1 : id; }
int PollSema(int id) { return sceKernelPollSema(id, 1) < 0 ? -1 : id; }

/* Reached by retail address from C that only knows func_ names. */
#define AT(address, name) __typeof__(name) func_##address __attribute__((alias(#name)));
AT(004215e0, CreateThread)
AT(00422920, StartThread)
AT(004215f0, DeleteThread)
AT(00421630, TerminateThread)
AT(004216d0, GetThreadId)
AT(00421670, ChangeThreadPriority)
AT(00421680, ChangeThreadPriority)
AT(00421700, SleepThread)
AT(00421710, WakeupThread)
AT(004226a8, WakeupThread)
AT(00421730, CancelWakeupThread)
AT(00421740, CancelWakeupThread)
AT(00421750, SuspendThread)
AT(004227c0, SuspendThread)
AT(00421770, ResumeThread)
AT(00421780, ResumeThread)
AT(004216e0, ReferThreadStatus)
AT(004216f0, ReferThreadStatus)
AT(004217e0, CreateSema)
AT(004217f0, DeleteSema)
AT(00421800, SignalSema)
AT(00421810, SignalSema)
AT(00421820, WaitSema)
AT(00421830, PollSema)
AT(00421840, PollSema)

int func_00421a60(int mode) { return 0; } /* FlushCache */

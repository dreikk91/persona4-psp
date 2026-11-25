/* b495 p4crash.prx: the crash catcher's kernel half (issue #21). A user EBOOT cannot register exception handlers, so
   this small kernel module, embedded in the EBOOT (crash.c) and loaded through the CFW's KUBridge, registers the
   default exception handler. On an exception in user code it saves the registers into a block the EBOOT owns and
   resumes the faulting thread, still in user mode, in the EBOOT's report function (crash.c p4_crash_trap) on the
   EBOOT's crash stack and $gp: the report is written by the game's own code, with its log ring and file paths.
   Exceptions in kernel code (the firmware's own break in a foreign context, seen at boot on other ports) are counted
   and that context parked, as the psp-developer notes measured on a PSP-3000; passing them on killed the console.
   Not compiled into the EBOOT: tools/psp_link.py build_crash_prx builds it as its own kernel PRX. */
#include <pspkernel.h>

PSP_MODULE_INFO("p4crash", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

int sceKernelRegisterDefaultExceptionHandler(void *func);

/* the block layout, shared with crash.c (words) */
enum { R_GPR = 0, R_LO = 32, R_HI, R_BADVADDR, R_CAUSE, R_EPC, R_STATUS, R_FCR31, R_ENTRIES, R_FOREIGN, R_FOREIGN_EPC,
       R_INSTALL, R_WORDS };
typedef struct { unsigned *regs, trap, stack, gp; } P4CrashArgs;

unsigned *p4c_regs, p4c_trap, p4c_stack, p4c_gp; /* read by the handler below */
void p4c_handler(void);

void p4c_park(void)
{
    for (;;)
        __asm__ volatile("nop");
}

/* Entered by the firmware with the faulting context's registers, v0 and v1 saved in COP0 control registers 4 and 5
   (the same entry pspsdk's src/debug/exception_asm.S uses). The first entry saves the context; later ones (the
   report itself faulted, or another thread) only send the thread to the trap on its own stack, which parks it. */
__asm__(
    ".set push\n.set noreorder\n.set noat\n"
    ".text\n.globl p4c_handler\np4c_handler:\n"
    "  mfc0  $v0, $14\n"                 /* EPC */
    "  bltz  $v0, 2f\n"                  /* kernel code: not ours */
    "  nop\n"
    "  lui   $v0, %hi(p4c_regs)\n"
    "  lw    $v0, %lo(p4c_regs)($v0)\n"
    "  lw    $v1, 156($v0)\n"            /* R_ENTRIES */
    "  addiu $v1, $v1, 1\n"
    "  sw    $v1, 156($v0)\n"
    "  addiu $v1, $v1, -1\n"
    "  bnez  $v1, 3f\n"                  /* already reporting: keep the first context and its stack */
    "  nop\n"
    "  sw $0, 0($v0)\n  sw $at, 4($v0)\n"
    "  cfc0 $at, $4\n  sw $at, 8($v0)\n"
    "  cfc0 $at, $5\n  sw $at, 12($v0)\n"
    "  sw $a0, 16($v0)\n  sw $a1, 20($v0)\n  sw $a2, 24($v0)\n  sw $a3, 28($v0)\n"
    "  sw $t0, 32($v0)\n  sw $t1, 36($v0)\n  sw $t2, 40($v0)\n  sw $t3, 44($v0)\n"
    "  sw $t4, 48($v0)\n  sw $t5, 52($v0)\n  sw $t6, 56($v0)\n  sw $t7, 60($v0)\n"
    "  sw $s0, 64($v0)\n  sw $s1, 68($v0)\n  sw $s2, 72($v0)\n  sw $s3, 76($v0)\n"
    "  sw $s4, 80($v0)\n  sw $s5, 84($v0)\n  sw $s6, 88($v0)\n  sw $s7, 92($v0)\n"
    "  sw $t8, 96($v0)\n  sw $t9, 100($v0)\n  sw $k0, 104($v0)\n  sw $k1, 108($v0)\n"
    "  sw $gp, 112($v0)\n  sw $sp, 116($v0)\n  sw $fp, 120($v0)\n  sw $ra, 124($v0)\n"
    "  mflo $v1\n  sw $v1, 128($v0)\n"
    "  mfhi $v1\n  sw $v1, 132($v0)\n"
    "  mfc0 $v1, $8\n  sw $v1, 136($v0)\n"   /* BadVAddr */
    "  mfc0 $v1, $13\n  sw $v1, 140($v0)\n"  /* Cause */
    "  mfc0 $v1, $14\n  sw $v1, 144($v0)\n"  /* EPC */
    "  mfc0 $v1, $12\n  sw $v1, 148($v0)\n"  /* Status */
    "  lui  $a0, 0x2000\n"                   /* CU1: the FPU is usable, its control word can be read */
    "  and  $a0, $a0, $v1\n"
    "  beqz $a0, 1f\n"
    "  nop\n"
    "  cfc1 $t0, $31\n  sw $t0, 152($v0)\n"
    "  ctc1 $0, $31\n"                       /* an FPU trap would fire again in the report */
    "1:\n"
    "  lui   $v1, %hi(p4c_stack)\n  lw $sp, %lo(p4c_stack)($v1)\n"  /* the faulting stack may be the fault */
    "3:\n"
    "  move  $a0, $v0\n"                     /* p4_crash_trap(regs) */
    "  lui   $v1, %hi(p4c_gp)\n  lw $gp, %lo(p4c_gp)($v1)\n"
    "  lui   $v1, %hi(p4c_trap)\n  lw $v1, %lo(p4c_trap)($v1)\n"
    "  mtc0  $v1, $14\n"
    "  nop\n  nop\n"
    "  eret\n"
    "  nop\n"
    "2:\n"                                   /* kernel context: count it, park it */
    "  lui   $v1, %hi(p4c_regs)\n"
    "  lw    $v1, %lo(p4c_regs)($v1)\n"
    "  lw    $a0, 160($v1)\n"                /* R_FOREIGN */
    "  addiu $a0, $a0, 1\n"
    "  sw    $a0, 160($v1)\n"
    "  sw    $v0, 164($v1)\n"                /* R_FOREIGN_EPC */
    "  lui   $v1, %hi(p4c_park)\n  addiu $v1, $v1, %lo(p4c_park)\n"
    "  mtc0  $v1, $14\n"
    "  nop\n  nop\n"
    "  eret\n"
    "  nop\n"
    ".set pop\n");

int module_start(SceSize args, void *argp)
{
    if (args < sizeof(P4CrashArgs) || !argp)
        return 1;
    const P4CrashArgs *a = argp;
    p4c_regs = a->regs, p4c_trap = a->trap, p4c_stack = a->stack, p4c_gp = a->gp;
    int r = sceKernelRegisterDefaultExceptionHandler((void *)((unsigned)p4c_handler | 0x80000000u));
    p4c_regs[R_INSTALL] = (unsigned)r + 1; /* 0: never ran; r + 1 otherwise (1: registered) */
    return 0; /* stays resident either way: the EBOOT reads R_INSTALL */
}

/* exported, so a stop really runs (pspsdk's default exports have no module_stop). The handler stays: the module is
   never stopped before the game exits. */
int module_stop(SceSize args, void *argp)
{
    (void)args, (void)argp;
    return 0;
}

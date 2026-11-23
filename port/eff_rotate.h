/* p4_eff_rotate (b480): the effect rotation helpers of effMisc.c without the VU0 model's generic steps.

   Heaven's orbiting particles (mdlEffectUpdateOrbitEmitter, field model 102) run, for every live particle every
   drawn tick, effMiscRotateXVU(tiltX), effMiscRotateYVU(tiltY) and effMiscCombineRotationVU(). The game's bodies are
   translated VU0 code: each instruction a call through the C model of VU0 (vu0.h: a loop over four lanes, a kind
   switch, a NaN/Inf clamp per lane). Here the same values are written with the same float operations, in the same
   order, straight into the shared VU0 state (p4vu), so every register and the accumulator end as before:
     RotateX: vf28..31 = rows of identity with up = (0, c, s), at = (0, -s, c); pos = (0, 0, 0, 1)
     RotateY: vf24..27 = rows of identity with right = (c, 0, -s), at = (s, 0, c)
     Combine: each of vf28..31 = vf24 * x + vf25 * y + vf26 * z + vf27 * w through ACC (MULA, MADDA, MADDA, MADD)
   The cos and sin of the last angles are kept (the particles keep their tilt: the same angles every tick).
   The EE registers the original's PEXTUW steps leave (r2..r5) are written too.
   p4_eff_rotate_check 1: every call also runs the VU0-model form on a copy and compares the registers (diffs).
   0: the game's translated bodies (effMisc.c's code, copied below as the reference). */
int p4_eff_rotate = 0, p4_eff_rotate_check = 0; /* b480 PSP Heaven: exact (check 0 diffs) but no gain (atick 0.85 M either way): off */
static unsigned eff_rot_calls, eff_rot_trig_hits, eff_rot_checks, eff_rot_diffs;

static struct { unsigned bits, valid; float c, s; } eff_trig[64];
static void eff_cos_sin(float angle, float *c, float *s)
{
    unsigned bits;
    __builtin_memcpy(&bits, &angle, 4);
    unsigned slot = (bits * 2654435761u) >> 26;
    if (eff_trig[slot].valid && eff_trig[slot].bits == bits) {
        *c = eff_trig[slot].c, *s = eff_trig[slot].s;
        eff_rot_trig_hits++;
        return;
    }
    *c = cosf(angle), *s = sinf(angle);
    eff_trig[slot].bits = bits, eff_trig[slot].valid = 1, eff_trig[slot].c = *c, eff_trig[slot].s = *s;
}

/* the original bodies (effMisc.c 004BD1A0, 004BD3E0, 004BD450), for 0 and the check */
static void eff_rotate_ref(float angle, int y)
{
    float m[16] __attribute__((aligned(16)));
    float cosine = cosf(angle), sine = sinf(angle);
    { __auto_type t0 = (m); P4Reg o0; p4_load(&o0, &t0, sizeof t0); p4vu.r[5] = p4vu.vf[0]; p4_pextuw(&p4vu.r[4], &p4vu.r[0], &p4vu.r[5]); p4_pextuw(&p4vu.r[2], &p4vu.r[0], &p4vu.r[4]); p4_pextuw(&p4vu.r[3], &p4vu.r[4], &p4vu.r[0]); __builtin_memcpy(((char *)o0.w[0] + (0)), &p4vu.r[2], 16); __builtin_memcpy(((char *)o0.w[0] + (16)), &p4vu.r[3], 16); __builtin_memcpy(((char *)o0.w[0] + (32)), &p4vu.r[4], 16); __builtin_memcpy(((char *)o0.w[0] + (48)), &p4vu.r[5], 16); }
    if (y)
        m[0] = cosine, m[2] = -sine, m[8] = sine, m[10] = cosine;
    else
        m[5] = cosine, m[6] = sine, m[9] = -sine, m[10] = cosine;
    int v = y ? 24 : 28;
    { __auto_type t0 = (m); P4Reg o0; p4_load(&o0, &t0, sizeof t0); __builtin_memcpy(&p4vu.vf[v], ((char *)o0.w[0] + (0)), 16); __builtin_memcpy(&p4vu.vf[v + 1], ((char *)o0.w[0] + (16)), 16); __builtin_memcpy(&p4vu.vf[v + 2], ((char *)o0.w[0] + (32)), 16); __builtin_memcpy(&p4vu.vf[v + 3], ((char *)o0.w[0] + (48)), 16); }
}
static void eff_combine_ref(void)
{
    for (int j = 28; j < 32; j++) {
        p4_vop(2, 15, &p4vu.acc, &p4vu.vf[24], &p4vu.vf[j], 0);
        p4_vop(3, 15, &p4vu.acc, &p4vu.vf[25], &p4vu.vf[j], 1);
        p4_vop(3, 15, &p4vu.acc, &p4vu.vf[26], &p4vu.vf[j], 2);
        p4_vop(3, 15, &p4vu.vf[j], &p4vu.vf[27], &p4vu.vf[j], 3);
    }
}

/* the identity rows and the EE registers the original's PEXTUW steps leave: r5 = vf0, r4 = at, r2 = right, r3 = up */
static void eff_rotate_regs(void)
{
    p4vu.r[5] = p4vu.vf[0];
    p4_pextuw(&p4vu.r[4], &p4vu.r[0], &p4vu.r[5]);
    p4_pextuw(&p4vu.r[2], &p4vu.r[0], &p4vu.r[4]);
    p4_pextuw(&p4vu.r[3], &p4vu.r[4], &p4vu.r[0]);
}

static void eff_rotate_native(float angle, int y)
{
    float c, s;
    eff_cos_sin(angle, &c, &s);
    eff_rotate_regs();
    P4Reg *v = &p4vu.vf[y ? 24 : 28];
    v[0] = p4vu.r[2], v[1] = p4vu.r[3], v[2] = p4vu.r[4], v[3] = p4vu.r[5];
    if (y)
        v[0].f[0] = c, v[0].f[2] = -s, v[2].f[0] = s, v[2].f[2] = c;
    else
        v[1].f[1] = c, v[1].f[2] = s, v[2].f[1] = -s, v[2].f[2] = c;
}

static void eff_combine_native(void)
{
    const P4Reg *a = &p4vu.vf[24], *b = &p4vu.vf[25], *c = &p4vu.vf[26], *d = &p4vu.vf[27];
    for (int j = 28; j < 32; j++) {
        P4Reg *t = &p4vu.vf[j];
        float x = t->f[0], y = t->f[1], z = t->f[2], w = t->f[3];
        float acc[4], out[4];
        for (int i = 0; i < 4; i++) acc[i] = p4_vu_finite(a->f[i] * x);
        for (int i = 0; i < 4; i++) acc[i] = p4_vu_finite(acc[i] + b->f[i] * y);
        for (int i = 0; i < 4; i++) acc[i] = p4_vu_finite(acc[i] + c->f[i] * z);
        for (int i = 0; i < 4; i++) out[i] = p4_vu_finite(acc[i] + d->f[i] * w);
        for (int i = 0; i < 4; i++) p4vu.acc.f[i] = acc[i], t->f[i] = out[i];
    }
}

static int eff_regs_differ(const struct P4Vu *a, const struct P4Vu *b)
{
    return __builtin_memcmp(&a->vf[24], &b->vf[24], 8 * sizeof(P4Reg)) || __builtin_memcmp(&a->acc, &b->acc, sizeof(P4Reg)) ||
           __builtin_memcmp(&a->r[2], &b->r[2], 4 * sizeof(P4Reg));
}

static void eff_rotate(float angle, int y)
{
    eff_rot_calls++;
    if (!p4_eff_rotate) {
        eff_rotate_ref(angle, y);
        return;
    }
    if (p4_eff_rotate_check) {
        static struct P4Vu saved, ref;
        saved = p4vu;
        eff_rotate_ref(angle, y);
        ref = p4vu;
        p4vu = saved;
        eff_rotate_native(angle, y);
        eff_rot_checks++;
        eff_rot_diffs += eff_regs_differ(&p4vu, &ref) != 0;
        return;
    }
    eff_rotate_native(angle, y);
}

void effMiscRotateXVU(float angle) { eff_rotate(angle, 0); }
void effMiscRotateYVU(float angle) { eff_rotate(angle, 1); }
void effMiscCombineRotationVU(void)
{
    eff_rot_calls++;
    if (!p4_eff_rotate) {
        eff_combine_ref();
        return;
    }
    if (p4_eff_rotate_check) {
        static struct P4Vu saved, ref;
        saved = p4vu;
        eff_combine_ref();
        ref = p4vu;
        p4vu = saved;
        eff_combine_native();
        eff_rot_checks++;
        eff_rot_diffs += eff_regs_differ(&p4vu, &ref) != 0;
        return;
    }
    eff_combine_native();
}

void p4_eff_rotate_report(void)
{
    if (eff_rot_calls)
        p4_log("eff rotate %d check %d per 256 frames: calls %u trig kept %u checks %u diffs %u\n", p4_eff_rotate,
               p4_eff_rotate_check, eff_rot_calls, eff_rot_trig_hits, eff_rot_checks, eff_rot_diffs);
    eff_rot_calls = eff_rot_trig_hits = eff_rot_checks = eff_rot_diffs = 0;
}

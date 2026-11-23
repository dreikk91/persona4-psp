/* Replace the CRI movie task at its scheduler boundary. The normal task destructor
 * still releases its work and wakes the event waiting for the cutscene to end. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "video.h"
#include "paths.h"
#include "audio.h"

extern void p4_log(const char *fmt, ...);
extern int iGpffffbaf8;
extern volatile unsigned p4_pad_press, p4_pad_reads;
static void movie_frame(unsigned frame) { iGpffffbaf8 = frame; }
unsigned p4_route_buttons(void);
static unsigned movie_buttons(void)
{
    unsigned route = p4_route_buttons();
    if (!p4_pad_reads) return route;
    p4_pad_reads--;
    return route | p4_pad_press;
}

/* b485: the movie runs in its own thread and this one watches it. A Media Engine RPC that never answers (sceMpegInit
   after load mpegbase: docs/audio_video_hang.md, b248-b484; or a decode stopping mid-movie, b484 P4CTOP1 packet 900)
   used to freeze the game for good. Now, when the player makes no progress (step, frame, shown) for
   VIDEO_STALL_US, the movie is given up: the game goes on as after a skipped movie, the stuck thread is left where it
   waits, and every later movie is skipped (the Media Engine does not answer any more this session). Audio restarts
   in a watched thread too; if that also hangs, the game goes on without sound. */
extern volatile unsigned video_frame, video_shown, video_step;
enum { VIDEO_STALL_US = 4000000 };
int p4_video_dead; /* the Media Engine stopped answering: movies are skipped */
int p4_audio_dead; /* the audio restart after it hung too: no sound */
static volatile int vt_done, vt_result;
static const char *vt_dir;
static char vt_stem[40];
static int vt_main(SceSize size, void *arg)
{
    (void)size, (void)arg;
    vt_result = p4_video_play(vt_dir, vt_stem, 0);
    p4_video_shutdown();
    vt_done = 1;
    return 0;
}
static volatile int at_done, at_result;
static int at_main(SceSize size, void *arg)
{
    (void)size, (void)arg;
    at_result = p4_audio_resume_after_video();
    at_done = 1;
    return 0;
}
/* run fn in a thread of this thread's priority; 1 done, 0 stalled (left running), -1 no thread */
static int watched(const char *name, int (*fn)(SceSize, void *), volatile int *done, int movie)
{
    *done = 0;
    SceUID t = sceKernelCreateThread(name, fn, sceKernelGetThreadCurrentPriority(), 64 * 1024,
                                     PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        if (t >= 0) sceKernelDeleteThread(t);
        return -1;
    }
    unsigned last = ~0u, since = sceKernelGetSystemTimeLow();
    while (!*done) {
        sceKernelDelayThread(10000);
        unsigned progress = movie ? video_step * 0x9e3779b1u ^ video_frame * 0x85ebca6bu ^ video_shown : 0;
        unsigned now = sceKernelGetSystemTimeLow();
        if (progress != last)
            last = progress, since = now;
        else if (now - since > VIDEO_STALL_US) {
            p4_log("%s stalled: no progress for %u ms (video step %u frame %u shown %u); left waiting, the game goes on\n", name,
                   (now - since) / 1000, video_step, video_frame, video_shown);
            return 0;
        }
    }
    sceKernelWaitThreadEnd(t, NULL);
    sceKernelDeleteThread(t);
    return 1;
}

int mwPlayerUpdate(unsigned char *owner)
{
    unsigned char *work = *(unsigned char **)(owner + 0x38);
    int index = *(short *)(work + 0x1ea);
    if (index < 0 || index >= 23) { p4_log("video invalid movie %d\n", index); return -1; }
    const char *path = *(const char **)(P4_K + 0x712c54 + index * 0x28);
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char stem[40]; int length = 0;
    while (base[length] && base[length] != '.' && length < (int)sizeof(stem) - 1) {
        stem[length] = toupper((unsigned char)base[length]); length++;
    }
    stem[length] = 0;
    char converted[P4_PATH_MAX]; SceIoStat stat;
    snprintf(converted, sizeof converted, "%sVIDEO/%s.p4v", p4_game_dir, stem);
    if (sceIoGetstat(converted, &stat) < 0) {
        p4_log("video missing %s; continuing\n", converted); return -1;
    }
    p4_log("video task begin index %d %s\n", index, stem);
    if (p4_video_renderer_begin() < 0) { p4_log("video GE did not drain\n"); return -1; }
    void *display = NULL; int stride = 0, format = 0;
    sceDisplayGetFrameBuf(&display, &stride, &format, PSP_DISPLAY_SETBUF_IMMEDIATE);
    p4_video_on_frame = movie_frame;
    p4_video_buttons = movie_buttons;
    int audio_paused = p4_audio_pause(1);
    {
        extern volatile unsigned p4_audio_blocks, p4_audio_errors, p4_audio_underruns, p4_audio_cpu_us;
        int p4_audio_state(void);
        p4_log("video audio state %d (1 running, 4 paused): blocks %u errors %u underruns %u cpu_us %u\n", p4_audio_state(),
               p4_audio_blocks, p4_audio_errors, p4_audio_underruns, p4_audio_cpu_us);
    }
    int result = -1;
    int audio_suspended = -1;
    if (audio_paused == 0) {
        p4_video_shutdown();
        audio_suspended = p4_audio_suspend_for_video();
    }
    if (audio_suspended >= 0 && p4_video_dead) {
        p4_log("video skipped: the Media Engine stopped answering earlier this session\n");
    } else if (audio_suspended >= 0) {
        vt_dir = p4_game_dir;
        snprintf(vt_stem, sizeof vt_stem, "%s", stem);
        int w = watched("p4video", vt_main, &vt_done, 1);
        if (w > 0)
            result = vt_result;
        else if (w == 0)
            p4_video_dead = 1;
        else { /* no thread: as before, on this one */
            result = p4_video_play(p4_game_dir, stem, 0);
            p4_video_shutdown();
        }
    } else
        p4_log("video skipped: audio firmware could not be released; continuing game\n");
    if (p4_audio_dead) {
        p4_log("video audio restart skipped: audio stopped answering earlier\n");
    } else if (p4_video_dead) {
        int w = watched("p4audio_restart", at_main, &at_done, 0);
        if (w == 0)
            p4_audio_dead = 1;
        else if ((w > 0 ? at_result : p4_audio_resume_after_video()) < 0)
            p4_log("video audio restart failed\n");
    } else if (p4_audio_resume_after_video() < 0)
        p4_log("video audio restart failed\n");
    if (audio_suspended < 0) p4_audio_pause(0);
    p4_video_on_frame = NULL; p4_video_buttons = NULL;
    if (display && stride > 0)
        sceDisplaySetFrameBuf(display, stride, format, PSP_DISPLAY_SETBUF_IMMEDIATE);
    p4_video_renderer_end();
    p4_log("video task end %s result %d; returning to game\n", stem, result);
    return -1;
}

/* The scheduler replacement presents directly; no native Sofdec draw nodes remain. */
void movieNativeTaskUpdate(void *context, void *work) { (void)context; (void)work; }

"""One measured diagnostic arm through the persistent PSPLink bridge; restores controls.

Example (after verifying modinfo and reaching a stationary scene):
  python tools/psp_probe_ab.py baseline --base 0x08804000
  python tools/psp_probe_ab.py no_particles --base 0x08804000 --set p4_probe_flags=4
Requires PSPDEV for the matching psp-nm. Never resets or starts the game.
"""
import argparse
import os
import json
import re
import struct
import subprocess
import time
from pathlib import Path

from psp_data_census import ROOT
from psp_link import BIN

DEFAULTS = {'p4_probe_flags': 0, 'p4_probe_geometry': 0, 'p4_probe_keep_every': 0,
            'p4_probe_geometry2': 0, 'p4_probe_geometry3': 0, 'p4_probe_geometry4': 0,
            'p4_probe_census': 0,
            'p4_probe_depth_mode': 0, 'p4_probe_depth_cut': 2000.0,
            'p4_mesh_frustum': 0, 'p4_exp_scissor': 0, 'p4_exp_ge_time': 0,
            'p4_ge_skin': 1, 'p4_ge_light': 1, 'p4_guard_clip': 1, 'p4_guard_compact': 1, 'p4_guard_near_ge': 0, 'p4_guard_clip_fast': 1, 'p4_guard_gnear': 1, 'p4_guard_boxes': 1, 'p4_uv_shift_step': 1, 'p4_cache_audit': 0, 'p4_cache_budget': 6291456,
            'p4_gs_test': 1, 'p4_gs_test_skip': 1, 'p4_pipeline': 1, 'p4_cull_far': 0, 'p4_guard_world': 1, 'p4_guard_full_cache': 1, 'p4_instance_replay': 1, 'p4_fog': 1, 'p4_vsync_late': 1, 'p4_light_prelit': 1, 'p4_light_unprelit': 1, 'p4_gs_mesh_alpha': 1, 'p4_flip_event': 1, 'p4_cover_stats': 0, 'p4_bone_direct': 1, 'p4_list_cached': 1, 'p4_skin_alpha': 1, 'p4_alpha_x2': 0, 'p4_im3d': 1, 'p4_uv_memo': 1, 'p4_hanim_c': 1, 'p4_hanim_vfpu': 1, 'p4_hanim_check': 0, 'p4_guard_chunk': 8, 'p4_guard_loop2': 1, 'p4_guard_box_vfpu': 1, 'p4_guard_box_check': 0, 'p4_probe_2d': 0, 'p4_frameskip': 1, 'p4_frameskip_max': 2, 'p4_frameskip_bones': 1, 'p4_uv_rebase': 1, 'p4_pose_plain': 0, 'p4_bone_check': 0, 'p4_dst_alpha_passes': 1, 'p4_lock_memo': 1, 'p4_rtt': 1, 'p4_skin_guard_fast': 1, 'p4_skin_guard_check': 0, 'p4_skin_vfpu': 1, 'p4_skin_vfpu_check': 0, 'p4_anim_cull': 0, 'p4_anim_far': 0, 'p4_anim_far_step': 2, 'p4_anim_far_depth': 500.0, 'p4_anim_far_kadd': 1, 'p4_skin_call': 1, 'p4_mesh_call': 1, 'p4_shadow_lookat': 1, 'p4_shadow_capsule': 1, 'p4_skip_visual_bits': 3, 'p4_guard_small': 0, 'p4_skin_sticky': 1, 'p4_skin_regroup': 1, 'p4_skin_pack': 1, 'p4_anim_skip_kadd': 1, 'p4_anim_lazy': 0, 'p4_anim_lazy_check': 0, 'p4_anim_native': 1, 'p4_anim_keymap': 1, 'p4_replay_census': 0, 'p4_skin_palette': 8, 'p4_bone_census': 0, 'p4_skin_simplify': 0, 'p4_mesh_override': 0, 'p4_mesh_capture': 0, 'p4_strip_why': 0, 'p4_strip_check': 0, 'p4_head_trace': 0, 'p4_fpu_chop': 0, 'p4_panel_scale': 0.0, 'p4_hanim_time': 1, 'p4_skin_dummy': 0, 'p4_cull_min_vertices': 500, 'p4_cull_cover': 0, 'p4_guard_result': 1, 'p4_guard_result_check': 0}
DEFAULTS.update(p4_ui_font=1, p4_ui_sprite=1, p4_font_skip=0, p4_calendar_skip=0)
FLOATS = {'p4_guard_big', 'p4_guard_tiny', 'p4_probe_depth_cut', 'p4_anim_far_depth', 'p4_panel_scale'}
DEFAULTS.update(p4_static_state_cache=0, p4_static_state_check=0)
DEFAULTS.update(p4_2d_alpha_masks=1, p4_translucent_late=1)
DEFAULTS.update(p4_skin_guard=0, p4_sync_skip_fresh=0, p4_sync_skip_check=0, p4_frame_census=0, p4_prof_clock=0, p4_matfx_env_ge=0, p4_guard_big=1.0, p4_anim_freeze=0, p4_static_list=1, p4_inst_batch=1, p4_clut32=1, p4_cull_back=3, p4_keep_previous=0, p4_im3d_cull=1, p4_feedback_sprites=1, p4_feedback_half=1, p4_screen_date=1, p4_gs_alpha_test=1, p4_alpha_write_test=1, p4_rtt_alpha=1, p4_timeline=0, p4_list_double=1)  # b356-b364, b370
DEFAULTS.update(p4_clut32_min=0, p4_guard_tiny=0.0)  # b388
DEFAULTS.update(p4_skin_wsplit=2, p4_clock_prof=0, p4_skin_nocolour=1, p4_skin_flat=1, p4_static_flat=1, p4_skin_drop_original=1)  # b390-b393
DEFAULTS.update(p4_2d_null_add=0, p4_probe_tlevel=0, p4_probe_tfilter=0, p4_mip_bias=0, p4_probe_fill=0, p4_2d_lean=1, p4_2d_cache=1, p4_2d_texonly=1)  # b1393-b1401
DEFAULTS.update(p4_static_pack=0, p4_static_pack_check=0)
DEFAULTS.update(p4_col_cull_check=0, p4_world_defer=1, p4_tex2d_census=0, p4_swz_copy=1)
DEFAULTS.update(p4_world_direct_check=0, p4_static_near=1)
DEFAULTS.update(p4_ui_native=0)  # b450: restore the compatibility path after native UI probes
DEFAULTS.update(p4_particle_vfpu=1, p4_particle_vfpu_check=0)
DEFAULTS.update(p4_fx_skip_draw=0, p4_skip_model_lite=7)
DEFAULTS.update(p4_static_uv_pack=1, p4_static_uv_check=0)
DEFAULTS.update(p4_static_position_pack=1)
DEFAULTS.update(p4_static_guard_lists=1)
DEFAULTS.update(p4_ptank_spill_budget=2 << 20)
DEFAULTS.update(p4_static_colour16=1, p4_anim_trace=0, p4_anim_trace_h=0)  # b409-b411
DEFAULTS.update(p4_flat_all=0, p4_model_census=0)  # b419-b420
DEFAULTS.update(p4_clut_pack=0)
DEFAULTS.update(p4_clear_stripes=1)
DEFAULTS.update(p4_ge_s8_step=0.35, p4_ge_s16_step=2.0, p4_ge_strips=1)  # b436-b438
FLOATS |= {'p4_ge_s8_step', 'p4_ge_s16_step'}
DEFAULTS.update(p4_inst_batch=1,
                p4_inst_batch_vertices=64, p4_inst_batch_min=4, p4_far_proxy=300, p4_far_proxy_min_vertices=100,
                p4_static_list=1, p4_static_list_check=0, p4_static_list_budget=4 << 20, p4_static_mesh_lists=1,
                p4_skin_list=1, p4_batch_stats=0, p4_anim_tracks=1, p4_anim_tracks_budget=1 << 20, p4_anim_tracks_reserve=3 << 19)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('label')
    ap.add_argument('--base', required=True, type=lambda v: int(v, 0))
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--shot', action='store_true')
    ap.add_argument('--preview', action='store_true', help='screenshot only, no timing windows')
    ap.add_argument('--samples', type=int, default=3)
    ap.add_argument('--old-build', action='store_true', help='measure an earlier build: its symbols come from its own ELF')
    ap.add_argument('--build-dir', help='the running build\'s folder (p4.elf, manifest.json) when build/psplink_bNNN is not it')
    args = ap.parse_args()
    if args.preview and not args.shot:
        ap.error('--preview requires --shot')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label) or args.samples < 2:
        ap.error('simple label and at least two samples required')
    session = Path(os.environ.get('P4_SESSION', ROOT / 'build/psplink_session'))
    log = session / 'console.log'
    queue = session / 'commands.txt'
    text = log.read_text(errors='replace')
    boots = re.findall(r'hwdbg b(\d+) PRX image=', text)
    if not boots or int(boots[-1]) < 85:
        raise SystemExit('No supported confirmed diagnostic boot')
    build_number = int(boots[-1])
    build = Path(args.build_dir) if args.build_dir else ROOT / f'build/psplink_b{build_number}'
    # Two agents share build numbers: refuse an ELF whose PRX is not the one deployed through host0.
    deployed = Path(f'C:/temp/p3f-psplink/b{build_number}/p4.prx')
    if deployed.is_file() and (build / 'p4.prx').is_file() and deployed.read_bytes() != (build / 'p4.prx').read_bytes():
        raise SystemExit(f'{build}/p4.prx is not the PRX deployed from {deployed}: another build took this number (--build-dir)')
    nm = subprocess.check_output([str(BIN / 'psp-nm.exe'), str(build / 'p4.elf')], text=True)
    symbols = {p[2]: int(p[0], 16) + args.base for p in (line.split() for line in nm.splitlines()) if len(p) == 3}
    manifest = json.loads((build / 'manifest.json').read_text())
    import hashlib
    for name, digest in manifest['source_sha256'].items():
        if not args.old_build and hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Build source changed; recheck symbols before touching RAM (--old-build: an earlier build, its own ELF)')
    changes = {}
    for item in args.set:
        name, value = item.split('=', 1)
        if name not in DEFAULTS:
            ap.error('unsupported control: ' + name)
        if name not in symbols:
            ap.error('control absent in this build: ' + name)
        changes[name] = float(value) if name in FLOATS else int(value, 0)

    def bits(name, value):
        return struct.unpack('<I', struct.pack('<f', value))[0] if name in FLOATS else value & 0xffffffff

    def send(commands):
        with queue.open('a', encoding='utf-8') as f:
            f.write('\n'.join(commands) + '\n')

    def write_controls(values, check=False):
        if not values:
            return
        commands = [f'pokew 0x{symbols[n]:08x} 0x{bits(n,v):08x}' for n, v in values.items()]
        commands.append('dcache w')
        if check:
            commands += [f'peekw 0x{symbols[n]:08x}' for n in values]
            commands += [f'echo DBG_FLUSH_AB_{i}_................................................' for i in range(40)]
        send(commands)

    def tail(offset):
        with log.open('rb') as f:
            f.seek(offset)
            return f.read().decode('utf-8', 'replace')

    position = log.stat().st_size
    started = time.monotonic()
    result = {'label': args.label, 'base': args.base, 'controls': changes, 'build': build_number}
    raw = ''
    try:
        write_controls(changes, check=True)
        # Readback proves receipt; exclude log windows preceding the last ack.
        ack_end = 0
        while changes:
            raw = tail(position)
            ends = []
            for name, value in changes.items():
                match = re.search(rf'0x{symbols[name]:08X}: 0x{bits(name,value):08X}', raw, re.I)
                if match:
                    ends.append(match.end())
            if len(ends) == len(changes):
                ack_end = max(ends)
                break
            if time.monotonic() - started > 30:
                raise TimeoutError('No control readback from PSP')
            time.sleep(0.5)
        pattern = rf'b{build_number} hwperf n=(\d+) ms=(\d+) cpu_us=(\d+) ge_wait_us=(\d+) present_us=(\d+)'
        previous = 0
        while not args.preview:
            raw = tail(position)
            matches = list(re.finditer(pattern, raw[ack_end:]))
            if len(matches) > previous:
                previous = len(matches)
                print(f'{args.label}: window {previous}/{args.samples}', flush=True)
            if len(matches) >= args.samples:
                break
            if time.monotonic() - started > 180:
                raise TimeoutError('No complete frame windows from PSP')
            time.sleep(0.5)
        if args.preview:
            time.sleep(0.75)  # let the changed frame reach the display
            matches = []
        samples = []
        for match in matches[:args.samples]:
            n, ms, cpu, ge, present = map(int, match.groups())
            samples.append(dict(frames=n, elapsed_ms=ms, frame_ms=ms/n, fps=n*1000/ms,
                                cpu_ms=cpu/1000, ge_wait_ms=ge/1000, present_ms=present/1000))
        result['samples'] = samples
        result['kept'] = samples[1:]  # first window may span the switch or screenshot
        if samples:
            result['average'] = {k: sum(s[k] for s in samples[1:])/len(samples[1:])
                                 for k in ('frame_ms','fps','cpu_ms','ge_wait_ms','present_ms')}
            print(json.dumps(result['average']), flush=True)
        if args.shot:
            # 2026-11-25: never PSPLink `scrshot` (its BMP lands at 0x88300000, over the Media Engine's program: the next
            # movie hung in sceMpegInit and the audio with it) and never `savemem` of VRAM (the game froze). b496+: the
            # game copies the next shown frame (480x272 8888) to a RAM buffer on request (rwsky.c p4_shot).
            if 'p4_shot' not in symbols:
                raise SystemExit('--shot needs b498+ (p4_shot); PSPLink scrshot corrupts the Media Engine program')
            frame = 480 * 272 * 2  # RGB565 (b498+), in video.c's low hole: savemem reads only 0x08800000-0x09FFFFFF
            destination = Path(f'C:/temp/p3f-psplink/b{build_number}') / (args.label + '.raw')
            if destination.exists() or destination.with_suffix('.png').exists():
                raise FileExistsError(destination)
            flag, buffer = symbols['p4_shot'], symbols['p4_shot_buf']
            flush = [f'echo DBG_FLUSH_SHOT_{i}_........................................' for i in range(40)]
            send([f'pokew 0x{flag:08x} 0x00000001', 'dcache w'] + flush)
            address = None
            until = time.monotonic() + 30
            while time.monotonic() < until and address is None:
                mark = log.stat().st_size
                send([f'peekw 0x{flag:08x}', f'peekw 0x{buffer:08x}'] + flush)
                time.sleep(2)
                text = tail(mark)
                state = re.search(rf'0x{flag:08X}: 0x([0-9A-F]{{8}})', text, re.I)
                at = re.search(rf'0x{buffer:08X}: 0x([0-9A-F]{{8}})', text, re.I)
                if state and int(state.group(1), 16) == 4:
                    raise RuntimeError('No scratch hole (a movie is playing?): no screenshot')
                if state and at and int(state.group(1), 16) == 2:
                    address = int(at.group(1), 16)
            if address is None:
                raise TimeoutError('The game did not copy a frame (is it drawing?)')
            send([f'savemem 0x{address:08x} 0x{frame:x} host0:/b{build_number}/{args.label}.raw'] + flush)
            until = time.monotonic() + 60
            while time.monotonic() < until:
                if destination.exists() and destination.stat().st_size == frame:
                    time.sleep(0.5)
                    from PIL import Image
                    image = Image.frombuffer('RGB', (480, 272), destination.read_bytes(), 'raw', 'BGR;16', 0, 1)
                    image.save(destination.with_suffix('.png'))
                    result['screenshot'] = str(destination.with_suffix('.png'))
                    break
                time.sleep(0.5)
            else:
                raise TimeoutError('Screenshot did not complete')
            send([f'pokew 0x{flag:08x} 0x00000003', 'dcache w'] + flush)
    except BaseException as error:
        result['error'] = str(error)
        raise
    finally:
        write_controls({name: DEFAULTS[name] for name in changes})
        result['restore_requested'] = bool(changes)
        output = build / 'experiments'
        output.mkdir(exist_ok=True)
        (output / (args.label + '.json')).write_text(json.dumps(result, indent=2))
        (output / (args.label + '.log')).write_text(raw, encoding='utf-8')


if __name__ == '__main__':
    main()

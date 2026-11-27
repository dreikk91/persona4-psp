/* b517: port buffers in the data image's VU microcode. Retail 0x64fe8c..0x707e48 (735 KB) is PS2 VU1 microcode;
   tools/psp_make_data.py blanks it (zeros in p4data.bin) and nothing on the PSP reads it: no data pointer in
   build/rebase_table.tsv, no data symbol and no code literal points into it. main() zeroes and loads the image before
   the game starts, so a buffer here starts zeroed like .bss, but must not be used before that (the log ring and the
   crash catcher run earlier and stay in .bss). What leaves .bss here goes back to the heap: PSP_IMAGE comes down by as
   much (tools/psp_make_data.py), and the heap starts after the image.
   VU_PLACE(type, name, offset): `name` is an lvalue of type `type` at that offset (sizeof works on it). Builds
   without P4_K (the audio probes) keep their own .bss copies. tools/psp_make_data.py checks the range is still VU. */
#ifndef P4_VU_PLACE_H
#define P4_VU_PLACE_H
enum {
    VU_BASE = 0x64fec0, VU_END = 0x707e40, /* retail, 64-byte aligned inside the run */
    VU_STREAMS = 0, VU_STREAMS_SIZE = 328320, /* audio.c streams */
    VU_MESH_CACHE = VU_STREAMS + VU_STREAMS_SIZE, VU_MESH_CACHE_SIZE = 229376, /* mesh.h mesh_cache */
    VU_LOCK_MEMO = VU_MESH_CACHE + VU_MESH_CACHE_SIZE, VU_LOCK_MEMO_SIZE = 53248, /* mesh.h lock_memo */
    VU_PARTITIONS = VU_LOCK_MEMO + VU_LOCK_MEMO_SIZE, VU_PARTITIONS_SIZE = 49152, /* audio_game.c partitions */
    VU_HANIM_SAVED = VU_PARTITIONS + VU_PARTITIONS_SIZE, VU_HANIM_SAVED_SIZE = 16384, /* hanim.h saved */
    VU_COL_SUB = VU_HANIM_SAVED + VU_HANIM_SAVED_SIZE, VU_COL_SUB_SIZE = 16384, /* collision.h sub */
    VU_COL_PICK = VU_COL_SUB + VU_COL_SUB_SIZE, VU_COL_PICK_SIZE = 4096, /* collision.h pick */
    VU_USED = VU_COL_PICK + VU_COL_PICK_SIZE
};
_Static_assert(VU_BASE + VU_USED <= VU_END, "VU placement past the VU run");
#ifdef P4_K
#define VU_PLACE(type, offset) (*(type *)(VU_BASE + P4_K + (offset)))
#endif
#endif

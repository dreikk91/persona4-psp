/* b514 p4_skin_disk: built skin entries (skin_geometry's GE weight groups) kept on the memory stick, so a character
   is built once on this PSP and read back in one go after that (b513 SKINT: ~30 ms a build, a read of its ~70 KB a
   few ms). Files skin/<key>.bin in the game folder. The key hashes everything the build reads: bind positions,
   normals, UVs, prelight, bone indices and weights, skinToBone, each mesh's indices, material colour and texture
   opacity, the bone count, lit/nrm, sample (skin_geometry's vertex hash XOR the build switches: sticky, regroup,
   pack, palette, strip, strip_pack, wsplit, nocolour, flat, mesh_override), a zero word (b528: was
   p4_mesh_strip_pack), p4_alpha_x2 (double_colour), the struct sizes and
   P4_SKIN_CODE: b515, a hash of the code the entry comes from (tools/psp_link.py skin_code_hash: skin.h,
   skin_disk.h, mesh.h's double_colour and normal_s8), so the files outlive builds that leave skinning alone (b514
   keyed the build number: every EBOOT built every character again).
   Only entries in the default shape are kept: every batch replaced by its weight groups and every group flat (no
   index list); far, simplified, override and capture builds always build. 0: always build, as b513.
   Included by skin.h before skin_geometry; p4_skin_disk and the counters are in rwsky.c (its report comes first). */
enum { SKIN_DISK_MAGIC = 0x4b533450 /* 'P4SK' */, SKIN_DISK_VERSION = 1, SKIN_DISK_END = 0x444e4553 /* 'SEND' */ };
extern const int p4_build_number;
extern int p4_alpha_x2;
#ifndef P4_SKIN_CODE
#define P4_SKIN_CODE 0u /* built without tools/psp_link.py's flags: the build number stands in */
#endif
static unsigned skin_disk_code(void)
{
    return P4_SKIN_CODE ? P4_SKIN_CODE : (unsigned)p4_build_number;
}

static int skin_disk_usable(void)
{
    return p4_skin_disk && !skin_bones_now && !skin_simplify_now && !p4_mesh_override && !p4_mesh_capture && !p4_batch_stats &&
           p4_skin_drop_original && p4_skin_flat;
}

static void skin_disk_hash(unsigned h[2], const void *data, unsigned bytes)
{
    const unsigned char *p = data;
    unsigned a = h[0], b = h[1];
    for (; bytes >= 4; bytes -= 4, p += 4) {
        unsigned w;
        memcpy(&w, p, 4);
        a = (a ^ w) * 16777619u, b = (b * 33) ^ w;
    }
    for (; bytes; bytes--, p++)
        a = (a ^ *p) * 16777619u, b = (b * 33) ^ *p;
    h[0] = a, h[1] = b;
}

static void skin_disk_key(const Geometry *g, const Skin *skin, int nbones, int lit, int nrm, unsigned sample, unsigned key[2])
{
    int nv = g->numVertices, nb = nbones < skin->numBones ? nbones : skin->numBones;
    unsigned h[2] = { 2166136261u, 5381 };
    unsigned head[13] = { SKIN_DISK_VERSION, skin_disk_code(), sizeof(SkinBatch), sizeof(SkinGroup), (unsigned)p4_alpha_x2,
                          0u, (unsigned)nv, (unsigned)nbones, (unsigned)lit, (unsigned)nrm, sample,
                         (unsigned)skin->maxWeights, (unsigned)g->numTexCoordSets };
    skin_disk_hash(h, head, sizeof head);
    skin_disk_hash(h, g->morphTarget->verts, nv * sizeof(V3));
    if (g->morphTarget->normals)
        skin_disk_hash(h, g->morphTarget->normals, nv * sizeof(V3));
    if (g->numTexCoordSets)
        skin_disk_hash(h, g->texCoords[0], nv * 8);
    if (g->preLit)
        skin_disk_hash(h, g->preLit, nv * 4);
    skin_disk_hash(h, skin->indices, nv * 4);
    if (skin->weights)
        skin_disk_hash(h, skin->weights, nv * 16);
    if (nb > 0)
        skin_disk_hash(h, skin->skinToBone, nb * sizeof(Matrix));
    const MeshHeader *header = g->mesh;
    const Mesh *mesh = (const Mesh *)((const char *)header + sizeof(MeshHeader) + header->firstMeshOffset);
    unsigned mh[2] = { header->flags, header->numMeshes };
    skin_disk_hash(h, mh, sizeof mh);
    for (int m = 0; m < header->numMeshes; m++) {
        void **texture = mesh[m].material->texture;
        unsigned mm[3] = { (unsigned)mesh[m].numIndices, mesh[m].material->colour,
                           (unsigned)raster_opaque(texture ? (const Raster *)*texture : 0) }; /* regroup reads these */
        skin_disk_hash(h, mm, sizeof mm);
        skin_disk_hash(h, mesh[m].indices, mesh[m].numIndices * 2);
    }
    key[0] = h[0], key[1] = h[1];
}

/* files of other skin code are never read again: the folder is emptied when skin/build holds another code hash */
static void skin_disk_prepare(void)
{
    sceIoMkdir(p4_path("skin"), 0777);
    unsigned stamp = 0, code = skin_disk_code();
    SceUID fd = sceIoOpen(p4_path("skin/build"), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        if (sceIoRead(fd, &stamp, 4) != 4)
            stamp = 0;
        sceIoClose(fd);
    }
    if (stamp == code)
        return;
    unsigned removed = 0;
    SceUID dir = sceIoDopen(p4_path("skin"));
    if (dir >= 0) {
        SceIoDirent entry;
        memset(&entry, 0, sizeof entry);
        while (sceIoDread(dir, &entry) > 0) {
            char name[64];
            if (strlen(entry.d_name) == 20 && !strcmp(entry.d_name + 16, ".bin")) { /* only our <key>.bin files */
                snprintf(name, sizeof name, "skin/%s", entry.d_name);
                removed += sceIoRemove(p4_path(name)) >= 0;
            }
            memset(&entry, 0, sizeof entry);
        }
        sceIoDclose(dir);
    }
    if ((fd = sceIoOpen(p4_path("skin/build"), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777)) >= 0) {
        sceIoWrite(fd, &code, 4);
        sceIoClose(fd);
    }
    p4_log("skin disk: code %08x (folder was %08x), %u old files removed\n", code, stamp, removed);
}

static const char *skin_disk_path(const unsigned key[2])
{
    static int made;
    if (!made)
        made = 1, skin_disk_prepare();
    char name[40];
    snprintf(name, sizeof name, "skin/%08x%08x.bin", key[0], key[1]);
    return p4_path(name);
}

/* a flat group's vertex bytes (the only shape kept) */
static int skin_disk_group_bytes(const SkinGeometry *e, const SkinGroup *gr)
{
    int c = ((gr->vtype & GU_WEIGHTS_BITS) >> 14) + 1;
    return gr->count * skin_layout_c(c, e->nrm, e->packed, !gr->nocol).stride;
}

typedef struct {
    unsigned magic, version, key[2], batches, nradius, packed, bytes, used[8], has_box;
    float pack[4];
} SkinDiskHead;

/* after a build: e as a file, if it has the kept shape. One buffer, one write: each sceIoWrite has a fixed cost. */
static void skin_disk_write(const SkinGeometry *e, const unsigned key[2])
{
    if (!e->batches)
        return;
    for (int b = 0; b < e->batches; b++) {
        const SkinBatch *k = &e->batch[b];
        if (k->v || k->strip || !k->grp)
            return;
        for (int i = 0; i < k->ngroups; i++)
            if (k->grp[i].idx || !k->grp[i].v)
                return;
    }
    unsigned t0 = sceKernelGetSystemTimeLow();
    SkinDiskHead head = { SKIN_DISK_MAGIC, SKIN_DISK_VERSION, { key[0], key[1] }, (unsigned)e->batches, (unsigned)e->nradius,
                          (unsigned)e->packed, e->bytes, { 0 }, e->box != 0, { e->pack[0], e->pack[1], e->pack[2], e->pack[3] } };
    memcpy(head.used, e->used, sizeof head.used);
    MkBuf out = { 0, 0, 0 };
    mk_put(&out, &head, sizeof head);
    if (e->nradius) {
        mk_put(&out, e->radius, e->nradius * 4);
        if (e->box)
            mk_put(&out, e->box, e->nradius * 24);
    }
    for (int b = 0; b < e->batches; b++) {
        const SkinBatch *k = &e->batch[b];
        mk_put(&out, k, sizeof *k); /* its pointers are rebuilt on reading */
        for (int i = 0; i < k->ngroups; i++) {
            mk_put(&out, &k->grp[i], sizeof k->grp[i]);
            mk_put(&out, k->grp[i].v, skin_disk_group_bytes(e, &k->grp[i]));
        }
    }
    unsigned end = SKIN_DISK_END;
    mk_put(&out, &end, 4);
    if (out.data) {
        SceUID fd = sceIoOpen(skin_disk_path(key), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            int ok = sceIoWrite(fd, out.data, out.size) == out.size;
            sceIoClose(fd);
            if (!ok)
                sceIoRemove(skin_disk_path(key)); /* a short file would only be refused later */
            skin_disk_writes += ok;
        }
    }
    free(out.data);
    skin_disk_us += sceKernelGetSystemTimeLow() - t0;
}

/* n bytes at the cursor, 0 past the end */
static const void *skin_disk_take(const unsigned char **at, const unsigned char *end, unsigned n)
{
    if (n > (unsigned)(end - *at))
        return 0;
    const void *p = *at;
    *at += n;
    return p;
}

/* e (empty, its identity fields set) filled from the file; 0 when there is none or it does not check out */
static int skin_disk_read(SkinGeometry *e, const unsigned key[2], int nbones)
{
    unsigned t0 = sceKernelGetSystemTimeLow();
    SceUID fd = sceIoOpen(skin_disk_path(key), PSP_O_RDONLY, 0);
    if (fd < 0) {
        skin_disk_misses++;
        return 0;
    }
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    unsigned char *file = size > (int)sizeof(SkinDiskHead) && size < (4 << 20) ? malloc(size) : 0;
    int ok = file && sceIoLseek32(fd, 0, PSP_SEEK_SET) == 0 && sceIoRead(fd, file, size) == size;
    sceIoClose(fd);
    const unsigned char *at = file, *end = file + (ok ? size : 0);
    const SkinDiskHead *head = ok ? skin_disk_take(&at, end, sizeof *head) : 0;
    ok = head && head->magic == SKIN_DISK_MAGIC && head->version == SKIN_DISK_VERSION && head->key[0] == key[0] &&
         head->key[1] == key[1] && head->batches > 0 && head->batches < 4096 && (int)head->nradius == nbones;
    if (ok) {
        e->packed = head->packed;
        memcpy(e->used, head->used, sizeof e->used);
        memcpy(e->pack, head->pack, sizeof e->pack);
        if (head->nradius) {
            const void *radius = skin_disk_take(&at, end, head->nradius * 4);
            const void *box = head->has_box ? skin_disk_take(&at, end, head->nradius * 24) : 0;
            e->radius = radius ? malloc(head->nradius * 4) : 0;
            e->box = box ? malloc(head->nradius * 24) : 0;
            ok = e->radius && (!head->has_box || e->box);
            if (ok) {
                memcpy(e->radius, radius, head->nradius * 4);
                if (e->box)
                    memcpy(e->box, box, head->nradius * 24);
                e->nradius = head->nradius;
            }
        }
    }
    if (ok && !(e->batch = calloc(head->batches, sizeof *e->batch)))
        ok = 0;
    for (unsigned b = 0; ok && b < head->batches; b++) {
        SkinBatch *k = &e->batch[b];
        /* records after a group's vertices may sit at any 2-byte offset: copied out, never read in place (b514:
           an unaligned word read faulted) */
        const void *in = skin_disk_take(&at, end, sizeof *k);
        if (!(ok = in != 0))
            break;
        memcpy(k, in, sizeof *k);
        int groups = k->ngroups;
        k->v = 0, k->idx = k->src = k->strip = 0, k->grp = 0, k->ngroups = 0;
        memset(k->call, 0, sizeof k->call);
        e->batches = b + 1; /* skin_free sees what is there so far */
        if (!(ok = groups > 0 && groups < 64))
            break;
        if (!(k->grp = calloc(groups, sizeof *k->grp)))
            ok = 0;
        for (int i = 0; ok && i < groups; i++) {
            SkinGroup *gr = &k->grp[i];
            const void *gin = skin_disk_take(&at, end, sizeof *gr);
            if (!(ok = gin != 0))
                break;
            memcpy(gr, gin, sizeof *gr);
            gr->v = 0, gr->idx = 0;
            k->ngroups = i + 1;
            int bytes = skin_disk_group_bytes(e, gr);
            const void *v = bytes > 0 ? skin_disk_take(&at, end, bytes) : 0;
            ok = v && (gr->v = memalign(16, bytes));
            if (ok) {
                memcpy(gr->v, v, bytes);
                sceKernelDcacheWritebackRange(gr->v, bytes);
            }
        }
    }
    const void *tail = ok ? skin_disk_take(&at, end, 4) : 0;
    unsigned end_word = 0;
    if (tail)
        memcpy(&end_word, tail, 4);
    ok = tail && end_word == SKIN_DISK_END && at == end;
    unsigned bytes = ok ? head->bytes : 0; /* head lives in file */
    free(file);
    if (ok) {
        e->bytes = bytes;
        skin_ge_bytes += e->bytes;
        skin_disk_hits++;
    }
    skin_disk_us += sceKernelGetSystemTimeLow() - t0;
    return ok;
}

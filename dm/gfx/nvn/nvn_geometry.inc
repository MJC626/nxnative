// General geometry replay: native VS capture -> GS compute -> ordered index
// compaction -> ordinary rendering. Included after the resource/pipeline helpers.
static NvProg *g_geo_program;
static uint64_t g_geo_indices, g_geo_index_bytes, g_geo_args;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void lc_land(void);

static int geo_cmp_index(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static id<MTLRenderPipelineState> geo_capture_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = GEO_MARK;
    id<MTLRenderPipelineState> ps = hm_get_id(&g_pso, &k);
    if (ps || hm_get_id(&g_pso_fail, &k)) return ps;
    NSError *e = nil;
    ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, nil, nil, 0) error:&e];
    if (ps) hm_put_id(&g_pso, &k, ps);
    else { hm_put_id(&g_pso_fail, &k, NSNull.null); MLOG("[RED] geometry vertex capture %s: %s", vs->key, e.localizedDescription.UTF8String); }
    return ps;
}
// Transform feedback of a geometry shader (Xenoblade Chronicles 3's GPU particles: c01e3ade8e15a842 emits up to 16 points per input point and captures 11 outputs, 172 bytes a point, which
// DrawTransformFeedback draws later). After the geometry compute and the index compaction, one thread copies the emitted vertices in emission order into feedback buffer 0, from the byte
// count in the game's control buffer on, and leaves there the new count (+0) and the indirect arguments of DrawTransformFeedback (+16: vertices, 1, 0, 0; tf_auto). The emitted vertex record is
// position (4 words), point size (1), then 4 words per output location in increasing order (Ryujinx's FillIoOffsetMap); a shader writing clip distances, a layer or other locations than the
// ones it captures has a different record, which is not mapped (a red line, nothing captured)
static const char k_geo_tf_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct geo_tf { device const uint *src; device const uint *idx; device const uint *args; device uint *dst; device uint *ctl; uint stride_src, stride_dst, nout, cap; uint off[32]; uint cnt[32]; };\n"
"kernel void geo_tf_capture(constant geo_tf& j [[buffer(0)]]) {\n"
" uint n = j.args[0], at = j.ctl[0] / 4, room = at < j.cap ? (j.cap - at) / j.stride_dst : 0; if (n > room) n = room;\n"
" for (uint i = 0; i < n; i++) { uint v = j.idx[i], w = at + i * j.stride_dst, k = 0;\n"
"  for (uint o = 0; o < j.nout; o++) for (uint c = 0; c < j.cnt[o]; c++) j.dst[w + k++] = j.src[v * j.stride_src + j.off[o] + c]; }\n"
" uint bytes = j.ctl[0] + n * j.stride_dst * 4; j.ctl[0] = bytes; j.ctl[4] = bytes / (j.stride_dst * 4); j.ctl[5] = 1; j.ctl[6] = 0; j.ctl[7] = 0;\n"
"}\n";
static void geo_tf(NvShader *base, NvShader *gs, uint64_t output, uint64_t packed, uint64_t args) {
    static id<MTLComputePipelineState> ps;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_geo_tf_msl) options:nil error:&e];
        if (lib) ps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_tf_capture"] error:&e];
        if (!ps) { RED_ONCE(112, "geometry transform feedback kernel: %s", e.localizedDescription.UTF8String); return; } }
    int words = 0, sorted = 1; for (int o = 0; o < base->gtf_n; o++) { words += base->gtf_cnt[o]; if (o && base->gtf_loc[o] <= base->gtf_loc[o - 1]) sorted = 0; }
    if (!sorted || gs->geo_stride != 5u + 4u * base->gtf_n || base->gtf_stride != 4 * words) {
        { static int once; if (!once++) MLOG("[RED] geometry shader %s: its emitted vertex record (%u words) does not map onto its feedback layout (%d outputs, %u bytes): nothing captured", base->key, gs->geo_stride, base->gtf_n, base->gtf_stride); }
        return; }
    void *mb; uint64_t mo, dst = 0, ctl = 0;
    if (!S.tf[0].addr || !S.tf[0].size || !mtl_gpu_addr(S.tf[0].addr, S.tf[0].size, &mb, &mo, &dst) || !S.tf_ctl || !mtl_gpu_addr(S.tf_ctl, 32, &mb, &mo, &ctl)) {
        RED_ONCE(113, "geometry transform feedback: buffer 0 (%#llx, %llu bytes) or control buffer %#llx in no memory pool: nothing captured", (unsigned long long)S.tf[0].addr, (unsigned long long)S.tf[0].size, (unsigned long long)S.tf_ctl); return; }
    struct { uint64_t src, idx, args, dst, ctl; uint32_t stride_src, stride_dst, nout, cap, off[32], cnt[32]; } j = {output, packed, args, dst, ctl, gs->geo_stride, (uint32_t)words, base->gtf_n, (uint32_t)(S.tf[0].size / 4)};
    for (int o = 0; o < base->gtf_n; o++) { j.off[o] = 5 + 4 * o; j.cnt[o] = base->gtf_cnt[o]; }
    id<MTLBuffer> jb; size_t jo = arena_alloc(sizeof j, &jb); memcpy((uint8_t *)jb.contents + jo, &j, sizeof j);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, ps); enc_c_args(ce, jb.gpuAddress + jo, 0);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(ce);
    pend_add(S.tf[0].addr, S.tf[0].size, 4); pend_add(S.tf_ctl, 32, 4); S.tf_gpu = 1;
    { static int once; if (!once++) MLOG("transform feedback: first capture of a geometry shader (%s, %d outputs, %u bytes a vertex)", base->key, base->gtf_n, base->gtf_stride); }
    PD("T %llu tf capture geometry=%s (counted on the GPU)", g_stat_pass, base->key);
}
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr->sh[0], *base = pr->sh[2];
    if (!vs || !vs->geo_fn || !base || !base->geo_vertices || pr->sh[3] || pr->sh[4]) {
        MLOG("[RED] geometry draw cannot capture this pipeline (vertex %s, geometry %s, tessellation %d, feedback %d)", vs ? vs->key : "-", base ? base->key : "-", !!(pr->sh[3] || pr->sh[4]), S.tf_on); return;
    }
    p_draw dw = dw0 ? *dw0 : (p_draw){0}; p_draw_idx di = di0 ? *di0 : (p_draw_idx){0};
    if (di0 && (di.itype < 0 || di.itype > 2)) { MLOG("[RED] geometry index type %d is invalid", di.itype); return; }
    uint64_t ind = dw0 ? dw.ind : di.ind;
    // Indirect records and GPU-written index data need a coherent CPU snapshot
    // for primitive assembly. This is deliberately a correctness path; no such
    // wait is added to ordinary VS/FS draws.
    // DrawTransformFeedback of a geometry capture (tf_auto, g_tf_gpu_draw) does not wait: its count, written by the capture kernel (geo_tf), is read as the last finished
    // submission left it, so particles born this frame show a frame late and those gone a frame longer. Xenoblade Chronicles 3 draws its GPU particles so; waiting for each
    // (the whole submission so far committed and finished) took 3-12 ms of the replay thread per draw. The other waits are taken once per submission and GPU write epoch
    // (g_wr_epoch, as mdi_wait's multi-draws), except for vertex data the CPU converts or the GPU wrote in this submission
    int tfg = ind && g_tf_gpu_draw, hard = 0;
    int wait = (ind && !tfg) || di0 != NULL;  // CPU assembly must also see writes from earlier submissions.
    for (int i = 0; i < 16 && !tfg; i++) if (S.vbuf[i].size && pend_hits(S.vbuf[i].address, S.vbuf[i].size)) wait = hard = 1;
    for (int i = 0; i < vs->rf.nv; i++) if (vconv_wanted(&vs->rf, i, NULL, 0)) wait = hard = 1;
    g_geo_st[0]++; if (tfg) g_geo_st[3]++;
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    if (wait && !hard && g_mdi_sub != g_sub_serial && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) >= g_fence) { g_mdi_sub = g_sub_serial; g_mdi_epoch = g_sub_wr0; }   // earlier submissions finished: as a wait when this one began
    if (wait && !hard && g_mdi_epoch == g_wr_epoch && g_mdi_sub == g_sub_serial) { wait = 0; g_geo_st[2]++; }
    if (wait) { mdi_wait(); g_mdi_epoch = g_wr_epoch; g_mdi_sub = g_sub_serial; g_geo_st[1]++; if (g_mdi_ev.signaledValue < g_mdi_v) return; memset(g_vconv, 0, sizeof g_vconv); }
    if (ind) {
        uint32_t a[5] = {0}; size_t n = dw0 ? 16 : 20;
        if (!nvn_host_readable(ind, n)) { MLOG("[RED] geometry indirect arguments are not readable"); return; }
        memcpy(a, (void *)(uintptr_t)ind, n);
        if (!a[0] || !a[1]) return;
        if (a[0] > INT32_MAX || a[1] > INT32_MAX) { MLOG("[RED] geometry indirect draw exceeds the host count range"); return; }
        if (dw0) { dw.count = a[0]; dw.instances = a[1]; dw.first = a[2]; dw.base_instance = a[3]; dw.ind = dw.mdi_cnt = 0; }
        else { di.count = a[0]; di.instances = a[1]; di.ibuf += (uint64_t)a[2] << di.itype; di.base_vertex = (int32_t)a[3]; di.base_instance = a[4]; di.ind = di.mdi_cnt = 0; }
    }
    int prim = dw0 ? dw.prim : di.prim, topology = nvn_geo_topology(prim);
    int count = dw0 ? dw.count : di.count, instances = dw0 ? dw.instances : di.instances;
    uint32_t first_instance = (uint32_t)(dw0 ? dw.base_instance : di.base_instance);
    g_draw_binst = first_instance;
    if (count <= 0 || instances < 0) return;
    if (!instances) instances = 1;
    if (topology < 0 || count > (1 << 22)) { MLOG("[RED] geometry primitive %d or count %d exceeds the assembly path", prim, count); return; }
    NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
    NvShader *gs = shader_get([key stringByAppendingFormat:@"_bg%d", topology], 2);
    NvShader *reader = shader_get([key stringByAppendingString:@"_bgv"], 0);
    if (!gs || !reader) return;
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) return;
    if (!gs->geo_pso) { NSError *e = nil; gs->geo_pso = [g_dev newComputePipelineStateWithFunction:gs->fn error:&e];
        if (!gs->geo_pso) { MLOG("[RED] geometry compute %s: %s", gs->key, e.localizedDescription.UTF8String); return; } }
    if (gs->geo_pso.maxTotalThreadsPerThreadgroup < 32) { MLOG("[RED] geometry kernel cannot run a 32-thread group"); return; }
    static id<MTLComputePipelineState> pack;
    if (!pack) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_pack_msl) options:nil error:&e];
        if (lib) pack = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_indices"] error:&e];
        if (!pack) { MLOG("[RED] geometry index compaction: %s", e.localizedDescription.UTF8String); return; } }

    uint32_t *order = malloc((size_t)count * 4), *unique = malloc((size_t)count * 4);
    if (!order || !unique) { free(order); free(unique); MLOG("[RED] geometry index allocation failed"); return; }
    uint32_t lo = UINT32_MAX, hi = 0; size_t nu = 0;
    int restart = di0 && S.restart.on;
    int isz = di0 ? 1 << di.itype : 0;
    uint32_t restart_index = S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX);
    if (di0 && (di.itype < 0 || di.itype > 2 || !nvn_host_readable(di.ibuf, (uint64_t)count * isz))) {
        free(order); free(unique); MLOG("[RED] geometry index buffer is not readable"); return;
    }
    for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)i;
        if (di0) { v = 0; memcpy(&v, (const uint8_t *)(uintptr_t)di.ibuf + (size_t)i * isz, isz); }
        order[i] = v;
        if (restart && v == restart_index) continue;
        lo = MIN(lo, v); hi = MAX(hi, v); unique[nu++] = v;
    }
    if (!nu) { free(order); free(unique); return; }
    int64_t first = di0 ? (int64_t)lo + di.base_vertex : dw.first;
    uint64_t span = (uint64_t)hi - lo + 1;
    if (first < 0 || first + span > (uint64_t)UINT32_MAX + 1) { free(order); free(unique); MLOG("[RED] geometry vertex range is outside 32-bit vertex IDs"); return; }
    // Normalize only after assembly, so a restart value cannot collide with a
    // normalized vertex number. Indexed captures use a sorted unique index list.
    size_t words = nvn_geo_assemble(prim, order, count, restart, restart_index, NULL, 0);
    int width = nvn_geo_width(topology);
    uint64_t np = words == SIZE_MAX ? 0 : words / width;
    if (!np) { free(order); free(unique); return; }
    uint64_t groups = np * (uint64_t)instances * gs->geo_invocations;
    if (groups > UINT32_MAX || span * (uint64_t)instances > (256ull << 20) / (144 * 4) ||
        (uint64_t)first_instance + instances > (uint64_t)UINT32_MAX + 1) {
        free(order); free(unique); MLOG("[RED] geometry draw exceeds the capture or invocation bound"); return;
    }
    uint64_t input_size = span * instances * 144 * 4;
    uint64_t output_size = groups * gs->geo_vertices * gs->geo_stride * 4;
    uint64_t raw_size = groups * gs->geo_index_stride * 4;
    uint64_t packed_size = groups * gs->geo_vertices * (gs->geo_output == 7 ? 3 : gs->geo_output == 6 ? 2 : 1) * 4;
    uint64_t total = input_size + output_size + raw_size + packed_size + words * 4 + nu * 4 + 4096;
    if (groups > UINT32_MAX || total > (256ull << 20) || np > UINT32_MAX) {
        free(order); free(unique); MLOG("[RED] geometry draw needs %llu MiB of scratch, above the 256 MiB bound", (unsigned long long)(total >> 20)); return;
    }
    id<MTLBuffer> scratch; size_t off = arena_alloc((size_t)total, &scratch);
    uint8_t *cpu = (uint8_t *)scratch.contents + off; uint64_t gpu = scratch.gpuAddress + off;
    size_t used = 0;
    #define GEO_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    uint64_t input, output, raw, packed, map, capture_ix, info, capture_info, args, job;
    void *pinput, *poutput, *praw, *ppacked, *pmap, *pcapture_ix, *pinfo, *pcapture_info, *pargs, *pjob;
    GEO_ALLOC(input_size, input, pinput); GEO_ALLOC(output_size, output, poutput); GEO_ALLOC(raw_size, raw, praw); GEO_ALLOC(packed_size, packed, ppacked);
    GEO_ALLOC(words * 4, map, pmap); GEO_ALLOC(nu * 4, capture_ix, pcapture_ix); GEO_ALLOC(32, info, pinfo); GEO_ALLOC(32, capture_info, pcapture_info);
    GEO_ALLOC(20, args, pargs); GEO_ALLOC(40, job, pjob);
    #undef GEO_ALLOC
    (void)pinput; (void)poutput; (void)praw; (void)ppacked;
    if (used > total) MDIE("geometry scratch size accounting");
    nvn_geo_assemble(prim, order, count, restart, restart_index, pmap, words);
    for (size_t i = 0; i < words; i++) ((uint32_t *)pmap)[i] -= lo;
    qsort(unique, nu, 4, geo_cmp_index);
    size_t unique_count = 0;
    for (size_t i = 0; i < nu; i++) if (!i || unique[i] != unique[i - 1]) ((uint32_t *)pcapture_ix)[unique_count++] = unique[i];
    free(order); free(unique);
    uint32_t vi[8] = {(uint32_t)span, (uint32_t)instances, 0, 0, (uint32_t)np, 0, 0, 0}; memcpy(pinfo, vi, 32);
    uint64_t ci[2] = {input, 0}; memcpy(pcapture_info, ci, 16);
    uint32_t cc[4] = {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}; memcpy((uint8_t *)pcapture_info + 16, cc, 16);
    memset(pargs, 0, 20);
    struct { uint64_t src, dst, args; uint32_t groups, stride, topology, capacity; } pj = {raw, packed, args, (uint32_t)groups, gs->geo_index_stride, gs->geo_output, (uint32_t)(packed_size / 4)};
    memcpy(pjob, &pj, sizeof pj);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd); enc_r_pso(re, capture); bind_vbufs(re);
    p_draw vertex_range = {.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    vconv_bind(re, vs, 1, &vertex_range, NULL);
    args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, capture_info);
    if (di0) enc_r_draw_indexed(re, MTLPrimitiveTypePoint, unique_count, MTLIndexTypeUInt32, capture_ix, nu * 4, instances, di.base_vertex, first_instance);
    else enc_r_draw_instanced(re, MTLPrimitiveTypePoint, dw.first, dw.count, instances, first_instance);
    enc_r_end(re);
    g_geo_bind[0] = input; g_geo_bind[1] = output; g_geo_bind[2] = raw; g_geo_bind[3] = info; g_geo_bind[4] = map;
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL);
    args_dirty_all(); encode_args(ce, gs, 2, 0); enc_c_pso(ce, gs->geo_pso);
    enc_c_dispatch(ce, MTLSizeMake((np + 31) / 32, instances, gs->geo_invocations), MTLSizeMake(32, 1, 1)); enc_c_end(ce);
    ce = enc_c_begin(NULL); enc_c_pso(ce, pack); enc_c_args(ce, job, 0);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(ce);
    if (S.tf_on && base->gtf_n) geo_tf(base, gs, output, packed, args);   // a capture (geo_tf); a geometry shader without a feedback layout is drawn as usual while a capture is active
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    swrite_note(pr); udc_note_draw(pr); g_wr_epoch++; g_mdx_epoch++;
    if (S.rast_discard) return;
    NvProg *raster = pr->geo_raster;
    if (!raster || raster->sh[0] != reader || raster->sh[1] != pr->sh[1]) {
        raster = [NvProg new]; raster->sh[0] = reader; raster->sh[1] = pr->sh[1]; pr->geo_raster = raster;
    }
    g_geo_program = raster; g_geo_indices = packed; g_geo_index_bytes = packed_size; g_geo_args = args;
    p_draw synthetic = {.prim = gs->geo_output == 1 ? 0 : gs->geo_output == 6 ? 1 : 4, .count = 3, .instances = 1, .base_instance = (int32_t)first_instance};
    draw(&synthetic, NULL);
    g_geo_program = nil; args_dirty_all();
    static int once; if (!once++) MLOG("geometry compute: vertex %s, geometry %s, %llu input primitives, %d instances", vs->key, gs->key, (unsigned long long)np, instances);
}

/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ir3c: a TGSI shader (the text libzgl's GLSL compiler and virgl speak) in,
 * an Adreno A6xx shader out: Mesa's tgsi_to_nir and ir3, set up the way
 * its gallium freedreno driver does, for the A610 of a "creek" phone.
 *   ir3c shader.tgsi [out.bin]
 * prints the disassembly and what the variant needs (registers, consts),
 * and writes the instructions to out.bin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pipe/p_shader_tokens.h"
#include "pipe/p_screen.h"
#include "tgsi/tgsi_text.h"
#include "nir/tgsi_to_nir.h"
#include "compiler/glsl_types.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "ir3/ir3_nir.h"
#include "common/freedreno_dev_info.h"
#include "isa/ir3-isa.h"

/* What tgsi_to_nir asks of a screen: the caps freedreno's has, and its NIR finalize. */
static struct ir3_compiler *compiler;

static void finalize(struct pipe_screen *ps, struct nir_shader *nir, bool optimize)
{
    (void)ps; (void)optimize;
    const struct ir3_shader_nir_options options = {0};
    if (!nir->info.io_lowered) {
        ir3_nir_lower_io_vars_to_temporaries(nir);
        ir3_nir_lower_io(nir);
    }
    ir3_finalize_nir(compiler, &options, nir);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ir3c shader.tgsi [out.bin]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static char text[1 << 20];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    static struct tgsi_token toks[16384];
    if (!tgsi_text_translate(text, toks, 16384)) { fprintf(stderr, "ir3c: %s: not TGSI it can read\n", argv[1]); return 1; }

    glsl_type_singleton_init_or_ref();
    struct fd_dev_id id = { .gpu_id = 610, .chip_id = 0x06010001 };
    const struct fd_dev_info *info = fd_dev_info_raw(&id);
    if (!info) { fprintf(stderr, "ir3c: no A610 in Mesa's device table\n"); return 1; }
    struct ir3_compiler_options copts = { .lower_base_vertex = true };
    compiler = ir3_compiler_create(NULL, &id, info, &copts);

    static struct pipe_screen screen;
    struct pipe_caps *caps = (struct pipe_caps *)&screen.caps;     /* const to the driver's users; this is the driver */
    caps->fs_position_is_sysval = caps->fs_face_is_integer_sysval = caps->fs_point_is_sysval = true;
    for (size_t i = 0; i < sizeof screen.nir_options / sizeof screen.nir_options[0]; i++) {
        ((struct pipe_shader_caps *)&screen.shader_caps[i])->integers = true;
        screen.nir_options[i] = ir3_get_compiler_options(compiler);
    }
    screen.finalize_nir = finalize;
    nir_shader *nir = tgsi_to_nir(toks, &screen, false);

    struct ir3_shader *sh = ir3_shader_from_nir(compiler, nir, &(struct ir3_shader_options){
        .api_wavesize = IR3_SINGLE_ONLY, .real_wavesize = IR3_SINGLE_ONLY });
    struct ir3_shader_key key = {0};
    struct ir3_shader_variant *v = ir3_shader_get_variant(sh, &key, false, false, NULL, NULL);
    if (!v || !v->bin) { fprintf(stderr, "ir3c: %s did not compile\n", argv[1]); return 1; }

    struct isa_decode_options dopts = { .gpu_id = 610, .show_errors = true, .branch_labels = true };
    ir3_isa_disasm(v->bin, (int)v->info.size, stdout, &dopts);
    printf("; %s shader: %u bytes, %u instructions, %u full registers, %u consts (vec4)\n",
           ir3_shader_stage(v), v->info.size, v->info.instrs_count, v->info.max_reg + 1, v->constlen);
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        if (!o || fwrite(v->bin, 1, v->info.size, o) != v->info.size) { perror(argv[2]); return 1; }
        fclose(o);
    }
    return 0;
}

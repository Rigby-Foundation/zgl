/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* vtrans: TGSI text in, the GLSL virglrenderer would run on the host out.
 * Host-side test tooling for zgl's shader compiler: it runs our TGSI through
 * virglrenderer's own parser (tgsi_text) and translator (vrend_shader), the
 * exact path a guest shader takes, so a shader that fails here would fail on
 * the host too. Not part of zgl; built against a virglrenderer checkout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_sanity.h"
#include "vrend_shader.h"

/* the bits of vrend's debug layer vrend_shader.c reaches for */
unsigned vrend_debug(const struct vrend_context *ctx, enum virgl_debug_flags flag) { (void)ctx; (void)flag; return 0; }
void vrend_print_context_name(const struct vrend_context *ctx) { (void)ctx; }
int vrend_debug_can_override(void) { return 0; }

static void base_cfg(struct vrend_shader_cfg *cfg, int glsl_version)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->glsl_version = glsl_version;
    cfg->max_draw_buffers = 8;
    cfg->max_shader_patch_varyings = 30;
    cfg->use_core_profile = 1;
    cfg->use_explicit_locations = glsl_version >= 330;
    cfg->use_integer = 1;
    cfg->has_nopersective = 1;
}

static char *join(struct vrend_strarray *sa)
{
    size_t n = 1;
    for (int i = 0; i < sa->num_strings; i++) n += sa->strings[i].size;
    char *out = malloc(n), *p = out;
    for (int i = 0; i < sa->num_strings; i++) { memcpy(p, sa->strings[i].buf, sa->strings[i].size); p += sa->strings[i].size; }
    *p = 0;
    return out;
}

/* One shader, with the key the renderer would build: the fragment shader
 * drawing into an ordinary (not y_0_top) framebuffer; the vertex shader
 * linked to that fragment shader (its interpolation and inputs). */
static char *translate(const char *tgsi, int glsl_version, int is_fs, struct vrend_shader_info *fs_sinfo,
                       struct vrend_variable_shader_info *fs_vinfo, char *err, size_t errsz)
{
    static struct tgsi_token tokens[16384];
    if (!tgsi_text_translate(tgsi, tokens, 16384)) { snprintf(err, errsz, "tgsi_text_translate failed"); return NULL; }
    if (!tgsi_sanity_check(tokens)) { snprintf(err, errsz, "tgsi_sanity_check failed"); return NULL; }
    struct vrend_shader_cfg cfg;
    base_cfg(&cfg, glsl_version);
    struct vrend_shader_key key;
    memset(&key, 0, sizeof key);
    if (is_fs) key.fs.lower_left_origin = 1;
    else if (fs_sinfo) {
        key.fs_info = fs_vinfo->fs_info;
        key.out_generic_expected_mask = fs_sinfo->in_generic_emitted_mask;
        key.out_texcoord_expected_mask = fs_sinfo->in_texcoord_emitted_mask;
    }
    struct vrend_shader_info sinfo, *si = is_fs && fs_sinfo ? fs_sinfo : &sinfo;
    struct vrend_variable_shader_info vinfo, *vi = is_fs && fs_vinfo ? fs_vinfo : &vinfo;
    memset(si, 0, sizeof *si);
    memset(vi, 0, sizeof *vi);
    struct vrend_strarray sa;
    memset(&sa, 0, sizeof sa);
    strarray_alloc(&sa, SHADER_MAX_STRINGS);
    if (!vrend_convert_shader(NULL, &cfg, tokens, 0, &key, si, vi, &sa)) { snprintf(err, errsz, "vrend_convert_shader failed"); return NULL; }
    return join(&sa);
}

/* Translate one shader on its own. Returns malloc'd GLSL or NULL. */
char *vtrans(const char *tgsi, int glsl_version, char *err, size_t errsz)
{
    return translate(tgsi, glsl_version, strncmp(tgsi, "FRAG", 4) == 0, NULL, NULL, err, errsz);
}

/* A linked pair, as the renderer does it: the fragment shader first, its
 * input layout in the vertex shader's key. */
int vtrans_pair(const char *vs_tgsi, const char *fs_tgsi, int glsl_version, char **vs_glsl, char **fs_glsl, char *err, size_t errsz)
{
    static struct vrend_shader_info fs_sinfo;
    static struct vrend_variable_shader_info fs_vinfo;
    *vs_glsl = *fs_glsl = NULL;
    *fs_glsl = translate(fs_tgsi, glsl_version, 1, &fs_sinfo, &fs_vinfo, err, errsz);
    if (!*fs_glsl) return -1;
    *vs_glsl = translate(vs_tgsi, glsl_version, 0, &fs_sinfo, &fs_vinfo, err, errsz);
    return *vs_glsl ? 0 : -1;
}

#ifdef VTRANS_MAIN
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vtrans file.tgsi [glsl_version]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    char err[256];
    char *glsl = vtrans(buf, argc > 2 ? atoi(argv[2]) : 410, err, sizeof err);
    if (!glsl) { fprintf(stderr, "vtrans: %s\n", err); return 1; }
    fputs(glsl, stdout);
    return 0;
}
#endif

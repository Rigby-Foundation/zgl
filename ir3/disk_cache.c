/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Rigby Foundation */
/* ir3's shader cache on disk, which sic has no use for (and Mesa's keys it
 * on the build-id of a shared library): never a hit, nothing stored. In
 * place of Mesa's ir3_disk_cache.c. */
#include "ir3_compiler.h"
#include "ir3_shader.h"

void ir3_disk_cache_init(struct ir3_compiler *compiler) { compiler->disk_cache = NULL; }

void ir3_disk_cache_init_shader_key(struct ir3_compiler *compiler, struct ir3_shader *shader)
{
    (void)compiler; (void)shader;
}

bool ir3_disk_cache_retrieve(struct ir3_shader *shader, struct ir3_shader_variant *v)
{
    (void)shader; (void)v;
    return false;
}

void ir3_disk_cache_store(struct ir3_shader *shader, struct ir3_shader_variant *v)
{
    (void)shader; (void)v;
}

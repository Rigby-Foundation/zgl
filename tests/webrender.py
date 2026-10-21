#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Rigby Foundation
"""Compile WebRender's shaders through zgl, the way WebRender builds them.

    tests/webrender.py RES_DIR [--features all] [-v]

RES_DIR is gfx/wr/webrender/res from a Firefox tree. Each shader is put
together as WebRender's device code does it (the #version line, the stage
define, WR_MAX_VERTEX_TEXTURE_WIDTH, the WR_FEATURE_ defines, then the source
with its `#include a,b` lines expanded once each) for GLSL 1.50 and ES 3.00,
then compiled and linked by vrend/build/glsltest --link: zgl, virglrenderer's
translation and the host's GL."""
import os, re, subprocess, sys, tempfile, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
GLSLTEST = os.path.join(HERE, "vrend", "build", "glsltest")

def expand(res, name, seen):
    if name in seen:
        return ""
    seen.add(name)
    out = []
    for line in open(os.path.join(res, name + ".glsl")):
        m = re.match(r"\s*#include\s+([\w,]+)", line)
        if m:
            for inc in m.group(1).split(","):
                out.append(expand(res, inc.strip(), seen))
        else:
            out.append(line)
    return "".join(out)

def build(res, name, stage, version, features):
    pre = "#version 300 es\n" if version == "es" else "#version 150\n"
    pre += "// shader: %s %s\n" % (name, ",".join(features))
    pre += "#define WR_%s_SHADER\n" % ("VERTEX" if stage == "vs" else "FRAGMENT")
    pre += "#define WR_MAX_VERTEX_TEXTURE_WIDTH 1024U\n"
    for f in features:
        pre += "#define WR_FEATURE_%s\n" % f
    return pre + expand(res, name, set())

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    res = args[0]
    # the shaders WebRender builds: files with a main() that nothing includes
    included = set()
    for f in os.listdir(res):
        if f.endswith(".glsl"):
            for m in re.finditer(r"^\s*#include\s+([\w,]+)", open(os.path.join(res, f)).read(), re.M):
                included.update(x.strip() for x in m.group(1).split(","))
    shaders = sorted(f[:-5] for f in os.listdir(res) if f.endswith(".glsl") and f[:-5] not in included and "void main" in open(os.path.join(res, f)).read())
    feature_sets = [[], ["ALPHA_PASS"], ["TEXTURE_2D"], ["ALPHA_PASS", "TEXTURE_2D"], ["TEXTURE_RECT"],
                    ["REPETITION", "ANTIALIASING"], ["FAST_PATH"], ["DITHERING"], ["GLYPH_TRANSFORM"],
                    ["DUAL_SOURCE_BLENDING"], ["DEBUG_OVERDRAW"], ["YUV"]]
    ok = bad = 0
    failures = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name in shaders:
            src_all = open(os.path.join(res, name + ".glsl")).read()
            for version in ("150", "es"):
                for feats in feature_sets:
                    # a texture type is always chosen (the shared samplers exist only with one)
                    if "TEXTURE_RECT" not in feats and "TEXTURE_2D" not in feats:
                        feats = feats + ["TEXTURE_2D"]
                    vs = build(res, name, "vs", version, feats)
                    fs = build(res, name, "fs", version, feats)
                    vp, fp = os.path.join(tmp, "v.glsl"), os.path.join(tmp, "f.glsl")
                    open(vp, "w").write(vs); open(fp, "w").write(fs)
                    r = subprocess.run([GLSLTEST, "--link", vp, fp], capture_output=True, text=True)
                    line = (r.stdout.strip().splitlines() or ["?"])[0]
                    tag = "%s %s [%s]" % (name, version, ",".join(feats))
                    if r.returncode == 0:
                        ok += 1
                        if verbose: print("ok  ", tag, line)
                    else:
                        bad += 1
                        failures.setdefault(line, []).append(tag)
                        if verbose: print("FAIL", tag, line)
    print("%d compiled and linked, %d failed" % (ok, bad))
    for msg, tags in sorted(failures.items(), key=lambda kv: -len(kv[1])):
        print("\n%dx %s\n    e.g. %s" % (len(tags), msg, "; ".join(tags[:3])))
    return 1 if bad else 0

sys.exit(main())

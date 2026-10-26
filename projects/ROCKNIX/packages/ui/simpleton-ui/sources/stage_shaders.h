/*
 * SimpletonOS UI - Soundstage's shaders (0.31; the glow 0.39, 25 Oct 2026).
 * In their own header so the rig's preview (rig/glowprev.c) draws exactly
 * what the unit does. One quad per bulb, u_R bulb radii to its edge (1.7
 * plain, 3.4 with the glow on, so the halo's tail has room); the fragment
 * shader works in bulb radii: the bulb itself to 1.0, a tight halo, and
 * with u_glow > 0 a wide soft bloom (u_glow 1 soft, 2 bright) that the
 * additive blend piles up where lit bulbs sit close. Unlit bulbs have no
 * halo at all.
 */
#ifndef SIMPLETON_STAGE_SHADERS_H
#define SIMPLETON_STAGE_SHADERS_H

static const char * VS =
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
    "varying vec2 v_uv; varying vec4 v_col;\n"
    /* GL counts rows from the bottom, the display from the top: the frame goes straight to the screen, so flip (0.31d) */
    "void main() { v_uv = vec2(a_uv.x, -a_uv.y); v_col = a_col; gl_Position = vec4(a_pos.x, -a_pos.y, 0.0, 1.0); }\n";
static const char * FS =
    "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
    "uniform float u_R; uniform float u_glow;\n"
    "void main() {\n"
    "  float d = length(v_uv) * u_R;\n"                             /* in bulb radii */
    "  float core = 1.0 - smoothstep(0.98, 1.06, d);\n"             /* the bulb */
    "  float halo = exp(-d * d * 1.38) * 0.55;\n"                   /* the tight halo it always had */
    "  float edge = 1.0 - smoothstep(0.75, 1.0, length(v_uv));\n"    /* fades to nothing at the quad's edge: no square */
    "  float bloom = u_glow > 0.0 ? exp(-d * d * (0.36 - 0.08 * u_glow)) * 0.14 * u_glow * u_glow * edge : 0.0;\n"   /* the wide soft one: soft 0.14, bright 0.56 and wider */
    "  vec2 h = v_uv * u_R - vec2(-0.34, 0.374);\n"
    "  float hl = exp(-dot(h, h) * 10.4) * 0.6;\n"                  /* the highlight */
    "  float b = v_col.a;\n"
    "  vec3 c = v_col.rgb * (core * (0.08 + 0.92 * b) + (halo + bloom) * b) + vec3(hl * b * core);\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

#endif

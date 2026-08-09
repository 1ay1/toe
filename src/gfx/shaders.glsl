// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe's terminal shaders in sokol-shdc annotated GLSL. Compiled by sokol-shdc
// into per-backend shaders (Metal/HLSL/GLSL/WGSL) + C reflection, so the same
// source drives every GPU API. Two programs:
//   * cell  — the instanced background-rect / glyph-quad pass (rounded-box SDF,
//             gamma-corrected coverage, 3-way is_glyph: rect / alpha / colour).
//   * image — the inline-image (kitty graphics) textured-quad pass.

// ─────────────────────────────── cell program ──────────────────────────────
@vs cell_vs
layout(binding=0) uniform cell_vs_params { vec4 uScreen; }; // xy = surface px, zw = origin (padding) px

in vec2 aCorner;   // unit quad corner (0..1)
in vec4 aRect;     // x,y,w,h in pixels
in vec4 aUV;       // u0,v0,u1,v1
in vec3 aColor;
// Packed flags: .x = mode (0 rect, 1 alpha glyph, 2 colour glyph, 3 SDF shape),
// .y = corner radius px, .z = SDF shape id, .w = spare. UBYTE4N -> [0,1]; the
// shader scales each channel back to its raw u8.
in vec4 aFlags;

out vec2 vUV;
out vec3 vColor;
out float vIsGlyph;
out float vShape;  // SDF shape id (when vIsGlyph == 3)
out vec2 vLocal;   // position within the rect, centred, in px
out vec2 vHalf;    // half-extent of the rect, in px
out float vRadius;

void main() {
    // Shift every rect by the padding origin (uScreen.zw), then px -> NDC over
    // the FULL surface (uScreen.xy). This insets the whole terminal by the
    // configured window padding with no per-vertex work at the emit sites.
    vec2 px = uScreen.zw + aRect.xy + aCorner * aRect.zw;
    vec2 ndc = vec2((px.x / uScreen.x) * 2.0 - 1.0,
                    1.0 - (px.y / uScreen.y) * 2.0); // y-down px -> y-up ndc
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = mix(aUV.xy, aUV.zw, aCorner);
    vColor = aColor;
    // aFlags arrive UBYTE4N-normalized ([0,1]); recover the raw u8 per channel.
    vIsGlyph = aFlags.x * 255.0;
    vRadius  = aFlags.y * 255.0;
    vShape   = aFlags.z * 255.0;
    vHalf = aRect.zw * 0.5;
    vLocal = (aCorner - 0.5) * aRect.zw;
}
@end

@fs cell_fs
layout(binding=0) uniform texture2D uAtlas;       // R8 coverage (alpha glyphs)
layout(binding=1) uniform texture2D uColorAtlas;  // RGBA (colour emoji)
layout(binding=0) uniform sampler uSmp;
layout(binding=1) uniform cell_fs_params { float uOpacity; }; // window opacity (bg only)

in vec2 vUV;
in vec3 vColor;
in float vIsGlyph;
in float vShape;
in vec2 vLocal;
in vec2 vHalf;
in float vRadius;
out vec4 frag;

float sd_round_box(vec2 p, vec2 half_ext, float r) {
    vec2 q = abs(p) - half_ext + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Signed distance to a triangle (a,b,c). Negative inside. From iq.
float sd_triangle(vec2 p, vec2 a, vec2 b, vec2 c) {
    vec2 e0 = b - a, e1 = c - b, e2 = a - c;
    vec2 v0 = p - a, v1 = p - b, v2 = p - c;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                     vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// Signed distance to an ANNULUS quarter arc (a rounded box-drawing corner):
// the ring of radius `r` and thickness `t`, centred at `ctr`. Only the quadrant
// facing (sx,sy) is kept, so ╭╮╰╯ are one function with a sign flip.
float sd_arc(vec2 p, vec2 ctr, float r, float t, vec2 quad) {
    vec2 d = p - ctr;
    float ring = abs(length(d) - r) - t * 0.5;   // distance to the ring band
    // Clip to the facing quadrant with two half-plane cuts (the arms).
    float cutx = quad.x * d.x;   // keep where d.x has the quadrant's sign
    float cuty = quad.y * d.y;
    return max(ring, -min(cutx, cuty));
}

// Evaluate the analytic SDF for shape id `sh` at local px `p`. Returns signed
// distance (negative = inside). The whole point: these render mathematically
// PERFECT and antialiased at ANY cell size / zoom, with no atlas + no memory.
float sdf_shape(int sh, vec2 p, vec2 hf) {
    float t = min(hf.x, hf.y) * 0.5;              // line/ring thickness
    if (sh == 1) return sd_triangle(p, vec2(-hf.x,-hf.y), vec2(hf.x,0.0), vec2(-hf.x,hf.y));  //  right-filled
    if (sh == 2) return sd_triangle(p, vec2(hf.x,-hf.y), vec2(-hf.x,0.0), vec2(hf.x,hf.y));   //  left-filled
    // Powerline chevron arrows ( ): the triangle minus an inset triangle.
    if (sh == 3) { float o=t; return max(sd_triangle(p, vec2(-hf.x,-hf.y), vec2(hf.x,0.0), vec2(-hf.x,hf.y)),
                                          -sd_triangle(p, vec2(-hf.x+o,-hf.y+o), vec2(hf.x-o,0.0), vec2(-hf.x+o,hf.y-o))); }
    if (sh == 4) { float o=t; return max(sd_triangle(p, vec2(hf.x,-hf.y), vec2(-hf.x,0.0), vec2(hf.x,hf.y)),
                                          -sd_triangle(p, vec2(hf.x-o,-hf.y+o), vec2(-hf.x+o,0.0), vec2(hf.x-o,hf.y-o))); }
    // Rounded corners as true quarter arcs. r = half the cell so the arc's
    // straight arms meet the neighbouring lines exactly at the cell edge.
    float r = min(hf.x, hf.y);
    float lw = min(hf.x, hf.y) * 0.25; // line width ~ 1/8 cell (matches kLight)
    if (sh == 5) return sd_arc(p, vec2( hf.x,  hf.y), r, lw, vec2(-1.0,-1.0)); // ╭ TL
    if (sh == 6) return sd_arc(p, vec2(-hf.x,  hf.y), r, lw, vec2( 1.0,-1.0)); // ╮ TR
    if (sh == 7) return sd_arc(p, vec2( hf.x, -hf.y), r, lw, vec2(-1.0, 1.0)); // ╰ BL
    if (sh == 8) return sd_arc(p, vec2(-hf.x, -hf.y), r, lw, vec2( 1.0, 1.0)); // ╯ BR
    return 1.0; // unknown shape -> empty
}

void main() {
    if (vIsGlyph > 2.5) {
        // Analytic SDF glyph (Powerline separators, rounded arcs). One crisp,
        // antialiased shape evaluated per pixel — resolution-independent, atlas-
        // free, and composited in the same instanced draw as everything else.
        float d = sdf_shape(int(vShape + 0.5), vLocal, vHalf);
        float aa = fwidth(d) + 1e-4;
        float a = 1.0 - smoothstep(-aa, aa, d);
        frag = vec4(vColor, a);
    } else if (vIsGlyph > 1.5) {
        // Colour emoji: sample the RGBA atlas straight; standard alpha blend.
        frag = texture(sampler2D(uColorAtlas, uSmp), vUV);
    } else if (vIsGlyph > 0.5) {
        float a = texture(sampler2D(uAtlas, uSmp), vUV).r;
        // Luminance-aware coverage gamma. HW alpha blending runs in the frame-
        // buffer's non-linear sRGB space, which makes light text on a dark bg
        // look too THIN and (less so) dark text on light look too heavy. So we
        // pick the correction by the glyph's own luminance: bright glyphs get a
        // stronger gamma (fatten stems), dark glyphs a gentle one. This is the
        // cheap, single-pass approximation of linear-space blending that kitty/
        // ghostty use. a==0/a==1 stay fixed points so solids are untouched.
        float luma = dot(vColor, vec3(0.2126, 0.7152, 0.0722));
        // gamma in ~[1.2 .. 1.6] as luma goes 0->1.
        float g = mix(1.2, 1.6, luma);
        a = pow(a, 1.0 / g);
        frag = vec4(vColor, a);
    } else {
        // Solid cell background. Window opacity scales its alpha so a semi-
        // transparent terminal shows the desktop through the BACKGROUND while
        // glyphs (above) stay fully opaque and readable.
        if (vRadius > 0.0) {
            float d = sd_round_box(vLocal, vHalf, vRadius);
            float a = 1.0 - smoothstep(-0.75, 0.75, d);
            frag = vec4(vColor, a * uOpacity);
        } else {
            frag = vec4(vColor, uOpacity);
        }
    }
}
@end

@program cell cell_vs cell_fs

// ─────────────────────────────── image program ─────────────────────────────
@vs image_vs
layout(binding=0) uniform image_vs_params { vec2 uScreen; vec4 uRect; };

in vec2 aCorner;  // unit quad corner
out vec2 vUV;

void main() {
    vec2 px = uRect.xy + aCorner * uRect.zw;
    vec2 ndc = vec2((px.x / uScreen.x) * 2.0 - 1.0,
                    1.0 - (px.y / uScreen.y) * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aCorner;
}
@end

@fs image_fs
layout(binding=0) uniform texture2D uTex;
layout(binding=0) uniform sampler uImgSmp;
in vec2 vUV;
out vec4 frag;
void main() { frag = texture(sampler2D(uTex, uImgSmp), vUV); }
@end

@program image image_vs image_fs

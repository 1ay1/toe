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
in float aIsGlyph; // 0 rect, 1 alpha glyph, 2 colour glyph (u8-normalized, *255 below)
in float aRadius;  // corner radius in px (rects only) (u8-normalized, *255 below)

out vec2 vUV;
out vec3 vColor;
out float vIsGlyph;
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
    // aIsGlyph / aRadius arrive UBYTE4N-normalized ([0,1]); recover raw u8.
    vIsGlyph = aIsGlyph * 255.0;
    vHalf = aRect.zw * 0.5;
    vLocal = (aCorner - 0.5) * aRect.zw;
    vRadius = aRadius * 255.0;
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
in vec2 vLocal;
in vec2 vHalf;
in float vRadius;
out vec4 frag;

float sd_round_box(vec2 p, vec2 half_ext, float r) {
    vec2 q = abs(p) - half_ext + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (vIsGlyph > 1.5) {
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

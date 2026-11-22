const SHARPNESS: f32 = 0.14;
// Smooth edges: how slanted an edge must be (smaller direction part / larger one) and how far along it the blend may reach.
const EDGE_SLANT: f32 = 0.04;
const EDGE_REACH: f32 = 6.0;

struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    blend: u32,
    out_width: u32,
    out_height: u32,
    options: u32,
    brightness: f32,
    contrast: f32,
    saturation: f32,
    gamma: f32,
    scanlines: f32,
    ssao: f32,
    bloom: f32,
    ssgi: f32,
    vignette: f32,
    filmic: u32,
    depth_width: u32,
    depth_height: u32,
    depth_valid: u32,
    use_hw_tex: u32,
    use_depth_tex: u32,
    photo_active: u32,
    photo_pan_x: f32,
    photo_pan_y: f32,
    photo_zoom: f32,
    photo_roll_rad: f32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;
@group(0) @binding(2) var<storage, read> depth_buf: array<f32>;
@group(0) @binding(3) var tex_cur: texture_2d<f32>;
@group(0) @binding(4) var tex_prev: texture_2d<f32>;
@group(0) @binding(5) var lin_samp: sampler;
@group(0) @binding(6) var depth_tex: texture_2d<f32>;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn texel_buf(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[base + cy * blit.width + cx]);
}

// Fast 2D Morton-cached hardware texture read when available, falling back to storage buffer for non-64px widths.
fn scene_texel(x: i32, y: i32) -> vec3<f32> {
    if (blit.use_hw_tex != 0u) {
        let p = vec2<i32>(clamp(x, 0, i32(blit.width) - 1), clamp(y, 0, i32(blit.height) - 1));
        var c = textureLoad(tex_cur, p, 0).rgb;
        if (blit.blend != 0u) { c = (c + textureLoad(tex_prev, p, 0).rgb) * 0.5; }
        return c;
    }
    var c = texel_buf(blit.current, x, y).rgb;
    if (blit.blend != 0u) { c = (c + texel_buf(blit.previous, x, y).rgb) * 0.5; }
    return c;
}

// Hardware TMU bilinear scene sample (1 TMU instruction per field instead of 4 storage reads + 4 unpacks + 3 mixes).
fn scene_linear(uv: vec2<f32>) -> vec3<f32> {
    if (blit.use_hw_tex != 0u) {
        var c = textureSampleLevel(tex_cur, lin_samp, uv, 0.0).rgb;
        if (blit.blend != 0u) { c = (c + textureSampleLevel(tex_prev, lin_samp, uv, 0.0).rgb) * 0.5; }
        return c;
    }
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let p = uv * size - 0.5;
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let top = mix(scene_texel(x, y), scene_texel(x + 1, y), f.x);
    let bottom = mix(scene_texel(x, y + 1), scene_texel(x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

// Three ways to enlarge the picture to the window (bit 2 below). Sharp (the default of bit 1 = 0): every PS2 pixel stays a hard square and only
// its border blends, the way an emulator shows it. Soft (bit 1): a plain smooth blend between neighbouring pixels everywhere,
// which is closest to what a TV did to the picture. Sharp (bit 2): see sharp_scene.
fn scene(uv: vec2<f32>) -> vec3<f32> {
    if ((blit.options & 6u) != 0u) {
        return scene_linear(uv);
    }
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let t = uv * size;
    let scale = max(floor(vec2<f32>(f32(blit.out_width), f32(blit.out_height)) / size), vec2<f32>(1.0));
    let d = fract(t) - 0.5;
    let region = 0.5 - 0.5 / scale;
    let crisp_uv = (floor(t) + 0.5 + (d - clamp(d, -region, region)) * scale) / size;
    return scene_linear(crisp_uv);
}

// Catmull-Rom weights (the cubic that passes through every source pixel and has a little overshoot at edges) for the four source
// pixels at offsets -1, 0, 1, 2 around a position with fraction f. Published in 1974; this is the textbook polynomial.
fn catmull_rom(f: f32) -> vec4<f32> {
    let f2 = f * f;
    let f3 = f2 * f;
    return vec4<f32>(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}

// Bilinear weights over the four source columns (or rows) -1, 0, 1, 2 around the pixel at floor(p), for a position u (in pixels
// from floor(p), between -1 and 2).
fn linear_weights(u: f32) -> vec4<f32> {
    let b = floor(u);
    let t = u - b;
    if (b < 0.0) { return vec4<f32>(1.0 - t, t, 0.0, 0.0); }
    if (b < 1.0) { return vec4<f32>(0.0, 1.0 - t, t, 0.0); }
    return vec4<f32>(0.0, 0.0, 1.0 - t, t);
}

fn row4(a: vec3<f32>, b: vec3<f32>, c: vec3<f32>, d: vec3<f32>, w: vec4<f32>) -> vec3<f32> {
    return a * w.x + b * w.y + c * w.z + d * w.w;
}

// Sharp look: a Catmull-Rom cubic enlargement (text and the health bar keep their edges, unlike a plain blend), then a light
// contrast-adaptive sharpening: the pixel is pushed away from the average of its four neighbours on the screen, less where the
// neighbourhood is already near black or white (so that edges do not get halos). Written from the public description of the idea
// (the amount follows how much room there is to the nearest of 0 and 1), not from any source code.
// The 4x4 source pixels around the position are read once (16 reads; the soft look reads 4) into named variables (an array that is
// indexed while running was slower on the Intel chip), and the cubic and the four neighbour blends are all taken from them.
fn sharp_scene(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let p = uv * size - 0.5;
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let t00 = scene_texel(x + -1, y + -1);
    let t01 = scene_texel(x + 0, y + -1);
    let t02 = scene_texel(x + 1, y + -1);
    let t03 = scene_texel(x + 2, y + -1);
    let t10 = scene_texel(x + -1, y + 0);
    let t11 = scene_texel(x + 0, y + 0);
    let t12 = scene_texel(x + 1, y + 0);
    let t13 = scene_texel(x + 2, y + 0);
    let t20 = scene_texel(x + -1, y + 1);
    let t21 = scene_texel(x + 0, y + 1);
    let t22 = scene_texel(x + 1, y + 1);
    let t23 = scene_texel(x + 2, y + 1);
    let t30 = scene_texel(x + -1, y + 2);
    let t31 = scene_texel(x + 0, y + 2);
    let t32 = scene_texel(x + 1, y + 2);
    let t33 = scene_texel(x + 2, y + 2);
    // one screen pixel in source pixels (at most one source pixel, so the neighbours stay inside the 4x4 block)
    let d = clamp(size / vec2<f32>(f32(blit.out_width), f32(blit.out_height)), vec2<f32>(0.0), vec2<f32>(0.99));
    let cx = catmull_rom(f.x);
    let cy = catmull_rom(f.y);
    let lx = linear_weights(f.x);
    let ly = linear_weights(f.y);
    let wxm = linear_weights(f.x - d.x);
    let wxp = linear_weights(f.x + d.x);
    let wyn = linear_weights(f.y - d.y);
    let wys = linear_weights(f.y + d.y);
    let c = row4(t00, t01, t02, t03, cx) * cy.x + row4(t10, t11, t12, t13, cx) * cy.y + row4(t20, t21, t22, t23, cx) * cy.z + row4(t30, t31, t32, t33, cx) * cy.w;
    let n = row4(t00, t01, t02, t03, lx) * wyn.x + row4(t10, t11, t12, t13, lx) * wyn.y + row4(t20, t21, t22, t23, lx) * wyn.z + row4(t30, t31, t32, t33, lx) * wyn.w;
    let s = row4(t00, t01, t02, t03, lx) * wys.x + row4(t10, t11, t12, t13, lx) * wys.y + row4(t20, t21, t22, t23, lx) * wys.z + row4(t30, t31, t32, t33, lx) * wys.w;
    let w = row4(t00, t01, t02, t03, wxm) * ly.x + row4(t10, t11, t12, t13, wxm) * ly.y + row4(t20, t21, t22, t23, wxm) * ly.z + row4(t30, t31, t32, t33, wxm) * ly.w;
    let e = row4(t00, t01, t02, t03, wxp) * ly.x + row4(t10, t11, t12, t13, wxp) * ly.y + row4(t20, t21, t22, t23, wxp) * ly.z + row4(t30, t31, t32, t33, wxp) * ly.w;
    let mn = min(min(min(n, s), min(w, e)), c);
    let mx = max(max(max(n, s), max(w, e)), c);
    let amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3<f32>(0.0001)), vec3<f32>(0.0), vec3<f32>(1.0)));
    let k = -amp * SHARPNESS;
    return (c + (n + s + w + e) * k) / (1.0 + 4.0 * k);
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }

fn edge_aa(uv: vec2<f32>, center: vec3<f32>) -> vec3<f32> {
    // Fast approximate edge AA, evaluated in image space. Flat regions return
    // unchanged, while edge direction controls subpixel reconstruction.
    // The edge test reads the five source pixels around this one (one read each, instead of a smooth
    // blend per neighbour): most of the picture is flat and stops here, so this is what the picture costs.
    let texel_step = 1.0 / vec2<f32>(f32(blit.width), f32(blit.height));
    let c = vec2<i32>(floor(uv * vec2<f32>(f32(blit.width), f32(blit.height))));
    let nw = luma(scene_texel(c.x - 1, c.y - 1));
    let ne = luma(scene_texel(c.x + 1, c.y - 1));
    let sw = luma(scene_texel(c.x - 1, c.y + 1));
    let se = luma(scene_texel(c.x + 1, c.y + 1));
    let m = luma(scene_texel(c.x, c.y));
    let low = min(m, min(min(nw, ne), min(sw, se)));
    let high = max(m, max(max(nw, ne), max(sw, se)));
    if (high - low < max(0.0312, high * 0.125)) { return center; }
    var direction = vec2<f32>(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * 0.03125, 0.0078125);
    // The dotted noise the first version left along hard edges (health bar ends, text) came from here: an edge that is nearly
    // straight up/down or left/right got a direction of up to 8 source pixels, so the blend reached far along the edge and pulled in
    // unrelated dark and bright pixels as dots and ticks. A straight edge has no stairs to smooth, so it is left alone, and the
    // reach along a slanted one is at most EDGE_REACH source pixels.
    let ax = abs(direction.x);
    let ay = abs(direction.y);
    if (min(ax, ay) < max(max(ax, ay) * EDGE_SLANT, reduce)) { return center; }
    direction = clamp(direction / (min(ax, ay) + reduce), vec2<f32>(-EDGE_REACH), vec2<f32>(EDGE_REACH)) * texel_step;
    // The blends along the edge are taken from the plain smooth blend (4 reads each; the cubic would cost 16 each), and what they change
    // against the plain blend at this pixel is added to `center`. For the soft and crisp looks `center` is that blend itself, so the
    // result is the blend along the edge; for Sharp the cubic and the sharpening stay (text stays crisp) and the stairs are smoothed on top.
    let plain = scene(uv);
    let a = 0.5 * (scene(uv - direction / 6.0) + scene(uv + direction / 6.0));
    let b = a * 0.5 + 0.25 * (scene(uv - direction * 0.5) + scene(uv + direction * 0.5));
    let lb = luma(b);
    return center + (select(b, a, lb < low || lb > high) - plain);
}

// Hardware-filtered smooth tap at source-pixel position p (1 TMU instruction when use_hw_tex != 0).
fn smooth_at(p: vec2<f32>, inv_size: vec2<f32>) -> vec3<f32> {
    return scene_linear((p + 0.5) * inv_size);
}

// A picture bigger than the window (an upscaled one in a smaller window): every screen pixel is the average of
// the source area it covers using hardware bilinear taps (up to 4x fewer memory transactions than storage reads).
fn reduced_scene(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let inv_size = 1.0 / size;
    let footprint = size / vec2<f32>(f32(blit.out_width), f32(blit.out_height));
    let n = vec2<i32>(clamp(ceil(footprint * 0.5) + 1.0, vec2<f32>(2.0), vec2<f32>(4.0)));
    let centre = uv * size - 0.5;
    var sum = vec3<f32>(0.0);
    for (var j = 0; j < n.y; j = j + 1) {
        for (var i = 0; i < n.x; i = i + 1) {
            let offset = ((vec2<f32>(f32(i), f32(j)) + 0.5) / vec2<f32>(n) - 0.5) * footprint;
            sum = sum + smooth_at(centre + offset, inv_size);
        }
    }
    return sum / f32(n.x * n.y);
}

fn sample_depth_px(x: i32, y: i32) -> f32 {
    let cx = clamp(x, 0, i32(max(blit.depth_width, 1u)) - 1);
    let cy = clamp(y, 0, i32(max(blit.depth_height, 1u)) - 1);
    if (blit.use_depth_tex != 0u) {
        return textureLoad(depth_tex, vec2<i32>(cx, cy), 0).r;
    }
    return depth_buf[u32(cy) * max(blit.depth_width, 1u) + u32(cx)];
}

fn sample_depth(uv: vec2<f32>) -> f32 {
    let p = uv * vec2<f32>(f32(blit.depth_width), f32(blit.depth_height)) - 0.5;
    let o = floor(p);
    let f = p - o;
    let x = i32(o.x);
    let y = i32(o.y);
    let top = mix(sample_depth_px(x, y), sample_depth_px(x + 1, y), f.x);
    let bot = mix(sample_depth_px(x, y + 1), sample_depth_px(x + 1, y + 1), f.x);
    return mix(top, bot, f.y);
}

fn aces_filmic(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn post_grade(color: vec3<f32>, uv: vec2<f32>) -> vec3<f32> {
    var c = clamp(color, vec3<f32>(0.0), vec3<f32>(1.0));
    let has_depth = blit.depth_valid != 0u && blit.depth_width > 0u && blit.depth_height > 0u;

    // 1. Depth-Aware SSAO & SSGI Bounce (automatically skips sky/UI extremes z <= 0.0005 or z >= 0.9995)
    let vp_aspect = f32(max(blit.out_width, 1u)) / f32(max(blit.out_height, 1u));
    let inv_aspect = vec2<f32>(1.0 / max(vp_aspect, 0.5), 1.0);
    let src_size = vec2<f32>(f32(max(blit.width, 1u)), f32(max(blit.height, 1u)));
    if (has_depth && (blit.ssao > 0.001 || blit.ssgi > 0.001)) {
        let z0 = sample_depth(uv);
        if (z0 > 0.0005 && z0 < 0.9995) {
            // Tiny deterministic per-pixel spiral rotation to prevent banding
            let hash = fract(sin(dot(floor(uv * vec2<f32>(f32(blit.out_width), f32(blit.out_height))), vec2<f32>(12.9898, 78.233))) * 43758.5453);
            let rot = hash * 6.2831853;

            if (blit.ssao > 0.001) {
                var occ = 0.0;
                let radius = 0.024;
                for (var i = 0; i < 12; i = i + 1) {
                    let fi = f32(i) + 0.5;
                    let r = sqrt(fi / 12.0) * radius;
                    let ang = fi * 2.3999632 + rot;
                    let off = vec2<f32>(cos(ang), sin(ang)) * r * inv_aspect;
                    let zs = sample_depth(clamp(uv + off, vec2<f32>(0.001), vec2<f32>(0.999)));
                    if (zs > 0.0005 && zs < 0.9995) {
                        // In normalized depth (0 = near, 1 = far), a closer neighbor (zs < z0) occludes this pixel
                        let dz = z0 - zs;
                        if (dz > 0.0015 && dz < 0.085) {
                            let range_w = 1.0 - smoothstep(0.025, 0.085, dz);
                            occ = occ + smoothstep(0.0015, 0.015, dz) * range_w;
                        }
                    }
                }
                let ao = clamp(1.0 - (occ / 12.0) * blit.ssao * 1.65, 0.25, 1.0);
                c = c * ao;
            }

            if (blit.ssgi > 0.001) {
                var bounce = vec3<f32>(0.0);
                var wsum = 0.001;
                let gi_r = 0.065;
                for (var i = 0; i < 8; i = i + 1) {
                    let fi = f32(i) + 0.5;
                    let r = sqrt(fi / 8.0) * gi_r;
                    let ang = fi * 2.3999632 + rot * 0.7;
                    let suv = clamp(uv + vec2<f32>(cos(ang), sin(ang)) * r * inv_aspect, vec2<f32>(0.002), vec2<f32>(0.998));
                    let zs = sample_depth(suv);
                    if (zs > 0.0005 && zs < 0.9995) {
                        let depth_w = exp(-abs(zs - z0) * 38.0);
                        let sc = scene_linear(suv);
                        let lum = luma(sc);
                        let emit_w = smoothstep(0.32, 0.95, lum) * depth_w;
                        bounce = bounce + sc * emit_w;
                        wsum = wsum + depth_w;
                    }
                }
                let gi_light = (bounce / wsum) * blit.ssgi * 0.55;
                c = c + gi_light * (vec3<f32>(0.35) + c * 0.65);
            }
        }
    }

    // 2. Bloom / HDR Glow (dual-ring luminance-thresholded soft gather using hardware-filtered TMU taps)
    if (blit.bloom > 0.001) {
        var glow = vec3<f32>(0.0);
        for (var ring = 0; ring < 2; ring = ring + 1) {
            let rad = select(0.011, 0.026, ring == 1);
            let wt = select(0.65, 0.35, ring == 1);
            for (var k = 0; k < 8; k = k + 1) {
                let ang = (f32(k) + f32(ring) * 0.5) * 0.78539816;
                let suv = clamp(uv + vec2<f32>(cos(ang), sin(ang)) * rad * inv_aspect, vec2<f32>(0.002), vec2<f32>(0.998));
                let sc = scene_linear(suv);
                let lum = luma(sc);
                let bright = max(lum - 0.70, 0.0) / 0.30;
                glow = glow + sc * (bright * bright) * wt;
            }
        }
        c = c + (glow / 8.0) * blit.bloom * 0.85;
    }

    // 3. Optional ACES Filmic Tone Mapping
    if (blit.filmic != 0u) {
        c = aces_filmic(c * 1.12);
    }

    // 4. User Image Grading (Brightness, Contrast, Saturation, Gamma)
    c = clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
    c = (c - vec3<f32>(0.5)) * blit.contrast + vec3<f32>(0.5) + vec3<f32>(blit.brightness);
    c = clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
    let y = dot(c, vec3<f32>(0.299, 0.587, 0.114));
    c = clamp(mix(vec3<f32>(y), c, blit.saturation), vec3<f32>(0.0), vec3<f32>(1.0));
    if (abs(blit.gamma - 1.0) > 0.001) {
        c = pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / max(blit.gamma, 0.1)));
    }

    // 5. Optional Gothic Vignette & CRT Scanlines
    if (blit.vignette > 0.001) {
        let d = uv - vec2<f32>(0.5);
        let vig = 1.0 - blit.vignette * 0.72 * smoothstep(0.22, 0.78, dot(d, d) * 2.2);
        c = c * vig;
    }
    if (blit.scanlines > 0.001) {
        let lines = clamp(f32(blit.height), 224.0, 480.0);
        let scan_lines = select(lines, lines * 0.5, lines > 300.0);
        let line_phase = fract(uv.y * scan_lines);
        let mask = 1.0 - blit.scanlines * 0.45 * smoothstep(0.35, 0.85, abs(line_phase - 0.5) * 2.0);
        c = c * mask;
    }
    return clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn photo_transform_uv(uv: vec2<f32>) -> vec2<f32> {
    if (blit.photo_active == 0u) {
        return uv;
    }
    let aspect = max(f32(blit.out_width) / max(f32(blit.out_height), 1.0), 0.1);
    let zoom = max(blit.photo_zoom, 0.25);
    var d = (uv - vec2<f32>(0.5)) / zoom;
    if (abs(blit.photo_roll_rad) > 0.0001) {
        let c = cos(blit.photo_roll_rad);
        let s = sin(blit.photo_roll_rad);
        let dx = d.x * aspect;
        let dy = d.y;
        d = vec2<f32>((dx * c - dy * s) / aspect, dx * s + dy * c);
    }
    return d + vec2<f32>(0.5 - blit.photo_pan_x, 0.5 - blit.photo_pan_y);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    let uv = photo_transform_uv(in.uv);
    if (blit.photo_active != 0u && (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    var c: vec3<f32>;
    if (blit.width > blit.out_width + blit.out_width / 16u || blit.height > blit.out_height + blit.out_height / 16u) {
        return vec4<f32>(post_grade(reduced_scene(uv), uv), 1.0);
    }
    if ((blit.options & 4u) != 0u) {
        c = sharp_scene(uv);
    } else {
        c = scene(uv);
    }
    if ((blit.options & 1u) != 0u) { c = edge_aa(uv, c); }
    return vec4<f32>(post_grade(c, uv), 1.0);
}

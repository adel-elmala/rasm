// Labs of docs/guides/image-based-lighting.html.

// The math of the page: the equirectangular mapping, the cube face table, GGX and its importance sampling, the
// split-sum BRDF table. The same formulas as the page's text.
const Ibl = (function () {
    // Cube faces in the usual order (+X, -X, +Y, -Y, +Z, -Z): the axis a face looks along, and the directions in which
    // its texel coordinates s (right) and t (down) grow.
    const FACES = ["+X", "−X", "+Y", "−Y", "+Z", "−Z"];
    const FORWARD = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
    const S = [[0, 0, -1], [0, 0, 1], [1, 0, 0], [1, 0, 0], [1, 0, 0], [-1, 0, 0]];
    const T = [[0, -1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1], [0, -1, 0], [0, -1, 0]];

    function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // Azimuth from -Z toward +X, elevation up from the horizon, both in degrees.
    function direction(azimuth, elevation) {
        const a = azimuth * Math.PI / 180;
        const e = elevation * Math.PI / 180;
        return [Math.cos(e) * Math.sin(a), Math.sin(e), -Math.cos(e) * Math.cos(a)];
    }
    // -Z at the image's centre, +X a quarter to its right, +Y at the top row.
    function equirect(d) {
        return [0.5 + Math.atan2(d[0], -d[2]) / (2 * Math.PI), Math.acos(Math.max(-1, Math.min(1, d[1]))) / Math.PI];
    }
    function cubeFace(d) {
        const a = d.map(Math.abs);
        let face;
        if (a[0] >= a[1] && a[0] >= a[2]) face = d[0] >= 0 ? 0 : 1;
        else if (a[1] >= a[2]) face = d[1] >= 0 ? 2 : 3;
        else face = d[2] >= 0 ? 4 : 5;
        const depth = dot(d, FORWARD[face]);
        return { face: face, s: (dot(d, S[face]) / depth + 1) / 2, t: (dot(d, T[face]) / depth + 1) / 2 };
    }

    function ggx(NoH, alpha) {
        const a = NoH * alpha;
        const k = alpha / (1 - NoH * NoH + a * a);
        return k * k / Math.PI;
    }
    function visibility(NoV, NoL, alpha) {
        const a2 = alpha * alpha;
        const l = NoV * Math.sqrt((-NoL * a2 + NoL) * NoL + a2);
        const v = NoL * Math.sqrt((-NoV * a2 + NoV) * NoV + a2);
        return 0.5 / Math.max(v + l, 1e-8);
    }
    function radicalInverse(i) {
        let bits = i >>> 0;
        bits = ((bits << 16) | (bits >>> 16)) >>> 0;
        bits = (((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >>> 1)) >>> 0;
        bits = (((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >>> 2)) >>> 0;
        bits = (((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >>> 4)) >>> 0;
        bits = (((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >>> 8)) >>> 0;
        return bits * 2.3283064365386963e-10;
    }
    function hammersley(i, n) { return [(i + 0.5) / n, radicalInverse(i)]; }
    // A half vector with the GGX distribution around +Z: its angle from the normal, and its turn around it.
    function sampleGgx(xi, alpha) {
        const cosTheta = Math.sqrt((1 - xi[1]) / (1 + (alpha * alpha - 1) * xi[1]));
        return { theta: Math.acos(Math.min(1, cosTheta)), phi: 2 * Math.PI * xi[0] };
    }
    // The mip of a cube of size² texels per face that one sample of pdf stands for, one mip coarser.
    function sampleMip(pdf, count, size, mips) {
        const texel = 4 * Math.PI / (6 * size * size);
        const sample = 1 / (count * pdf + 1e-6);
        return Math.max(0, Math.min(mips - 1, 0.5 * Math.log2(sample / texel) + 1));
    }
    // The split-sum table at (n.v, roughness): f0's scale and bias.
    function brdf(NoV, roughness, count) {
        const alpha = roughness * roughness;
        const v = [Math.sqrt(1 - NoV * NoV), 0, NoV];
        let a = 0;
        let b = 0;
        for (let i = 0; i < count; i++) {
            const s = sampleGgx(hammersley(i, count), alpha);
            const h = [Math.sin(s.theta) * Math.cos(s.phi), Math.sin(s.theta) * Math.sin(s.phi), Math.cos(s.theta)];
            const VoH = Math.max(0, dot(v, h));
            const NoL = Math.max(0, 2 * VoH * h[2] - v[2]);
            if (NoL <= 0) continue;
            const g = visibility(NoV, NoL, alpha) * 4 * NoL * VoH / Math.max(h[2], 1e-6);
            const fc = Math.pow(1 - VoH, 5);
            a += (1 - fc) * g;
            b += fc * g;
        }
        return [a / count, b / count];
    }
    return { FACES: FACES, direction: direction, equirect: equirect, cubeFace: cubeFace, ggx: ggx, hammersley: hammersley, sampleGgx: sampleGgx, sampleMip: sampleMip, brdf: brdf };
})();

// A direction, where it lands in the 2:1 image and on which face of the cube.
RasmLab.register("env-lookup", function (lab) {
    const svg = lab.svg(760, 300, "Left, an equirectangular image with a dot where the chosen direction lands. Right, the six faces of a cube map, with the face the direction falls on marked and the dot on it.");
    const azimuth = lab.slider({ label: "Turn (azimuth)", min: -180, max: 180, step: 5, value: 35, format: function (v) { return `${v}°`; } });
    const elevation = lab.slider({ label: "Up (elevation)", min: -85, max: 85, step: 5, value: 5, format: function (v) { return `${v}°`; } });
    const uvOut = lab.readout("place in the image (u, v)");
    const faceOut = lab.readout("cube face, place on it (s, t)");
    const rowOut = lab.readout("width of one image texel, at this row");
    const note = lab.note();

    const IX = 20;
    const IY = 40;
    const IW = 400;
    const IH = 200;
    lab.el(svg, "text", { x: IX, y: 24, class: "title", text: "the image: 360° across, 180° down" });
    lab.el(svg, "rect", { x: IX, y: IY, width: IW, height: IH, class: "paper" });
    lab.el(svg, "path", { d: `M${IX + IW / 4},${IY}V${IY + IH}M${IX + IW / 2},${IY}V${IY + IH}M${IX + 3 * IW / 4},${IY}V${IY + IH}M${IX},${IY + IH / 2}H${IX + IW}M${IX},${IY + IH / 4}H${IX + IW}M${IX},${IY + 3 * IH / 4}H${IX + IW}`, class: "grid" });
    [["+Z", 0], ["−X", 0.25], ["−Z", 0.5], ["+X", 0.75], ["+Z", 1]].forEach(function (p) {
        lab.el(svg, "text", { x: IX + p[1] * IW, y: IY + IH + 16, class: "mid small", text: p[0] });
    });
    lab.el(svg, "text", { x: IX - 4, y: IY + 10, class: "end small", text: "up" });
    lab.el(svg, "text", { x: IX + IW / 2, y: IY + IH + 34, class: "mid small t-faint", text: "u →   (v: down from the top row)" });
    const imageDot = lab.el(svg, "circle", { r: 7, class: "accent" });

    const CX = 470;
    const CY = 40;
    const CS = 80;
    const faces = [];
    lab.el(svg, "text", { x: CX, y: 24, class: "title", text: "the cube: six square faces" });
    for (let f = 0; f < 6; f++) {
        const x = CX + (f % 3) * (CS + 10);
        const y = CY + Math.floor(f / 3) * (CS + 36);
        const rect = lab.el(svg, "rect", { x: x, y: y, width: CS, height: CS, class: "paper" });
        lab.el(svg, "text", { x: x + CS / 2, y: y + CS + 16, class: "mid small mono", text: Ibl.FACES[f] });
        faces.push({ x: x, y: y, rect: rect });
    }
    const faceDot = lab.el(svg, "circle", { r: 6, class: "accent" });
    lab.el(svg, "text", { x: CX, y: 280, class: "small t-faint", text: "each face: s → right, t → down" });

    lab.draw(function () {
        const d = Ibl.direction(azimuth.value, elevation.value);
        const uv = Ibl.equirect(d);
        lab.set(imageDot, { cx: IX + uv[0] * IW, cy: IY + uv[1] * IH });
        const c = Ibl.cubeFace(d);
        faces.forEach(function (f, i) { lab.set(f.rect, { class: i === c.face ? "mem" : "paper" }); });
        lab.set(faceDot, { cx: faces[c.face].x + c.s * CS, cy: faces[c.face].y + c.t * CS });
        uvOut.set(`${uv[0].toFixed(3)}, ${uv[1].toFixed(3)}`);
        faceOut.set(`${Ibl.FACES[c.face]}: ${c.s.toFixed(2)}, ${c.t.toFixed(2)}`, "mem");
        // A texel of a W-wide image covers 360 / W degrees of longitude, which at this latitude is that times cos.
        const width = Math.cos(elevation.value * Math.PI / 180);
        rowOut.set(`${(width * 100).toFixed(0)}% of one at the horizon`, width < 0.5 ? "bad" : "ok");
        if (c.face === 2 || c.face === 3)
            note.set("The top and bottom faces get the image's first and last rows, where one texel of the image covers a sliver of the sphere. Many image texels fall on one cube texel there: average them, or the cube aliases.", "bad");
        else
            note.set("The four side faces each hold a quarter of the image's width, so a cube face as wide as a quarter of the image keeps its detail.", "ok");
    });
});

// Roughness: how wide the reflected light is, which mip holds it, and which mip each sample reads.
RasmLab.register("ggx-lobe", function (lab) {
    const svg = lab.svg(760, 300, "A surface seen from the side, with the reflected direction straight up. The GGX lobe around it gets wider with roughness. Dots on the arc are importance samples, at their angle from the mirror direction. On the right, the six mips of a prefiltered cube, with the one this roughness reads marked.");
    const rough = lab.slider({ label: "Roughness", min: 0, max: 1, step: 0.05, value: 0.4, format: function (v) { return v.toFixed(2); } });
    const count = lab.choice({ label: "Samples", value: 64, options: [{ value: 16, label: "16" }, { value: 64, label: "64" }, { value: 256, label: "256" }] });
    const mipOut = lab.readout("mip of the prefiltered cube");
    const widthOut = lab.readout("lobe width (half of its peak)");
    const sampleOut = lab.readout("source mip a sample reads (512² cube)");
    const note = lab.note();

    const OX = 230;
    const OY = 260;
    const R = 200;
    lab.el(svg, "line", { x1: OX - R - 10, y1: OY, x2: OX + R + 10, y2: OY, class: "surface" });
    lab.el(svg, "line", { x1: OX, y1: OY, x2: OX, y2: OY - R - 10, class: "arrow dashed" });
    lab.el(svg, "text", { x: OX + 6, y: OY - R - 14, class: "small", text: "mirror direction" });
    lab.el(svg, "text", { x: OX + R + 14, y: OY - 6, class: "small t-faint", text: "dots: samples" });
    const lobe = lab.el(svg, "path", { class: "light" });
    const dots = lab.el(svg, "g", {});

    const MX = 500;
    const mips = [];
    lab.el(svg, "text", { x: MX, y: 24, class: "title", text: "prefiltered cube, one roughness per mip" });
    for (let m = 0; m < 6; m++) {
        const size = 64 >> m;
        const x = MX + [0, 72, 112, 136, 152, 164][m];
        const rect = lab.el(svg, "rect", { x: x, y: 40, width: Math.max(size, 6), height: Math.max(size, 6), class: "paper" });
        lab.el(svg, "text", { x: x + Math.max(size, 6) / 2, y: 124, class: "mid small mono", text: `${m}` });
        mips.push(rect);
    }
    lab.el(svg, "text", { x: MX, y: 144, class: "small t-faint", text: "mip m holds roughness m / 5" });

    lab.draw(function () {
        const roughness = rough.value;
        const alpha = Math.max(roughness * roughness, 1e-3);
        const n = count.value;
        // The lobe of reflected directions, with n = v: the half vector is halfway between n and l.
        const peak = Ibl.ggx(1, alpha);
        let d = "";
        let halfWidth = 0;
        for (let deg = -90; deg <= 90; deg += 1) {
            const theta = deg * Math.PI / 180;
            const value = Ibl.ggx(Math.cos(theta / 2), alpha) / peak;
            if (value >= 0.5) halfWidth = Math.max(halfWidth, Math.abs(deg));
            const r = R * value;
            d += `${d ? "L" : "M"}${(OX + r * Math.sin(theta)).toFixed(1)},${(OY - r * Math.cos(theta)).toFixed(1)}`;
        }
        lab.set(lobe, { d: d + "Z" });

        lab.clear(dots);
        let pdfs = [];
        for (let i = 0; i < n; i++) {
            const s = Ibl.sampleGgx(Ibl.hammersley(i, n), alpha);
            const thetaL = 2 * s.theta;
            if (thetaL >= Math.PI / 2) continue;
            // Each sample on an arc, at its angle from the mirror direction, on the side its turn around it points to.
            const angle = Math.cos(s.phi) >= 0 ? thetaL : -thetaL;
            lab.el(dots, "circle", { cx: OX + 1.04 * R * Math.sin(angle), cy: OY - 1.04 * R * Math.cos(angle), r: 3, class: "accent" });
            pdfs.push(Ibl.ggx(Math.cos(s.theta), alpha) / 4);
        }
        pdfs.sort(function (a, b) { return a - b; });
        const median = pdfs[Math.floor(pdfs.length / 2)];

        const lod = roughness * 5;
        mips.forEach(function (rect, m) {
            lab.set(rect, { class: Math.abs(m - lod) < 1 ? (m === Math.round(lod) ? "hot" : "mem") : "paper" });
        });
        mipOut.set(lod.toFixed(2), "mem");
        widthOut.set(roughness === 0 ? "0° (a mirror)" : `±${halfWidth}°`);
        if (roughness === 0) {
            sampleOut.set("copy, no samples", "ok");
            note.set("Roughness 0 is a mirror: its mip is a copy of the environment at the same size, with no sampling at all.", "ok");
            return;
        }
        const mip = Ibl.sampleMip(median, n, 512, 10);
        sampleOut.set(mip.toFixed(1), "gpu");
        if (mip < 0.5)
            note.set("Each sample reads the sharpest mip: with few samples and a narrow lobe, a small bright light is hit by one sample or by none, and the result sparkles (fireflies).", "bad");
        else
            note.set(`Each of the ${n} samples stands for about 1/${n} of the lobe, so it reads a mip whose texels are that big: the bright light is spread out, not hit or missed.`, "ok");
    });
});

// The split-sum table: what the BRDF does to f0, by viewing angle and roughness.
RasmLab.register("brdf-lut", function (lab) {
    const svg = lab.svg(760, 344, "Two square tables, 32 by 32 cells, computed here. Across: n dot v from 0 to 1. Down: roughness from 0 to 1. Left table: the scale of f0. Right table: the bias. A handle picks a cell.");
    const scaleOut = lab.readout("scale (x)");
    const biasOut = lab.readout("bias (y)");
    const plasticOut = lab.readout("reflected, f0 = 0.04 (plastic)");
    const note = lab.note();

    const N = 32;
    const CELL = 8;
    const LX = 60;
    const RX = 420;
    const TY = 40;
    const table = [];
    for (let j = 0; j < N; j++) {
        for (let i = 0; i < N; i++)
            table.push(Ibl.brdf(Math.max((i + 0.5) / N, 1e-4), (j + 0.5) / N, 64));
    }
    function cells(x0, k) {
        for (let j = 0; j < N; j++) {
            for (let i = 0; i < N; i++) {
                const v = Math.round(255 * Math.pow(Math.min(1, table[j * N + i][k]), 1 / 2.2));
                lab.el(svg, "rect", { x: x0 + i * CELL, y: TY + j * CELL, width: CELL, height: CELL, fill: `rgb(${v},${v},${v})` });
            }
        }
        lab.el(svg, "text", { x: x0 + N * CELL / 2, y: TY + N * CELL + 18, class: "mid small", text: "n · v →  (0: grazing, 1: facing)" });
        lab.el(svg, "text", { x: x0 - 8, y: TY + 10, class: "end small", text: "0" });
        lab.el(svg, "text", { x: x0 - 8, y: TY + N * CELL, class: "end small", text: "1" });
        lab.el(svg, "text", { x: x0 - 8, y: TY + N * CELL / 2, class: "end small", text: "rough" });
    }
    lab.el(svg, "text", { x: LX, y: 26, class: "title", text: "scale of f0" });
    lab.el(svg, "text", { x: RX, y: 26, class: "title", text: "bias, added" });
    cells(LX, 0);
    cells(RX, 1);
    const mark = lab.el(svg, "rect", { width: CELL, height: CELL, class: "hot" });
    lab.el(svg, "text", { x: 380, y: 338, class: "mid small t-faint", text: "drag the handle on the left table" });
    const handle = lab.handle(svg, { x: LX + 0.7 * N * CELL, y: TY + 0.3 * N * CELL, label: "Pick n·v and roughness", minX: LX, maxX: LX + N * CELL - 1, minY: TY, maxY: TY + N * CELL - 1 });

    lab.draw(function () {
        const i = Math.min(N - 1, Math.floor((handle.x - LX) / CELL));
        const j = Math.min(N - 1, Math.floor((handle.y - TY) / CELL));
        lab.set(mark, { x: RX + i * CELL, y: TY + j * CELL });
        const v = table[j * N + i];
        scaleOut.set(v[0].toFixed(3));
        biasOut.set(v[1].toFixed(3));
        plasticOut.set((0.04 * v[0] + v[1]).toFixed(3), "gpu");
        const NoV = (i + 0.5) / N;
        if (NoV < 0.2 && j < N / 2)
            note.set("At a grazing angle on a smooth surface the bias is large: Fresnel makes even plastic reflect most of the light, like a lake seen from far away.", "ok");
        else if (j > 3 * N / 4)
            note.set("Rough surfaces reflect less in total (scale + bias below 1): light that bounces between the tiny facets is lost in this model.", "bad");
        else
            note.set("Specular light = prefiltered environment × (f0 × scale + bias). One texture fetch replaces the whole BRDF integral.", "ok");
    });
});

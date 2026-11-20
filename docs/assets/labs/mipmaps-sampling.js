// Labs of docs/guides/mipmaps-sampling.html.

// A checker floor seen in perspective, sampled per pixel like a GPU does: uv per pixel, derivatives from the
// neighbor pixels, λ = log2(texels per pixel), then the chosen filter on a real mip chain (2×2 box averages).
RasmLab.register("floor-filter", function (lab) {
    const W = 380;
    const H = 220;
    const ctx = lab.canvas(W, H, "A checker floor that runs to the horizon, drawn with the chosen texture filter.");
    const svg = lab.svg(360, 220, "A plot of the mip level against the screen row, from near to far: the level that matches (λ) and the level the filter reads.");
    const FILTERS = {
        point: { label: "point", mips: "none", linear: false, reads: 1 },
        bilinearNoMips: { label: "bilinear, no mips", mips: "none", linear: true, reads: 4 },
        pointMips: { label: "point + mips", mips: "nearest", linear: false, reads: 1 },
        bilinear: { label: "bilinear + mips", mips: "nearest", linear: true, reads: 4 },
        trilinear: { label: "trilinear", mips: "linear", linear: true, reads: 8 },
    };
    const filter = lab.choice({
        label: "Filter",
        options: Object.keys(FILTERS).map(function (key) { return { value: key, label: FILTERS[key].label }; }),
        value: "point",
    });
    const tint = lab.toggle({ label: "Color each level", value: false });
    const readsOut = lab.readout("texel reads per pixel");
    const rhoOut = lab.readout("texels under one pixel at the far end (ρ)");
    const lambdaOut = lab.readout("the level that matches there (λ)");
    const note = lab.note();

    // ---- the texture: a 256×256 checker in linear light, and its mip chain
    const SIZE = 256;
    const CHECK = 8;
    const levels = [];
    let level0 = new Float32Array(SIZE * SIZE);
    for (let y = 0; y < SIZE; y++) {
        for (let x = 0; x < SIZE; x++) level0[y * SIZE + x] = ((x / CHECK | 0) + (y / CHECK | 0)) % 2 ? 0.8 : 0.03;
    }
    levels.push({ size: SIZE, data: level0 });
    while (levels[levels.length - 1].size > 1) {
        const up = levels[levels.length - 1];
        const size = up.size / 2;
        const data = new Float32Array(size * size);
        for (let y = 0; y < size; y++) {
            for (let x = 0; x < size; x++) {
                data[y * size + x] = (up.data[2 * y * up.size + 2 * x] + up.data[2 * y * up.size + 2 * x + 1] +
                                      up.data[(2 * y + 1) * up.size + 2 * x] + up.data[(2 * y + 1) * up.size + 2 * x + 1]) / 4;
            }
        }
        levels.push({ size: size, data: data });
    }
    const MAX_LEVEL = levels.length - 1;

    function texel(level, x, y) {
        const m = level.size - 1;      // sizes are powers of two, so "and" is the repeat wrap mode
        return level.data[(y & m) * level.size + (x & m)];
    }
    function sampleLevel(index, u, v, linear) {
        const level = levels[index];
        if (!linear) return texel(level, Math.floor(u * level.size), Math.floor(v * level.size));
        const x = u * level.size - 0.5;
        const y = v * level.size - 0.5;
        const x0 = Math.floor(x);
        const y0 = Math.floor(y);
        const fx = x - x0;
        const fy = y - y0;
        return (texel(level, x0, y0) * (1 - fx) + texel(level, x0 + 1, y0) * fx) * (1 - fy) +
               (texel(level, x0, y0 + 1) * (1 - fx) + texel(level, x0 + 1, y0 + 1) * fx) * fy;
    }
    // the level or the two levels a filter reads for one λ: [level a, level b, weight of b]
    function pick(lambda, mips) {
        if (mips === "none" || lambda <= 0) return [0, 0, 0];
        if (mips === "nearest") {
            const n = Math.min(MAX_LEVEL, Math.floor(lambda + 0.5));
            return [n, n, 0];
        }
        const a = Math.min(MAX_LEVEL, Math.floor(lambda));
        return [a, Math.min(MAX_LEVEL, a + 1), lambda - Math.floor(lambda)];
    }

    // ---- the camera: 1 unit above the floor, looking at the horizon, which is 30 pixels above the picture
    const FOCAL = 300;
    const HORIZON = -30;
    const REPEAT = 0.25;        // the texture repeats every 4 units
    let travel = 0;
    function uvAt(px, py) {
        const z = FOCAL / (py + 0.5 - HORIZON);
        return [(px + 0.5 - W / 2) * z / FOCAL * REPEAT, (z + travel) * REPEAT];
    }
    function lambdaAt(px, py) {
        const here = uvAt(px, py);
        const right = uvAt(px + 1, py);
        const down = uvAt(px, py + 1);
        const rhoX = Math.hypot((right[0] - here[0]) * SIZE, (right[1] - here[1]) * SIZE);
        const rhoY = Math.hypot((down[0] - here[0]) * SIZE, (down[1] - here[1]) * SIZE);
        return { rhoX: rhoX, rhoY: rhoY, rho: Math.max(rhoX, rhoY), lambda: Math.log2(Math.max(rhoX, rhoY)), uv: here };
    }

    lab.animate(function (dt) { travel += dt * 0.35; });

    // ---- the plot
    const PX = { x0: 44, x1: 346, y0: 184, y1: 22 };
    const SHOWN = 5;
    function plotX(row) { return PX.x0 + (H - 1 - row) / (H - 1) * (PX.x1 - PX.x0); }
    function plotY(level) { return PX.y0 - lab.clamp(level, 0, SHOWN) / SHOWN * (PX.y0 - PX.y1); }
    let gridPath = "";
    for (let n = 1; n <= SHOWN; n++) gridPath += `M${PX.x0},${plotY(n)}H${PX.x1}`;
    lab.el(svg, "path", { d: gridPath, class: "grid" });
    lab.el(svg, "path", { d: `M${PX.x0},${PX.y1 - 8}V${PX.y0}H${PX.x1 + 6}`, class: "arrow" });
    const dots = [];
    for (let n = 0; n <= SHOWN; n++) {
        lab.el(svg, "text", { x: PX.x0 - 20, y: plotY(n) + 4, class: "end small", text: String(n) });
        dots.push(lab.el(svg, "circle", { cx: PX.x0 - 10, cy: plotY(n), r: 5, class: "box" }));
    }
    lab.el(svg, "text", { x: PX.x0 + 8, y: 14, class: "small", text: "mip level" });
    lab.el(svg, "text", { x: PX.x0, y: 202, class: "small", text: "near" });
    lab.el(svg, "text", { x: PX.x1, y: 202, class: "end small", text: "far" });
    lab.el(svg, "text", { x: (PX.x0 + PX.x1) / 2, y: 202, class: "mid small", text: "screen row, bottom to top" });
    const wanted = lab.el(svg, "path", { class: "arrow dashed" });
    const used = lab.el(svg, "path", { class: "arrow hot thick" });
    lab.el(svg, "text", { x: PX.x0 + 14, y: 44, class: "small t-hot", text: "thick: the level the filter reads" });
    lab.el(svg, "text", { x: PX.x0 + 14, y: 60, class: "small", text: "dashed: λ, the level that matches" });

    function encode(l) {
        return 255 * (l <= 0.0031308 ? 12.92 * l : 1.055 * Math.pow(l, 1 / 2.4) - 0.055);
    }
    function rgb(name) {
        ctx.fillStyle = lab.css(name);      // the canvas turns any CSS color into #rrggbb
        const hex = ctx.fillStyle;
        return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)];
    }
    const LEVEL_COLORS = ["--cpu", "--gpu", "--mem", "--shader", "--accent", "--bad"];
    const image = ctx.createImageData(W, H);

    lab.draw(function () {
        const f = FILTERS[filter.value];
        const colors = LEVEL_COLORS.map(rgb);
        const out = image.data;
        for (let py = 0; py < H; py++) {
            for (let px = 0; px < W; px++) {
                const s = lambdaAt(px, py);
                const p = pick(s.lambda, f.mips);
                let value = sampleLevel(p[0], s.uv[0], s.uv[1], f.linear);
                if (p[2] > 0) value = value * (1 - p[2]) + sampleLevel(p[1], s.uv[0], s.uv[1], f.linear) * p[2];
                const gray = encode(value);
                const k = (py * W + px) * 4;
                if (tint.value) {
                    const a = colors[Math.min(p[0], SHOWN)];
                    const b = colors[Math.min(p[1], SHOWN)];
                    for (let c = 0; c < 3; c++) out[k + c] = 0.45 * gray + 0.55 * (a[c] * (1 - p[2]) + b[c] * p[2]);
                } else {
                    out[k] = out[k + 1] = out[k + 2] = gray;
                }
                out[k + 3] = 255;
            }
        }
        ctx.putImageData(image, 0, 0);

        // the plot: the middle column of the screen, row by row
        let wantedPath = "";
        let usedPath = "";
        for (let py = H - 1; py >= 0; py--) {
            const lambda = lambdaAt(W / 2, py).lambda;
            const p = pick(lambda, f.mips);
            wantedPath += `${wantedPath ? "L" : "M"}${plotX(py).toFixed(1)},${plotY(Math.max(lambda, 0)).toFixed(1)}`;
            usedPath += `${usedPath ? "L" : "M"}${plotX(py).toFixed(1)},${plotY(p[0] + (p[1] - p[0]) * p[2]).toFixed(1)}`;
        }
        lab.set(wanted, { d: wantedPath });
        lab.set(used, { d: usedPath });
        dots.forEach(function (dot, n) {
            if (tint.value) lab.set(dot, { class: null, fill: `rgb(${colors[n].join(",")})` });
            else lab.set(dot, { class: "box", fill: null });
        });

        const far = lambdaAt(W / 2, 0);
        readsOut.set(String(f.reads), f.reads === 8 ? "ok" : "");
        rhoOut.set(far.rho.toFixed(1));
        lambdaOut.set(far.lambda.toFixed(2));
        const covered = `${far.rhoX.toFixed(0)} × ${far.rhoY.toFixed(0)}`;
        if (f.mips === "none") note.set(`Aliasing: a far pixel covers about ${covered} texels of level 0 but reads only ${f.reads}. The far pattern is false (moiré), and it flickers when the camera moves.`, "bad");
        else if (!f.linear) note.set("No flicker: far pixels read a small level, where one texel is already the average. But each pixel returns one texel, so the floor is blocky, and the level jumps.", "");
        else if (f.mips === "nearest") note.set("Smooth and steady. But the level is a whole number, so a line shows on the floor where it jumps. Color the levels to see the lines.", "");
        else note.set("Trilinear: two levels around λ, blended by its fraction. No flicker, no blocks, no lines.", "ok");
    });
});

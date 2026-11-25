// Labs of docs/guides/reference-grid.html.

// A grid of 1 cm cells in levels of s, computed per pixel as the page's shader does: the plane coordinates in cells,
// their change over a pixel (the derivative), the level of detail log_s(d * F), three levels side by side, and the
// pristine-grid line coverage. A top view keeps one level everywhere; a tilted perspective view changes level with
// distance.
RasmLab.register("grid-levels", function (lab) {
    const W = 760;
    const H = 300;
    const CELL = 0.01;          // metres: the finest level
    const FADE = 32;            // pixels: a level's cells are fully shown from this width up
    const MINOR = 1.0;          // line widths in pixels
    const MAJOR = 1.5;
    const ctx = lab.canvas(W, H, "A reference grid computed per pixel: the levels a pixel shows, faded by the level of detail, in a top view or a tilted perspective view.");
    const view = lab.choice({ label: "View", options: [{ value: "top", label: "top, orthographic" }, { value: "tilted", label: "tilted, perspective" }], value: "top" });
    const zoom = lab.slider({ label: "Zoom out (log10 of metres per pixel, or of the camera's height)", min: -4, max: 1.5, step: 0.01, value: -2.6, format: function (v) { return v.toFixed(2); } });
    const subdivisions = lab.choice({ label: "Subdivisions s", options: ["2", "4", "5", "10"], value: "10" });
    const levelOut = lab.readout("level A's cells at the centre");
    const widthOut = lab.readout("their width on screen");
    const blendOut = lab.readout("blend: A fades by 1 − blend");
    const note = lab.note();
    const image = ctx.createImageData(W, H);

    function rgb(name) {
        ctx.fillStyle = lab.css(name);      // the canvas turns any CSS color into #rrggbb
        const hex = ctx.fillStyle;
        return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)];
    }
    function smoothstep(e0, e1, x) {
        const t = Math.min(Math.max((x - e0) / (e1 - e0), 0), 1);
        return t * t * (3 - 2 * t);
    }
    function frac(x) {
        return x - Math.floor(x);
    }
    // One axis of the shader's getLineCoverage: lines at whole uv, widthPixels wide.
    function axisCoverage(uv, derivative, widthPixels) {
        const target = Math.min(widthPixels * derivative, 1);
        const invert = target > 0.5;
        const width = invert ? 1 - target : target;
        const drawWidth = Math.min(Math.max(width, derivative), 0.5);
        const smoothing = Math.max(derivative, 1e-6) * 1.5;
        let distance = Math.abs(frac(uv) * 2 - 1);
        distance = invert ? distance : 1 - distance;
        let coverage = smoothstep(drawWidth + smoothing, drawWidth - smoothing, distance);
        coverage *= Math.min(Math.max(width / drawWidth, 0), 1);
        coverage = coverage + (width - coverage) * Math.min(Math.max(derivative * 2 - 1, 0), 1);
        return invert ? 1 - coverage : coverage;
    }
    function lineCoverage(u, v, du, dv, widthPixels) {
        const x = axisCoverage(u, du, widthPixels);
        const y = axisCoverage(v, dv, widthPixels);
        return x + (1 - x) * y;
    }

    // The plane point (in cells) a pixel shows, or null above the horizon, and how steeply its ray meets the plane.
    function hit(px, py, tilted, scale) {
        if (!tilted)
            return { u: 37.3 + (px - W / 2) * scale / CELL, v: 11.8 + (py - H / 2) * scale / CELL, facing: 1 };
        const tanHalf = Math.tan(25 * Math.PI / 180);
        const nx = (2 * (px + 0.5) / W - 1) * tanHalf * W / H;
        const ny = (1 - 2 * (py + 0.5) / H) * tanHalf;
        const pitch = -28 * Math.PI / 180;
        // The view ray (nx, ny, -1) turned down by the pitch around the camera's right axis.
        const dy = ny * Math.cos(pitch) + Math.sin(pitch);
        const dz = ny * Math.sin(pitch) - Math.cos(pitch);
        if (dy >= -1e-6)
            return null;
        const t = scale / -dy;
        return { u: 37.3 + nx * t / CELL, v: 11.8 + dz * t / CELL, facing: -dy / Math.hypot(nx, dy, dz) };
    }

    lab.draw(function () {
        const tilted = view.value === "tilted";
        const scale = Math.pow(10, zoom.value);
        const s = Number(subdivisions.value);
        const back = rgb("--bg-soft");
        const ink = rgb("--fg");
        const out = image.data;
        let centre = null;
        for (let py = 0; py < H; py++) {
            for (let px = 0; px < W; px++) {
                const p = hit(px, py, tilted, scale);
                const right = p && hit(px + 1, py, tilted, scale);
                const below = p && hit(px, py + 1, tilted, scale);
                let alpha = 0;
                if (p && right && below) {
                    const du = Math.hypot(right.u - p.u, below.u - p.u);
                    const dv = Math.hypot(right.v - p.v, below.v - p.v);
                    const lod = Math.max(Math.log(Math.max(du, dv) * FADE) / Math.log(s), 0);
                    const level = Math.floor(lod);
                    const blend = lod - level;
                    const sizeA = Math.pow(s, level);
                    const a = lineCoverage(p.u / sizeA, p.v / sizeA, du / sizeA, dv / sizeA, MINOR) * (1 - blend) * 0.35;
                    const b = lineCoverage(p.u / (sizeA * s), p.v / (sizeA * s), du / (sizeA * s), dv / (sizeA * s), MAJOR + (MINOR - MAJOR) * blend) * (0.7 + (0.35 - 0.7) * blend);
                    const c = lineCoverage(p.u / (sizeA * s * s), p.v / (sizeA * s * s), du / (sizeA * s * s), dv / (sizeA * s * s), MAJOR) * blend * 0.7;
                    alpha = a;
                    alpha = b + alpha * (1 - b);
                    alpha = c + alpha * (1 - c);
                    alpha *= smoothstep(0.01, 0.06, p.facing);
                    if (px === W / 2 && py === H / 2)
                        centre = { level: level, blend: blend, cells: CELL * sizeA, pixels: sizeA / Math.max(du, dv) };
                }
                const i = 4 * (py * W + px);
                for (let k = 0; k < 3; k++) out[i + k] = back[k] + (ink[k] - back[k]) * alpha;
                out[i + 3] = 255;
            }
        }
        ctx.putImageData(image, 0, 0);

        if (centre) {
            const metres = centre.cells;
            levelOut.set(metres >= 1 ? `${metres.toFixed(metres < 10 ? 1 : 0)} m` : `${(metres * 100).toFixed(0)} cm`, "gpu");
            widthOut.set(`${centre.pixels.toFixed(1)} px`);
            blendOut.set(centre.blend.toFixed(2));
            if (centre.level === 0 && centre.pixels > FADE)
                note.set("The finest cells are wider than F pixels: only their level and the next show, at full strength.", "ok");
            else
                note.set(`The centre shows level A fading out (blend ${centre.blend.toFixed(2)}), the next level as the minor lines, and the one after fading in as the major lines.`, "ok");
        } else {
            levelOut.set("none");
            widthOut.set("–");
            blendOut.set("–");
            note.set("The centre pixel looks above the horizon: its ray never meets the plane.", "bad");
        }
    });
});

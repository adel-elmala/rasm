// Labs of docs/guides/tiled-export.html.

// A perspective camera from above, its image cut into four tiles. The point's pixel in the whole image comes from the
// projection; its pixel in the chosen tile comes from the same projection followed by the tile's clip-space scale and
// shift (scale W / R, shift (W - 2 r) / R - 1). The reader drags the point and picks the tile: the tile's pixel plus the
// tile's offset is always the image's pixel, and the point shows in the tile only when it falls in the tile's columns.
RasmLab.register("tile-window", function (lab) {
    const svg = lab.svg(760, 300, "A camera seen from above, its frustum, and a point you drag. Above, the image strip of 800 pixels cut into four tiles of 200, with the point's pixel; below, the chosen tile drawn alone, with the point's pixel in it.");
    const tileChoice = lab.choice({ label: "Tile", options: ["0", "1", "2", "3"], value: "1" });
    const imageOut = lab.readout("pixel in the image");
    const tileOut = lab.readout("pixel in the tile + offset");
    const note = lab.note();

    const W = 800;
    const R = 200;
    const FOCAL = 1.2;
    const EYE = { x: 380, y: 280 };
    const DEPTH_SCALE = 200;
    const STRIP_X = 180;
    const STRIP_W = 400;

    lab.el(svg, "text", { x: 20, y: 22, class: "title", text: "the image, 800 px" });
    lab.el(svg, "rect", { x: STRIP_X, y: 10, width: STRIP_W, height: 18, class: "paper" });
    lab.el(svg, "path", { d: `M${STRIP_X + 100},10 V28 M${STRIP_X + 200},10 V28 M${STRIP_X + 300},10 V28`, class: "grid" });
    const chosen = lab.el(svg, "rect", { y: 10, width: STRIP_W / 4, height: 18, class: "accent o-50" });
    const imageDot = lab.el(svg, "circle", { cy: 19, r: 5, class: "hot" });
    lab.el(svg, "text", { x: 20, y: 62, class: "title", text: "the tile alone, 200 px" });
    lab.el(svg, "rect", { x: STRIP_X, y: 50, width: STRIP_W, height: 18, class: "paper" });
    const tileDot = lab.el(svg, "circle", { cy: 59, r: 5, class: "hot" });
    const tileLabel = lab.el(svg, "text", { x: STRIP_X + STRIP_W + 10, y: 64, class: "small" });

    // The frustum's edges: x = ±z / FOCAL, z in view units below the eye.
    const edge = 1 / FOCAL * DEPTH_SCALE;
    lab.el(svg, "path", { d: `M${EYE.x},${EYE.y} L${EYE.x - edge},${EYE.y - DEPTH_SCALE} M${EYE.x},${EYE.y} L${EYE.x + edge},${EYE.y - DEPTH_SCALE}`, class: "arrow" });
    const slices = lab.el(svg, "path", { class: "accent o-25" });
    lab.el(svg, "circle", { cx: EYE.x, cy: EYE.y, r: 6, class: "box" });
    const handle = lab.handle(svg, { x: 355, y: 160, label: "Move the point", minX: 200, maxX: 560, minY: 90, maxY: 250 });

    // NDC x of a point (x right, z forward) and its pixel in an image of `width` pixels.
    function toNdc(x, z) {
        return (x / z) * FOCAL;
    }
    function toPixel(ndc, width) {
        return (ndc * 0.5 + 0.5) * width;
    }

    lab.draw(function () {
        const tile = Number(tileChoice.value);
        const start = tile * R;
        const x = (handle.x - EYE.x) / DEPTH_SCALE;
        const z = (EYE.y - handle.y) / DEPTH_SCALE;
        const ndc = toNdc(x, z);
        const imagePixel = toPixel(ndc, W);
        // The tile's clip transform: scale and shift, times w; in NDC it is the same scale and shift.
        const scale = W / R;
        const shift = (W - 2 * start) / R - 1;
        const tileNdc = ndc * scale + shift;
        const tilePixel = toPixel(tileNdc, R);
        const inTile = tilePixel >= 0 && tilePixel < R;

        lab.set(chosen, { x: STRIP_X + tile * STRIP_W / 4 });
        const inImage = imagePixel >= 0 && imagePixel < W;
        lab.set(imageDot, { cx: STRIP_X + lab.clamp(imagePixel, 0, W) / W * STRIP_W, class: inImage ? "hot" : "hot o-25" });
        lab.set(tileDot, { cx: STRIP_X + lab.clamp(tilePixel, 0, R) / R * STRIP_W, class: inTile ? "hot" : "hot o-25" });
        lab.set(tileLabel, { text: `x' = ${scale.toFixed(0)} x ${shift >= 0 ? "+" : "−"} ${Math.abs(shift).toFixed(0)}` });
        // The chosen tile's slice of the frustum: between its left and right NDC.
        const left = (start / W) * 2 - 1;
        const right = ((start + R) / W) * 2 - 1;
        const far = DEPTH_SCALE;
        lab.set(slices, { d: `M${EYE.x},${EYE.y} L${EYE.x + left / FOCAL * far},${EYE.y - far} L${EYE.x + right / FOCAL * far},${EYE.y - far} Z` });

        imageOut.set(inImage ? imagePixel.toFixed(1) : "off the image", inImage ? "gpu" : "bad");
        tileOut.set(`${tilePixel.toFixed(1)} + ${start} = ${(tilePixel + start).toFixed(1)}`, inTile ? "ok" : "");
        if (!inImage)
            note.set("The point is outside the camera's view: no tile draws it.", "bad");
        else if (inTile)
            note.set(`Tile ${tile} draws the point at pixel ${tilePixel.toFixed(1)}; plus the tile's offset ${start}, that is the image's pixel.`, "ok");
        else
            note.set(`The point is in another tile: tile ${tile} puts it at ${tilePixel.toFixed(1)}, outside its 0 to 200, and the clip test drops it.`);
    });
});

// A row of 240 pixels with three bright spots, blurred by three box blurs of radius r (reach 3 r). The whole row is
// blurred at once, clamping at its own ends. The tiled row is cut in tiles of 60: each is blurred over its render rect
// (the tile and the guard on each inner side, inside the row), clamping at that rect's ends, and only the tile is kept.
// The difference shows the seams, which vanish once the guard is at least the reach.
RasmLab.register("guard-band", function (lab) {
    const svg = lab.svg(760, 260, "Two curves of a blurred row of pixels: the row blurred whole, and the row blurred tile by tile with a guard band. Below them, the difference, which spikes at the tile borders while the guard is smaller than the blur's reach.");
    const radiusSlider = lab.slider({ label: "Blur radius r (reach 3 r)", min: 1, max: 14, step: 1, value: 8 });
    const guardSlider = lab.slider({ label: "Guard band", min: 0, max: 48, step: 4, value: 12 });
    const errorOut = lab.readout("largest seam");
    const costOut = lab.readout("pixels drawn per pixel kept");
    const note = lab.note();

    const N = 240;
    const TILE = 60;
    const X0 = 30;
    const STEP = 3;
    const signal = [];
    for (let i = 0; i < N; i++)
        signal.push(0.1 + (i >= 50 && i < 66 ? 1 : 0) + (i >= 112 && i < 124 ? 0.8 : 0) + (i >= 174 && i < 184 ? 1.1 : 0));

    lab.el(svg, "text", { x: X0, y: 18, class: "title", text: "blurred whole" });
    const whole = lab.el(svg, "polyline", { class: "arrow gpu nofill" });
    lab.el(svg, "text", { x: X0, y: 110, class: "title", text: "blurred in tiles of 60, with the guard" });
    const tiled = lab.el(svg, "polyline", { class: "arrow cpu nofill" });
    lab.el(svg, "text", { x: X0, y: 202, class: "title", text: "difference, magnified" });
    const diff = lab.el(svg, "g", {});
    let borders = "";
    for (let t = TILE; t < N; t += TILE)
        borders += `M${X0 + t * STEP},24 V250 `;
    lab.el(svg, "path", { d: borders, class: "arrow dashed o-50" });

    // Box blur of radius r over a[lo .. hi), clamping at its ends.
    function box(a, lo, hi, r) {
        const out = a.slice();
        for (let i = lo; i < hi; i++) {
            let sum = 0;
            for (let k = -r; k <= r; k++)
                sum += a[lab.clamp(i + k, lo, hi - 1)];
            out[i] = sum / (2 * r + 1);
        }
        return out;
    }
    function blur(a, lo, hi, r) {
        let b = a;
        for (let pass = 0; pass < 3; pass++)
            b = box(b, lo, hi, r);
        return b;
    }
    function points(values, top, height) {
        return values.map(function (v, i) { return `${X0 + i * STEP},${top + height - Math.min(v, 1.3) / 1.3 * height}`; }).join(" ");
    }

    lab.draw(function () {
        const r = radiusSlider.value;
        const guard = guardSlider.value;
        const single = blur(signal, 0, N, r);
        const result = new Array(N).fill(0);
        let drawn = 0;
        for (let t = 0; t < N; t += TILE) {
            const lo = Math.max(0, t - guard);
            const hi = Math.min(N, t + TILE + guard);
            drawn += hi - lo;
            const b = blur(signal, lo, hi, r);
            for (let i = t; i < t + TILE; i++)
                result[i] = b[i];
        }
        lab.set(whole, { points: points(single, 26, 70) });
        lab.set(tiled, { points: points(result, 118, 70) });
        lab.clear(diff);
        let worst = 0;
        for (let i = 0; i < N; i++) {
            const d = Math.abs(result[i] - single[i]);
            worst = Math.max(worst, d);
            if (d > 0.002)
                lab.el(diff, "rect", { x: X0 + i * STEP, y: 250 - Math.min(d * 3000, 40), width: STEP, height: Math.min(d * 3000, 40), class: "bad" });
        }
        const reach = 3 * r;
        errorOut.set(worst < 1e-9 ? "0" : worst.toFixed(3), worst < 1e-9 ? "ok" : "bad");
        costOut.set((drawn / N).toFixed(2));
        if (worst < 1e-9)
            note.set(`The guard (${guard}) covers the blur's reach (${reach}): every tile matches the whole row exactly.`, "ok");
        else
            note.set(`The blur reaches ${reach} pixels, past the guard of ${guard}: near each border a tile clamps where the whole row has pixels.`, "bad");
    });
});

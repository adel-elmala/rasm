// Labs of docs/guides/lod-selection.html.

// The worked example of section 7 with the depth, the field of view and the screen height free: each level's
// error in pixels, the coarsest level under the limit, and the depths where the level switches.
RasmLab.register("lod-pick", function (lab) {
    const svg = lab.svg(760, 286, "A depth axis split into one band per level, with a marker at the object's depth, and one bar per level for its error on screen against a limit of 1 pixel.");
    const depth = lab.slider({ label: "Depth of the center w", min: 1, max: 300, step: 1, value: 20, format: function (v) { return `${v} m`; } });
    const fov = lab.slider({ label: "Field of view", min: 20, max: 100, step: 5, value: 60, format: function (v) { return `${v}°`; } });
    const height = lab.choice({ label: "Screen height", options: [{ value: 1080, label: "1080 px" }, { value: 2160, label: "2160 px" }], value: 1080 });
    const levelOut = lab.readout("the level that is drawn");
    const errorOut = lab.readout("its error on screen");
    const trianglesOut = lab.readout("triangles drawn, against the full mesh");
    const note = lab.note();

    const ERRORS = [0, 0.005, 0.012, 0.03, 0.08];     // metres, the table of section 7
    const RADIUS = 1;
    const SCALE = 1;
    const TAU = 1;
    const AXIS = { x: 60, width: 670, y: 44, h: 30 };  // 1 m to 1000 m, log scale
    const BAR = { x: 200, perPixel: 100, max: 400 };

    function axisX(metres) {
        return AXIS.x + AXIS.width * lab.clamp(Math.log10(Math.max(metres, 1)) / 3, 0, 1);
    }

    const title = lab.el(svg, "text", { x: 20, y: 22, class: "title" });
    const bands = [];
    const bandNames = [];
    const switchLabels = [];
    for (let i = 0; i < ERRORS.length; i++) {
        bands.push(lab.el(svg, "rect", { y: AXIS.y, height: AXIS.h, rx: 3 }));
        bandNames.push(lab.el(svg, "text", { y: AXIS.y + 20, class: "mid", text: `LOD ${i}` }));
        switchLabels.push(lab.el(svg, "text", { y: AXIS.y + 46, class: "mid small" }));
    }
    lab.el(svg, "text", { x: AXIS.x, y: AXIS.y + 46, class: "small t-faint", text: "1 m" });
    lab.el(svg, "text", { x: AXIS.x + AXIS.width, y: AXIS.y + 46, class: "end small t-faint", text: "1000 m" });
    lab.el(svg, "text", { x: 20, y: AXIS.y + 66, class: "small", text: "depth of the sphere's nearest point, w − r (each step to the right is ten times farther)" });
    const marker = lab.el(svg, "line", { y1: AXIS.y - 8, y2: AXIS.y - 1, class: "arrow hot thick" });
    const markerFoot = lab.el(svg, "line", { y1: AXIS.y + AXIS.h + 1, y2: AXIS.y + AXIS.h + 7, class: "arrow hot thick" });
    const markerDot = lab.el(svg, "circle", { cy: AXIS.y - 8, r: 5, class: "badge" });

    lab.el(svg, "line", { x1: BAR.x, y1: 130, x2: BAR.x, y2: 254, class: "arrow" });
    lab.el(svg, "text", { x: BAR.x, y: 270, class: "mid small", text: "0 px" });
    const rows = [];
    for (let i = 1; i < ERRORS.length; i++) {
        const y = 136 + (i - 1) * 30;
        lab.el(svg, "text", { x: 20, y: y + 15, class: "small", text: `LOD ${i} · error ${ERRORS[i]} m` });
        rows.push({
            bar: lab.el(svg, "rect", { x: BAR.x, y: y, height: 20, rx: 2 }),
            value: lab.el(svg, "text", { y: y + 15, class: "small" }),
        });
    }
    const limitX = BAR.x + TAU * BAR.perPixel;
    lab.el(svg, "line", { x1: limitX, y1: 130, x2: limitX, y2: 254, class: "hot dashed" });
    lab.el(svg, "text", { x: limitX, y: 270, class: "mid small t-hot", text: "limit: 1 px" });

    lab.draw(function () {
        const p11 = 1 / Math.tan(fov.value * Math.PI / 360);
        const pixelsAtOneMetre = p11 * height.value / 2;       // 935.3 for 60° and 1080
        const nearest = depth.value - RADIUS;

        // the rule of section 4: the last level with e <= eMax; LOD 0 when the sphere reaches the camera
        let level = 0;
        let pixelsPerUnit = 0;
        if (nearest > 0) {
            pixelsPerUnit = pixelsAtOneMetre / nearest;
            const eMax = TAU / (pixelsPerUnit * SCALE);
            for (let i = 0; i < ERRORS.length; i++) if (ERRORS[i] <= eMax) level = i;
        }

        if (nearest > 0) lab.set(title, { text: `w − r = ${nearest} m · pixels per metre there: ${pixelsAtOneMetre.toFixed(1)} / ${nearest} = ${pixelsPerUnit.toFixed(1)}` });
        else lab.set(title, { text: "w − r = 0 m: the sphere reaches the camera" });

        // level i is allowed from the depth where its error is exactly the limit
        const from = ERRORS.map(function (e) { return e * SCALE * pixelsAtOneMetre / TAU; });
        for (let i = 0; i < ERRORS.length; i++) {
            const x0 = axisX(from[i]);
            const x1 = i + 1 < ERRORS.length ? axisX(from[i + 1]) : AXIS.x + AXIS.width;
            lab.set(bands[i], { x: x0.toFixed(1), width: Math.max(0, x1 - x0 - 2).toFixed(1), class: i === level ? "gpu" : "box" });
            lab.set(bandNames[i], { x: ((x0 + x1) / 2).toFixed(1), class: x1 - x0 > 50 ? (i === level ? "mid title" : "mid") : "hide" });
            lab.set(switchLabels[i], { x: x0.toFixed(1), text: i > 0 ? `${from[i].toFixed(1)} m` : "" });
        }
        const mx = axisX(nearest);
        lab.set(marker, { x1: mx, x2: mx });
        lab.set(markerFoot, { x1: mx, x2: mx });
        lab.set(markerDot, { cx: mx });

        rows.forEach(function (row, k) {
            const i = k + 1;
            const pixels = nearest > 0 ? ERRORS[i] * SCALE * pixelsPerUnit : Infinity;
            const under = pixels <= TAU;
            const width = Math.min(BAR.max, pixels * BAR.perPixel);
            const clipped = pixels * BAR.perPixel > BAR.max;
            lab.set(row.bar, { width: width.toFixed(1), class: under ? "gpu" : "hot" });
            let tx = BAR.x + width + 8;
            if (tx < limitX + 4 && tx + 46 > limitX - 4) tx = limitX + 8;     // keep the number off the limit line
            const text = nearest > 0 ? `${pixels.toFixed(2)} px${clipped ? " →" : ""}` : "no limit →";
            lab.set(row.value, { x: tx.toFixed(1), text: text, class: under ? "small t-gpu" : "small t-hot" });
        });

        const shown = nearest > 0 ? ERRORS[level] * SCALE * pixelsPerUnit : 0;
        levelOut.set(`LOD ${level}`, "gpu");
        errorOut.set(`${shown.toFixed(2)} px`, "ok");
        trianglesOut.set(level === 0 ? "all" : `about 1/${Math.pow(2, level)}`);
        if (nearest <= 0) note.set("The sphere reaches the camera: w − r is 0, so the rule gives LOD 0 without dividing.");
        else if (level === 0) note.set(`LOD 0: even LOD 1 would move the surface by ${(ERRORS[1] * SCALE * pixelsPerUnit).toFixed(2)} px, more than the limit. The full mesh is drawn.`);
        else if (level === ERRORS.length - 1) note.set(`LOD ${level}: every level is under 1 px, so the coarsest one is drawn.`, "ok");
        else note.set(`LOD ${level} is the coarsest level under 1 px. LOD ${level + 1} would show ${(ERRORS[level + 1] * SCALE * pixelsPerUnit).toFixed(2)} px.`, "ok");
    });
});

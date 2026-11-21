// Labs of docs/guides/msaa.html.

// AgX for a gray, as on the tone-mapping page (a gray stays gray through both matrices), and the sRGB byte the screen
// gets for a linear value.
const MsaaTone = (function () {
    function agxGray(x) {
        const ev = Math.max(-12.47393, Math.min(4.026069, Math.log2(Math.max(x, 1e-10))));
        const t = (ev + 12.47393) / 16.5;
        const t2 = t * t;
        const t4 = t2 * t2;
        const v = 15.5 * t4 * t2 - 40.14 * t4 * t + 31.96 * t4 - 6.868 * t2 * t + 0.4298 * t2 + 0.1191 * t - 0.00232;
        return Math.pow(Math.max(0, Math.min(1, v)), 2.2);
    }
    function toByte(linear) {
        const s = linear <= 0.0031308 ? 12.92 * linear : 1.055 * Math.pow(linear, 1 / 2.4) - 0.055;
        return Math.round(255 * Math.max(0, Math.min(1, s)));
    }
    return { agxGray: agxGray, toByte: toByte };
})();

// One edge pixel with four samples: some see the sky, the others a dark roof. Three ways to make one value of them:
// the plain mean (a resolve attachment), the mean weighted by 1 / (1 + value) (the resolve pass), and the mean of what
// each sample would show on screen (the ideal, which needs the tone curve before the mean).
RasmLab.register("msaa-resolve", function (lab) {
    const svg = lab.svg(760, 250, "One pixel split into four samples, each sky or roof. Three bars show the byte on screen for the plain mean of the samples, the weighted mean, and the ideal: the mean of what each sample would show.");
    const sky = lab.slider({ label: "Sky brightness", min: 0.5, max: 64, step: 0.5, value: 8, format: function (v) { return v.toFixed(1); } });
    const roofSamples = lab.slider({ label: "Samples on the roof", min: 0, max: 4, step: 1, value: 2, format: function (v) { return `${v} of 4`; } });
    const plainOut = lab.readout("plain mean, byte");
    const weightedOut = lab.readout("weighted mean, byte");
    const idealOut = lab.readout("ideal, byte");
    const note = lab.note();

    const ROOF = 0.05;
    const POSITIONS = [[0.375, 0.125], [0.875, 0.375], [0.125, 0.625], [0.625, 0.875]];
    lab.el(svg, "text", { x: 40, y: 24, class: "title", text: "one pixel, 4 samples" });
    lab.el(svg, "rect", { x: 40, y: 40, width: 180, height: 180, class: "paper" });
    const samples = POSITIONS.map(function (p) {
        const dot = lab.el(svg, "circle", { cx: 40 + p[0] * 180, cy: 40 + p[1] * 180, r: 16 });
        const label = lab.el(svg, "text", { x: 40 + p[0] * 180, y: 40 + p[1] * 180 + 34, class: "mid small" });
        return { dot: dot, label: label };
    });
    const bars = ["plain mean", "weighted mean", "ideal"].map(function (name, i) {
        const y = 52 + i * 62;
        lab.el(svg, "text", { x: 290, y: y - 6, class: "small", text: name });
        lab.el(svg, "rect", { x: 290, y: y, width: 400, height: 28, class: "zone" });
        const fill = lab.el(svg, "rect", { x: 290, y: y, height: 28 });
        const value = lab.el(svg, "text", { x: 700, y: y + 19, class: "small mono" });
        return { fill: fill, value: value };
    });

    function gray(byte) {
        return `rgb(${byte}, ${byte}, ${byte})`;
    }

    lab.draw(function () {
        const values = POSITIONS.map(function (p, i) { return i < roofSamples.value ? ROOF : sky.value; });
        values.forEach(function (v, i) {
            const byte = MsaaTone.toByte(MsaaTone.agxGray(v));
            lab.set(samples[i].dot, { fill: gray(byte) });
            lab.set(samples[i].label, { text: v === ROOF ? "roof 0.05" : `sky ${sky.value.toFixed(1)}` });
        });
        const plain = values.reduce(function (a, b) { return a + b; }, 0) / values.length;
        const weights = values.map(function (v) { return 1 / (1 + v); });
        const weighted = values.reduce(function (sum, v, i) { return sum + v * weights[i]; }, 0) / weights.reduce(function (a, b) { return a + b; }, 0);
        const plainByte = MsaaTone.toByte(MsaaTone.agxGray(plain));
        const weightedByte = MsaaTone.toByte(MsaaTone.agxGray(weighted));
        const idealByte = Math.round(values.reduce(function (sum, v) { return sum + MsaaTone.toByte(MsaaTone.agxGray(v)); }, 0) / values.length);
        [plainByte, weightedByte, idealByte].forEach(function (byte, i) {
            lab.set(bars[i].fill, { width: 400 * byte / 255, fill: gray(byte) });
            lab.set(bars[i].value, { text: `${byte}` });
        });
        const plainError = Math.abs(plainByte - idealByte);
        const weightedError = Math.abs(weightedByte - idealByte);
        plainOut.set(`${plainByte}`, plainError > 20 ? "bad" : "ok");
        weightedOut.set(`${weightedByte}`, weightedError > 20 ? "bad" : "ok");
        idealOut.set(`${idealByte}`);
        if (roofSamples.value === 0 || roofSamples.value === 4)
            note.set("Every sample sees the same thing, so all three give the same byte: no edge in this pixel.", "ok");
        else if (plainError > weightedError)
            note.set(`The plain mean is ${plainError} bytes from the ideal: one bright sample pulls the pixel toward the sky. The weighted mean is ${weightedError} bytes off.`, "ok");
        else
            note.set("With a dim sky the samples are close, and both means are near the ideal.", "ok");
    });
});

// A row of pixels across a cut-out's edge, where the texture's alpha falls from 1 to 0. Three ways to turn alpha into
// covered samples: discard below the cutoff (all or nothing), alpha itself as coverage, and the sharpened alpha
// (alpha - cutoff) / fwidth(alpha) + 0.5.
RasmLab.register("alpha-to-coverage", function (lab) {
    const svg = lab.svg(760, 260, "A row of 16 pixels across the edge of a leaf. The top row shows the texture's alpha falling from left to right. Three rows below show how many of 4 samples each pixel covers: with discard, with alpha as coverage, and with the sharpened alpha.");
    const cutoff = lab.slider({ label: "Cutoff", min: 0.1, max: 0.9, step: 0.05, value: 0.5, format: function (v) { return v.toFixed(2); } });
    const steep = lab.slider({ label: "Alpha change per pixel", min: 0.05, max: 0.6, step: 0.05, value: 0.15, format: function (v) { return v.toFixed(2); } });
    const edgeOut = lab.readout("edge, sharpened (pixel)");
    const edgeDiscardOut = lab.readout("edge, discard (pixel)");
    const note = lab.note();

    const N = 16;
    const X = 150;
    const W = 36;
    const rows = ["alpha", "discard", "alpha as coverage", "sharpened"];
    rows.forEach(function (name, r) {
        lab.el(svg, "text", { x: X - 10, y: 50 + r * 54, class: "end small", text: name });
    });
    const cells = rows.map(function (name, r) {
        const row = [];
        for (let x = 0; x < N; x++)
            row.push(lab.el(svg, "rect", { x: X + x * W, y: 30 + r * 54, width: W - 2, height: 34 }));
        return row;
    });
    const mark = lab.el(svg, "line", { y1: 20, y2: 240, class: "arrow hot dashed" });

    function coverageGray(c) {
        const v = Math.round(225 - 170 * c);
        return `rgb(${v}, ${Math.round(225 - 90 * c)}, ${v})`;
    }

    lab.draw(function () {
        const alpha = [];
        for (let x = 0; x < N; x++) alpha.push(Math.max(0, Math.min(1, 1.2 - steep.value * x)));
        const quantize = function (c) { return Math.round(c * 4) / 4; };
        let edgeSharp = null;
        let edgeDiscard = null;
        for (let x = 0; x < N; x++) {
            const a = alpha[x];
            const discard = a >= cutoff.value ? 1 : 0;
            const sharp = Math.max(0, Math.min(1, (a - cutoff.value) / Math.max(steep.value, 1e-4) + 0.5));
            lab.set(cells[0][x], { fill: coverageGray(a) });
            lab.set(cells[1][x], { fill: coverageGray(discard) });
            lab.set(cells[2][x], { fill: coverageGray(quantize(a)) });
            lab.set(cells[3][x], { fill: coverageGray(quantize(sharp)) });
            if (edgeDiscard === null && discard === 0) edgeDiscard = x;
            if (edgeSharp === null && sharp < 0.5) edgeSharp = x;
        }
        // Where alpha crosses the cutoff, in pixels from the left edge of the row.
        const crossing = (1.2 - cutoff.value) / steep.value;
        lab.set(mark, { x1: X + (crossing + 0.5) * W, x2: X + (crossing + 0.5) * W });
        edgeOut.set(edgeSharp === null ? "–" : `${edgeSharp}`, "ok");
        edgeDiscardOut.set(edgeDiscard === null ? "–" : `${edgeDiscard}`);
        note.set(`The dashed line is where alpha crosses ${cutoff.value.toFixed(2)}. Discard turns the pixels at once; the sharpened alpha covers 0 to 4 samples over about one pixel around that line, so the edge is smooth and stays where the cutoff puts it. Alpha as coverage spreads it over ${Math.round(1 / steep.value)} pixels and moves it.`, "ok");
    });
});

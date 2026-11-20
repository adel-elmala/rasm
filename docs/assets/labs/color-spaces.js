// Labs of docs/guides/color-spaces.html.

// The sRGB transfer functions of IEC 61966-2-1, on values from 0 to 1.
const RasmSrgb = {
    decode: function (s) { return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4); },
    encode: function (l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * Math.pow(l, 1 / 2.4) - 0.055; },
};

// One stored byte, followed through the decode curve to the light it shows.
RasmLab.register("srgb-byte", function (lab) {
    const svg = lab.svg(760, 300, "A plot of the sRGB curve with one point on it: the stored byte on the code axis and the light it shows on the linear axis. Beside it, the byte as a gray and two bars: the code and the light.");
    const byte = lab.slider({ label: "Stored byte", min: 0, max: 255, value: 128 });
    const codeOut = lab.readout("sRGB code s = byte / 255");
    const lightOut = lab.readout("linear light L = decode(s)");
    const shortcutOut = lab.readout("L with the 2.2 shortcut");
    const note = lab.note();

    const P = { x: 60, y: 40, size: 220 };
    function px(l) { return P.x + l * P.size; }
    function py(s) { return P.y + P.size - s * P.size; }

    lab.el(svg, "rect", { x: P.x, y: P.y, width: P.size, height: P.size, class: "zone" });
    lab.el(svg, "line", { x1: px(0), y1: py(0), x2: px(1), y2: py(1), class: "arrow dashed" });
    let curve = "";
    for (let k = 0; k <= 110; k++) {
        const l = Math.pow(k / 110, 2.2);       // more points near black, where the curve bends most
        curve += `${k ? "L" : "M"}${px(l).toFixed(1)},${py(RasmSrgb.encode(l)).toFixed(1)}`;
    }
    lab.el(svg, "path", { d: curve, class: "arrow gpu thick" });
    lab.el(svg, "text", { x: P.x - 6, y: py(0) + 4, class: "end small", text: "0" });
    lab.el(svg, "text", { x: P.x - 6, y: py(1) + 4, class: "end small", text: "1" });
    lab.el(svg, "text", { x: px(1), y: py(0) + 18, class: "mid small", text: "1" });
    lab.el(svg, "text", { x: px(0.5), y: 296, class: "mid small", text: "linear light L" });
    lab.el(svg, "text", { x: P.x, y: 28, class: "small", text: "code s" });
    lab.el(svg, "text", { x: 230, y: 128, class: "small t-faint", text: "s = L" });
    const guide = lab.el(svg, "path", { class: "arrow hot dashed" });
    const dot = lab.el(svg, "circle", { r: 5, class: "accent" });
    const sLabel = lab.el(svg, "text", { x: P.x - 6, class: "end small t-hot" });
    const lLabel = lab.el(svg, "text", { y: py(0) + 18, class: "mid small t-hot" });

    const BAR = { x: 420, width: 300 };
    lab.el(svg, "text", { x: 350, y: 56, class: "title", text: "What the byte is, and what it shows" });
    lab.el(svg, "text", { x: 410, y: 99, class: "end small", text: "the pixel" });
    const swatch = lab.el(svg, "rect", { x: BAR.x, y: 78, width: 64, height: 32 });
    lab.el(svg, "rect", { x: BAR.x, y: 78, width: 64, height: 32, class: "grid" });
    const swatchText = lab.el(svg, "text", { x: BAR.x + 76, y: 99, class: "mono" });
    lab.el(svg, "text", { x: 410, y: 151, class: "end small", text: "code s" });
    lab.el(svg, "rect", { x: BAR.x, y: 134, width: BAR.width, height: 24, class: "zone" });
    const codeBar = lab.el(svg, "rect", { x: BAR.x, y: 134, height: 24, class: "cpu" });
    lab.el(svg, "text", { x: 410, y: 203, class: "end small", text: "light L" });
    lab.el(svg, "rect", { x: BAR.x, y: 186, width: BAR.width, height: 24, class: "zone" });
    const lightBar = lab.el(svg, "rect", { x: BAR.x, y: 186, height: 24, class: "light" });
    lab.el(svg, "path", { d: `M${BAR.x + BAR.width / 2},126V218`, class: "dashed" });
    lab.el(svg, "text", { x: BAR.x + BAR.width / 2, y: 234, class: "mid small t-faint", text: "half" });
    lab.el(svg, "text", { x: 350, y: 262, class: "small", text: "Blue: how far the byte is along the code range." });
    lab.el(svg, "text", { x: 350, y: 280, class: "small", text: "Yellow: how much of full light the screen gives." });

    lab.draw(function () {
        const s = byte.value / 255;
        const l = RasmSrgb.decode(s);
        lab.set(guide, { d: `M${P.x},${py(s).toFixed(1)}H${px(l).toFixed(1)}V${py(0)}` });
        lab.set(dot, { cx: px(l), cy: py(s) });
        lab.set(sLabel, { y: lab.clamp(py(s) + 4, py(1) + 16, py(0) - 12), text: s.toFixed(3) });
        lab.set(lLabel, { x: lab.clamp(px(l), px(0) + 18, px(1) - 24), text: l.toFixed(3) });
        lab.set(swatch, { fill: `rgb(${byte.value},${byte.value},${byte.value})` });
        lab.set(swatchText, { text: `byte ${byte.value}` });
        lab.set(codeBar, { width: (s * BAR.width).toFixed(1) });
        lab.set(lightBar, { width: (l * BAR.width).toFixed(1) });

        codeOut.set(s.toFixed(3), "cpu");
        lightOut.set(l.toFixed(l < 0.1 ? 4 : 3));
        shortcutOut.set(Math.pow(s, 2.2).toFixed(l < 0.1 ? 4 : 3));
        const percent = `${(100 * s).toFixed(0)} % of the code range, but ${(100 * l).toFixed(1)} % of full light`;
        if (s <= 0.04045) note.set(`Byte ${byte.value} is on the straight piece near black: L = s / 12.92. The 2.2 shortcut is far off here.`);
        else if (byte.value === 255) note.set("Byte 255 is full light. The two bars only meet at the two ends.");
        else note.set(`Byte ${byte.value} is ${percent}.`);
    });
});

// Two colors mixed step by step, once on the stored codes and once on linear light.
RasmLab.register("mix-colors", function (lab) {
    const svg = lab.svg(760, 250, "Two bars that go from one color to another. The top bar mixes the sRGB codes. The bottom bar mixes linear light and encodes the result. A marker shows one mix on both.");
    const PAIRS = {
        gray: { label: "black + white", a: [0, 0, 0], b: [255, 255, 255] },
        redGreen: { label: "red + green", a: [255, 0, 0], b: [0, 255, 0] },
        blueYellow: { label: "blue + yellow", a: [0, 0, 255], b: [255, 255, 0] },
    };
    const pair = lab.choice({
        label: "Colors",
        options: Object.keys(PAIRS).map(function (key) { return { value: key, label: PAIRS[key].label }; }),
        value: "gray",
    });
    const mix = lab.slider({ label: "Mix", min: 0, max: 100, value: 50, format: function (v) { return `${v} %`; } });
    const codesOut = lab.readout("the mix on codes, as bytes");
    const linearOut = lab.readout("the mix in linear, as bytes");
    const lossOut = lab.readout("light of the codes mix, against the linear mix");
    const note = lab.note();

    const BAR = { x: 150, width: 440, height: 56 };
    const STEPS = 88;
    const ROWS = [{ y: 40, title: "on codes", linear: false }, { y: 132, title: "in linear", linear: true }];

    // one mix of two byte colors: t = 0 gives a, t = 1 gives b
    function blend(a, b, t, linear) {
        return a.map(function (unused, c) {
            if (!linear) return Math.round(a[c] * (1 - t) + b[c] * t);
            const light = RasmSrgb.decode(a[c] / 255) * (1 - t) + RasmSrgb.decode(b[c] / 255) * t;
            return Math.round(255 * RasmSrgb.encode(light));
        });
    }
    function light(bytes) {
        return bytes.reduce(function (sum, v) { return sum + RasmSrgb.decode(v / 255); }, 0);
    }
    function bytesText(bytes) {
        return bytes[0] === bytes[1] && bytes[1] === bytes[2] ? String(bytes[0]) : bytes.join(", ");
    }

    ROWS.forEach(function (row) {
        lab.el(svg, "text", { x: BAR.x - 12, y: row.y + 26, class: "end title", text: row.title });
        lab.el(svg, "text", { x: BAR.x - 12, y: row.y + 44, class: "end small", text: row.linear ? "decode, mix, encode" : "mix the bytes" });
        row.cells = [];
        for (let k = 0; k < STEPS; k++) {
            row.cells.push(lab.el(svg, "rect", { x: BAR.x + k * BAR.width / STEPS, y: row.y, width: BAR.width / STEPS + 0.6, height: BAR.height }));
        }
        lab.el(svg, "rect", { x: BAR.x, y: row.y, width: BAR.width, height: BAR.height, class: "grid" });
        row.swatch = lab.el(svg, "rect", { x: 620, y: row.y, width: 56, height: BAR.height });
        lab.el(svg, "rect", { x: 620, y: row.y, width: 56, height: BAR.height, class: "grid" });
        row.text = lab.el(svg, "text", { x: 648, y: row.y + BAR.height + 16, class: "mid mono small" });
    });
    lab.el(svg, "text", { x: 648, y: 28, class: "mid small", text: "this mix" });
    const marker = lab.el(svg, "path", { class: "arrow hot" });
    const markerText = lab.el(svg, "text", { y: 28, class: "mid small t-hot" });
    lab.el(svg, "text", { x: BAR.x, y: 232, class: "small", text: "0 %: the first color" });
    lab.el(svg, "text", { x: BAR.x + BAR.width, y: 232, class: "end small", text: "100 %: the second color" });

    lab.draw(function () {
        const colors = PAIRS[pair.value];
        const t = mix.value / 100;
        ROWS.forEach(function (row) {
            row.cells.forEach(function (cell, k) {
                lab.set(cell, { fill: `rgb(${blend(colors.a, colors.b, k / (STEPS - 1), row.linear).join(",")})` });
            });
            row.result = blend(colors.a, colors.b, t, row.linear);
            lab.set(row.swatch, { fill: `rgb(${row.result.join(",")})` });
            lab.set(row.text, { text: bytesText(row.result) });
        });
        const x = BAR.x + t * BAR.width;
        lab.set(marker, { d: `M${x.toFixed(1)},34V${ROWS[1].y + BAR.height + 6}` });
        lab.set(markerText, { x: x.toFixed(1), text: `${mix.value} %` });

        const share = 100 * light(ROWS[0].result) / light(ROWS[1].result);
        codesOut.set(bytesText(ROWS[0].result), share < 90 ? "bad" : "");
        linearOut.set(bytesText(ROWS[1].result), "ok");
        lossOut.set(`${share.toFixed(0)} %`, share < 90 ? "bad" : "ok");
        if (share >= 99.5) note.set("At the two ends nothing is mixed, so both ways give the same color.", "ok");
        else if (share < 90) note.set(`The mix on codes sends only ${share.toFixed(0)} % of the light it should: the middle of the top bar is too dark.`, "bad");
        else note.set(`Close to one end the two ways nearly agree: the mix on codes sends ${share.toFixed(0)} % of the light it should.`);
    });
});

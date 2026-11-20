// Labs of docs/guides/tone-mapping.html.

// The curves of the page, on linear values. agx() is the minimal AgX of the page, with the same matrices, range and
// polynomial: mul(M, v) with M written row by row, so out[i] = dot(row i, v).
const ToneMap = (function () {
    const AGX_INSET = [
        [0.842479062253094, 0.0784335999999992, 0.0792237451477643],
        [0.0423282422610123, 0.878468636469772, 0.0791661274605434],
        [0.0423756549057051, 0.0784336, 0.879142973793104],
    ];
    const AGX_OUTSET = [
        [1.19687900512017, -0.0980208811401368, -0.0990297440797205],
        [-0.0528968517574562, 1.15190312990417, -0.0989611768448433],
        [-0.0529716355144438, -0.0980434501171241, 1.15107367264116],
    ];
    const AGX_MIN_EV = -12.47393;
    const AGX_MAX_EV = 4.026069;

    function mul(m, v) {
        return m.map(function (row) { return row[0] * v[0] + row[1] * v[1] + row[2] * v[2]; });
    }
    function saturate(x) {
        return Math.max(0, Math.min(1, x));
    }
    function agxContrast(x) {
        const x2 = x * x;
        const x4 = x2 * x2;
        return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    }
    function agx(rgb) {
        let v = mul(AGX_INSET, rgb.map(function (c) { return Math.max(c, 0); }));
        v = v.map(function (c) {
            const ev = Math.max(AGX_MIN_EV, Math.min(AGX_MAX_EV, Math.log2(Math.max(c, 1e-10))));
            return agxContrast((ev - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV));
        });
        v = mul(AGX_OUTSET, v);
        return v.map(function (c) { return Math.pow(saturate(c), 2.2); });
    }
    function acesFit(x) {
        return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    }
    function perChannel(f) {
        return function (rgb) { return rgb.map(f); };
    }
    // the sRGB encode the display target does after the curve
    function encode(l) {
        return l <= 0.0031308 ? 12.92 * l : 1.055 * Math.pow(l, 1 / 2.4) - 0.055;
    }
    const CURVES = {
        clip: { label: "Clip", cls: "arrow bad dashed", map: perChannel(function (x) { return saturate(x); }) },
        reinhard: { label: "Reinhard", cls: "arrow gpu", map: perChannel(function (x) { return x / (1 + x); }) },
        aces: { label: "ACES fit", cls: "arrow hot", map: perChannel(acesFit) },
        agx: { label: "AgX", cls: "arrow shader", map: agx },
    };
    return { CURVES: CURVES, encode: encode };
})();

// One gray value through each curve, on a log axis of scene values. Exposure moves the curves sideways.
RasmLab.register("tone-curve", function (lab) {
    const svg = lab.svg(760, 360, "A plot of four tone curves: scene value on a log axis, screen code from 0 to 1. Below it, a gray ramp drawn through the chosen curve.");
    const stops = lab.slider({ label: "Exposure", min: -4, max: 4, step: 0.5, value: 0, format: function (v) { return `${v > 0 ? "+" : ""}${v} stops`; } });
    const pick = lab.choice({ label: "Curve", value: "agx", options: [
        { value: "clip", label: "Clip" }, { value: "reinhard", label: "Reinhard" }, { value: "aces", label: "ACES fit" }, { value: "agx", label: "AgX" },
    ] });
    const factorOut = lab.readout("exposure factor");
    const grayOut = lab.readout("scene 0.18 on screen (code, byte)");
    const whiteOut = lab.readout("scene value that turns white");
    const note = lab.note();

    const X0 = 70;
    const X1 = 730;
    const Y0 = 30;
    const Y1 = 240;
    const LOG_MIN = -12;
    const LOG_MAX = 8;
    function sx(log) { return X0 + (log - LOG_MIN) / (LOG_MAX - LOG_MIN) * (X1 - X0); }
    function sy(code) { return Y1 - code * (Y1 - Y0); }

    lab.el(svg, "rect", { x: X0, y: Y0, width: X1 - X0, height: Y1 - Y0, class: "zone" });
    const TICKS = [[0.001, "0.001"], [0.01, "0.01"], [0.18, "0.18"], [1, "1"], [10, "10"], [100, "100"]];
    let grid = `M${X0},${sy(0.5)}H${X1}`;
    TICKS.forEach(function (t) {
        const x = sx(Math.log2(t[0]));
        grid += `M${x.toFixed(1)},${Y0}V${Y1}`;
        lab.el(svg, "text", { x: x, y: Y1 + 16, class: "mid small", text: t[1] });
    });
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "text", { x: X0 - 6, y: sy(0) + 4, class: "end small", text: "0" });
    lab.el(svg, "text", { x: X0 - 6, y: sy(0.5) + 4, class: "end small", text: "0.5" });
    lab.el(svg, "text", { x: X0 - 6, y: sy(1) + 4, class: "end small", text: "1" });
    lab.el(svg, "text", { x: X0, y: 20, class: "small", text: "on screen: sRGB code" });
    lab.el(svg, "text", { x: X1, y: Y1 + 32, class: "end small", text: "scene value, linear, on a log axis →" });

    const paths = {};
    Object.keys(ToneMap.CURVES).forEach(function (key) {
        paths[key] = lab.el(svg, "path", { class: ToneMap.CURVES[key].cls });
    });
    const grayLine = lab.el(svg, "path", { class: "arrow dashed" });
    const grayDot = lab.el(svg, "circle", { r: 5, class: "accent" });
    const whiteMark = lab.el(svg, "line", { y1: Y0, y2: Y1, class: "arrow hot dashed" });
    const whiteText = lab.el(svg, "text", { y: Y0 + 14, class: "small t-hot halo" });

    const RAMP_Y = 300;
    const RAMP_N = 80;
    const ramp = [];
    for (let i = 0; i < RAMP_N; i++) {
        const w = (X1 - X0) / RAMP_N;
        ramp.push(lab.el(svg, "rect", { x: (X0 + i * w).toFixed(1), y: RAMP_Y, width: (w + 0.6).toFixed(1), height: 34 }));
    }
    lab.el(svg, "rect", { x: X0, y: RAMP_Y, width: X1 - X0, height: 34, class: "ghost" });
    const rampLabel = lab.el(svg, "text", { x: X0, y: RAMP_Y - 8, class: "small" });
    const rampGray = lab.el(svg, "line", { y1: RAMP_Y - 4, y2: RAMP_Y + 38, class: "arrow hot" });

    function code(key, v) {
        return ToneMap.encode(ToneMap.CURVES[key].map([v, v, v])[1]);
    }

    lab.draw(function () {
        const e = Math.pow(2, stops.value);
        Object.keys(ToneMap.CURVES).forEach(function (key) {
            let d = "";
            for (let i = 0; i <= 200; i++) {
                const log = LOG_MIN + (LOG_MAX - LOG_MIN) * i / 200;
                const y = code(key, e * Math.pow(2, log));
                d += `${i ? "L" : "M"}${sx(log).toFixed(1)},${sy(y).toFixed(1)}`;
            }
            lab.set(paths[key], { d: d, class: ToneMap.CURVES[key].cls + (key === pick.value ? " thick" : " o-50") });
        });

        const key = pick.value;
        const grayCode = code(key, 0.18 * e);
        const gx = sx(Math.log2(0.18));
        lab.set(grayDot, { cx: gx, cy: sy(grayCode) });
        lab.set(grayLine, { d: `M${gx.toFixed(1)},${Y1}V${sy(grayCode).toFixed(1)}H${X0}` });

        // the smallest scene value whose code reaches 0.99 (byte 253)
        let white = null;
        for (let log = LOG_MIN; log <= LOG_MAX; log += 0.01) {
            if (code(key, e * Math.pow(2, log)) >= 0.99) { white = log; break; }
        }
        if (white === null) {
            lab.set(whiteMark, { x1: -10, x2: -10 });
            lab.set(whiteText, { text: "" });
            whiteOut.set(`above ${Math.pow(2, LOG_MAX).toFixed(0)}`, "ok");
        } else {
            const wx = sx(white);
            const v = Math.pow(2, white);
            lab.set(whiteMark, { x1: wx, x2: wx });
            const right = wx < X1 - 130;
            lab.set(whiteText, { x: right ? wx + 6 : wx - 6, class: `small t-hot halo${right ? "" : " end"}`, text: "white from here" });
            whiteOut.set(v < 10 ? v.toFixed(2) : v.toFixed(0), v < 2 ? "bad" : "ok");
        }

        for (let i = 0; i < RAMP_N; i++) {
            const log = LOG_MIN + (LOG_MAX - LOG_MIN) * (i + 0.5) / RAMP_N;
            const byte = Math.round(255 * code(key, e * Math.pow(2, log)));
            lab.set(ramp[i], { fill: `rgb(${byte},${byte},${byte})` });
        }
        lab.set(rampLabel, { text: `the same scene values, drawn through ${ToneMap.CURVES[key].label}` });
        lab.set(rampGray, { x1: gx, x2: gx });

        factorOut.set(`× ${e < 1 ? e.toFixed(3) : e.toFixed(e % 1 ? 2 : 0)}`);
        const byte = Math.round(255 * grayCode);
        grayOut.set(`${grayCode.toFixed(2)} · ${byte}`, Math.abs(byte - 128) <= 12 ? "ok" : "bad");

        const shift = stops.value === 0 ? "" : ` Exposure ${stops.value > 0 ? "+" : ""}${stops.value} moves every curve ${Math.abs(stops.value)} stops to the ${stops.value > 0 ? "left" : "right"}.`;
        if (key === "clip") note.set(`Clip: every scene value above ${(1 / e).toFixed(2)} is the same white, so lamps, sky and highlights lose all detail.${shift}`, "bad");
        else if (byte < 116) note.set(`Middle gray lands at byte ${byte}, darker than the middle of the screen range (128). Raise the exposure.${shift}`, "bad");
        else if (byte > 140) note.set(`Middle gray lands at byte ${byte}, brighter than the middle of the screen range (128), and more of the top turns white. Lower the exposure.${shift}`, "bad");
        else note.set(`Middle gray lands near byte 128, the middle of the screen range, and the bright values still roll off slowly into white.${shift}`, "ok");
    });
});

// A saturated light made brighter stop by stop, through each curve. Per-channel curves shift its hue.
RasmLab.register("hue-shift", function (lab) {
    const svg = lab.svg(760, 300, "Rows of color swatches: a saturated light from 1 to 256 times brighter, through clip, Reinhard, ACES fit and AgX. The chosen brightness is outlined in each row.");
    const COLORS = {
        red: [1, 0.05, 0.02],
        green: [0.05, 1, 0.03],
        blue: [0.03, 0.06, 1],
    };
    const colorPick = lab.choice({ label: "Light", value: "red", options: [
        { value: "red", label: "Red" }, { value: "green", label: "Green" }, { value: "blue", label: "Blue" },
    ] });
    const bright = lab.slider({ label: "Brightness", min: 0, max: 8, step: 1, value: 4, format: function (v) { return `+${v} stops (× ${Math.pow(2, v)})`; } });
    const srcOut = lab.readout("hue of the light");
    const acesOut = lab.readout("hue on screen: ACES fit");
    const agxOut = lab.readout("hue on screen: AgX");
    const note = lab.note();

    const ROWS = ["clip", "reinhard", "aces", "agx"];
    const SW = 52;
    const X0 = 110;
    const Y0 = 36;
    const ROW_H = 60;
    lab.el(svg, "text", { x: X0, y: 20, class: "small", text: "brightness in stops: ×1 … ×256" });
    lab.el(svg, "text", { x: 650, y: 20, class: "small", text: "chosen" });
    const cells = [];
    const rings = [];
    const big = [];
    ROWS.forEach(function (key, r) {
        const y = Y0 + r * ROW_H;
        lab.el(svg, "text", { x: X0 - 12, y: y + 30, class: "end title", text: ToneMap.CURVES[key].label });
        const row = [];
        for (let s = 0; s <= 8; s++) {
            row.push(lab.el(svg, "rect", { x: X0 + s * (SW + 4), y: y, width: SW, height: 44, rx: 4 }));
            if (r === ROWS.length - 1) lab.el(svg, "text", { x: X0 + s * (SW + 4) + SW / 2, y: y + 60, class: "mid small", text: `+${s}` });
        }
        cells.push(row);
        rings.push(lab.el(svg, "rect", { y: y - 3, width: SW + 6, height: 50, rx: 6, class: "hot" }));
        big.push(lab.el(svg, "rect", { x: 650, y: y, width: 90, height: 44, rx: 4 }));
    });

    function hue(rgb) {
        const max = Math.max(rgb[0], rgb[1], rgb[2]);
        const min = Math.min(rgb[0], rgb[1], rgb[2]);
        if (max <= 0 || (max - min) / max < 0.08) return null;
        const d = max - min;
        let h;
        if (max === rgb[0]) h = ((rgb[1] - rgb[2]) / d) % 6;
        else if (max === rgb[1]) h = (rgb[2] - rgb[0]) / d + 2;
        else h = (rgb[0] - rgb[1]) / d + 4;
        h *= 60;
        return h < 0 ? h + 360 : h;
    }
    function hueName(h) {
        if (h === null) return "white";
        const names = [[15, "red"], [45, "orange"], [70, "yellow"], [160, "green"], [200, "cyan"], [255, "blue"], [290, "purple"], [345, "pink"], [361, "red"]];
        for (let k = 0; k < names.length; k++) if (h < names[k][0]) return names[k][1];
        return "red";
    }
    function show(h) {
        return h === null ? "white" : `${h.toFixed(0)}° ${hueName(h)}`;
    }
    function rgbFill(linear) {
        const c = linear.map(function (l) { return Math.round(255 * ToneMap.encode(Math.max(0, Math.min(1, l)))); });
        return `rgb(${c[0]},${c[1]},${c[2]})`;
    }

    lab.draw(function () {
        const base = COLORS[colorPick.value];
        let acesHue = null;
        let agxHue = null;
        ROWS.forEach(function (key, r) {
            for (let s = 0; s <= 8; s++) {
                const out = ToneMap.CURVES[key].map(base.map(function (c) { return c * Math.pow(2, s); }));
                lab.set(cells[r][s], { fill: rgbFill(out) });
            }
            const out = ToneMap.CURVES[key].map(base.map(function (c) { return c * Math.pow(2, bright.value); }));
            lab.set(big[r], { fill: rgbFill(out) });
            lab.set(rings[r], { x: X0 + bright.value * (SW + 4) - 3 });
            if (key === "aces") acesHue = hue(out);
            if (key === "agx") agxHue = hue(out);
        });
        const srcHue = hue(base);
        srcOut.set(show(srcHue));
        const far = function (h) { return h === null ? 0 : Math.min(Math.abs(h - srcHue), 360 - Math.abs(h - srcHue)); };
        const acesMoved = acesHue !== null && far(acesHue) > 15;
        const agxMoved = agxHue !== null && far(agxHue) > 15;
        const times = `At × ${Math.pow(2, bright.value)}`;
        acesOut.set(show(acesHue), acesMoved ? "bad" : "ok");
        agxOut.set(show(agxHue), agxMoved ? "bad" : "ok");
        if (acesMoved && !agxMoved)
            note.set(`${times}, the per-channel ACES fit turns the ${colorPick.value} light ${hueName(acesHue)}: its small channels catch up while the big one is stuck at 1. AgX shows it ${agxHue === null ? "white" : `still ${hueName(agxHue)}, paler`}.`, "bad");
        else if (acesMoved)
            note.set(`${times}, both curves move the hue: ACES fit to ${hueName(acesHue)}, AgX to ${hueName(agxHue)}. AgX moves less and loses saturation instead.`, "bad");
        else if (acesHue === null && agxHue === null)
            note.set(`${times}, both curves show the light as white: it is far above the screen's range, and white is the right answer for it.`, "ok");
        else
            note.set(`${times}, the light still shows its own hue through every curve. Make it brighter.`, "ok");
    });
});

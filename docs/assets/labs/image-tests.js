// Labs of docs/guides/image-tests.html.

// A 48 x 27 image (a ball, its shadow, sky and ground), a stored golden of it and a new render. The new render has the
// small noise a second run of the same code shows, and optionally a real bug. The test is the page's rule: a pixel
// differs when one of its channels is off by more than T; the test fails when more than F of the pixels differ.
RasmLab.register("image-threshold", function (lab) {
    const W = 48;
    const H = 27;
    const CELL = 5;
    const GAP = 20;
    const TOP = 20;
    const ctx = lab.canvas(3 * W * CELL + 2 * GAP, TOP + H * CELL, "Three small images side by side: the golden, the new render and their difference. In the difference, gray pixels are the same, orange ones differ by no more than T, red ones by more than T.");
    const change = lab.choice({
        label: "the new render",
        options: [
            { value: "noise", label: "same code, another run" },
            { value: "dim", label: "bug: light 3% dimmer" },
            { value: "shadow", label: "bug: shadow 1 px off" },
        ],
        value: "noise",
    });
    const threshold = lab.slider({ label: "T, per channel", min: 0, max: 16, value: 3, format: function (v) { return `${v} / 255`; } });
    const fraction = lab.slider({ label: "F, share of pixels", min: 0, max: 5, step: 0.1, value: 0.1, format: function (v) { return `${v.toFixed(1)}%`; } });
    const overOut = lab.readout("pixels over T");
    const maxOut = lab.readout("largest difference");
    const verdictOut = lab.readout("test");
    const note = lab.note();

    // A small seeded random, so the noise is the same on every visit.
    function random(seed) {
        let s = seed >>> 0;
        return function () {
            s = (s * 1664525 + 1013904223) >>> 0;
            return s / 4294967296;
        };
    }

    // The scene, in 0..255 sRGB numbers. shadowShift moves the shadow, light scales the ball's sunlight.
    function render(shadowShift, light) {
        const pixels = [];
        for (let y = 0; y < H; y++) {
            for (let x = 0; x < W; x++) {
                let c;
                if (y < 15) {
                    const t = y / 15;
                    c = [150 + 60 * t, 185 + 40 * t, 235 - 10 * t];
                } else {
                    const shadow = Math.pow((x + 0.5 - 30 - shadowShift) / 9, 2) + Math.pow((y + 0.5 - 22) / 2.4, 2) < 1;
                    c = shadow ? [92, 90, 86] : [170, 166, 158];
                }
                const dx = (x + 0.5 - 22) / 8;
                const dy = (y + 0.5 - 13) / 8;
                if (dx * dx + dy * dy < 1) {
                    const nz = Math.sqrt(1 - dx * dx - dy * dy);
                    const lambert = Math.max(0, -0.45 * dx - 0.55 * dy + 0.7 * nz);
                    const shade = 0.18 + 0.82 * lambert * light;
                    c = [235 * shade, 90 * shade, 60 * shade];
                }
                pixels.push(c.map(function (v) { return Math.round(Math.min(255, Math.max(0, v))); }));
            }
        }
        return pixels;
    }

    // A second run of the same code: a few pixels on edges are off by 1 or 2, one by 6 (a tie in the rasterizer).
    function addNoise(pixels, golden) {
        const next = random(7);
        return pixels.map(function (c, i) {
            const x = i % W;
            const y = Math.floor(i / W);
            const edge = [[1, 0], [-1, 0], [0, 1], [0, -1]].some(function (d) {
                const nx = x + d[0];
                const ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) return false;
                const o = golden[ny * W + nx];
                return Math.abs(o[0] - golden[i][0]) + Math.abs(o[1] - golden[i][1]) + Math.abs(o[2] - golden[i][2]) > 30;
            });
            const r = next();
            let delta = 0;
            if (edge && r < 0.5) delta = r < 0.15 ? 2 : 1;
            if (!edge && r < 0.03) delta = 1;
            if (x === 15 && y === 9) delta = 6;
            const sign = next() < 0.5 ? -1 : 1;
            return c.map(function (v) { return Math.min(255, Math.max(0, v + sign * delta)); });
        });
    }

    const golden = render(0, 1);
    const renders = {
        noise: addNoise(render(0, 1), golden),
        dim: addNoise(render(0, 0.97), golden),
        shadow: addNoise(render(1, 1), golden),
    };

    function panel(index, title, pixels) {
        const x0 = index * (W * CELL + GAP);
        ctx.fillStyle = lab.css("--fg-soft");
        ctx.fillText(title, x0, 13);
        pixels.forEach(function (c, i) {
            ctx.fillStyle = `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
            ctx.fillRect(x0 + (i % W) * CELL, TOP + Math.floor(i / W) * CELL, CELL, CELL);
        });
    }

    lab.draw(function () {
        const actual = renders[change.value];
        const t = threshold.value;
        let over = 0;
        let largest = 0;
        const diff = actual.map(function (c, i) {
            const g = golden[i];
            const d = Math.max(Math.abs(c[0] - g[0]), Math.abs(c[1] - g[1]), Math.abs(c[2] - g[2]));
            largest = Math.max(largest, d);
            if (d > t) {
                over++;
                return [220, 38, 38];
            }
            if (d > 0) return [245, 158, 11];
            const gray = Math.round((g[0] + g[1] + g[2]) / 9);
            return [gray, gray, gray];
        });
        const share = 100 * over / (W * H);
        const pass = share <= fraction.value;

        ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
        ctx.font = "12px system-ui, sans-serif";
        panel(0, "golden (stored)", golden);
        panel(1, "new render", actual);
        panel(2, "difference", diff);

        overOut.set(`${over} of ${W * H} = ${share.toFixed(2)}%`, over > 0 ? "bad" : "ok");
        maxOut.set(`${largest} / 255`);
        verdictOut.set(pass ? "pass" : "fail", pass ? "ok" : "bad");
        const bug = change.value !== "noise";
        if (!bug && pass) note.set("Right: the same code passes. The noise is either under T or too rare to reach F.", "ok");
        else if (!bug) note.set("Wrong: the test fails on noise alone. Raise T above the noise, or F above the share of rare outliers.", "bad");
        else if (!pass) note.set("Right: the bug fails the test. Look at the red pixels in the difference: they show where it is.", "ok");
        else if (change.value === "dim") note.set("Wrong: T is so high that a 3% darker light passes. Keep T near the noise.", "bad");
        else note.set("Wrong: F is so high that a whole row of wrong pixels passes. Keep F near the noise.", "bad");
    });
});

// Labs of docs/guides/temporal-history.html.

// Two copies of a small image. Each frame reads one and draws the other: new = max(ball, old × 0.85).
// With the swap off, every frame draws A and reads B, so nothing ever fills B and the trail is lost.
RasmLab.register("ping-pong", function (lab) {
    const W = 100;
    const H = 50;
    const ZOOM = 3;
    const K = 0.85;
    const ctx = lab.canvas(720, 200, "Two copies of an image with a moving ball and its fading trail. Each frame, one copy is read and the other is drawn.");
    const small = document.createElement("canvas");
    small.width = W;
    small.height = H;
    const smallCtx = small.getContext("2d");
    const image = smallCtx.createImageData(W, H);

    const copies = [new Float32Array(W * H), new Float32Array(W * H)];
    let frame = -1;
    let drawIndex = 0;
    let readIndex = 1;

    function step() {
        frame += 1;
        drawIndex = swap.value ? frame % 2 : 0;
        readIndex = swap.value ? (frame + 1) % 2 : 1;
        const cx = 50 + 34 * Math.cos(frame * 0.14);
        const cy = 25 + 15 * Math.sin(frame * 0.14);
        const from = copies[readIndex];
        const to = copies[drawIndex];
        for (let y = 0; y < H; y++) {
            for (let x = 0; x < W; x++) {
                const current = Math.hypot(x + 0.5 - cx, y + 0.5 - cy) < 6 ? 1 : 0;
                to[y * W + x] = Math.max(current, from[y * W + x] * K);
            }
        }
    }

    lab.button("Next frame", step);
    const swap = lab.toggle({ label: "Swap the copies every frame", value: true });
    let wait = 0;
    lab.animate(function (dt) {
        wait += dt;
        while (wait >= 0.25) {
            wait -= 0.25;
            step();
        }
    });
    const frameOut = lab.readout("frame");
    const drawOut = lab.readout("draws copy");
    const readOut = lab.readout("reads copy");
    const note = lab.note();
    step();

    lab.draw(function () {
        ctx.clearRect(0, 0, 720, 200);
        ctx.font = "13px system-ui, sans-serif";
        ctx.imageSmoothingEnabled = false;
        ctx.lineWidth = 3;
        copies.forEach(function (copy, k) {
            for (let p = 0; p < W * H; p++) {
                image.data[p * 4] = 255 * copy[p];
                image.data[p * 4 + 1] = 200 * copy[p];
                image.data[p * 4 + 2] = 110 * copy[p];
                image.data[p * 4 + 3] = 255;
            }
            smallCtx.putImageData(image, 0, 0);
            const x = k === 0 ? 30 : 390;
            ctx.drawImage(small, x, 36, W * ZOOM, H * ZOOM);
            const role = k === drawIndex ? "draws" : (k === readIndex ? "reads" : "");
            const color = lab.css(k === drawIndex ? "--gpu" : "--mem");
            ctx.strokeStyle = color;
            ctx.strokeRect(x - 1.5, 34.5, W * ZOOM + 3, H * ZOOM + 3);
            ctx.fillStyle = color;
            ctx.fillText(`copy ${"AB"[k]}: frame ${frame} ${role} it`, x, 22);
        });
        // the pass: from the copy it reads to the copy it draws
        const dir = drawIndex === 1 ? 1 : -1;
        ctx.strokeStyle = lab.css("--fg-soft");
        ctx.fillStyle = lab.css("--fg-soft");
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(360 - dir * 16, 111);
        ctx.lineTo(360 + dir * 12, 111);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(360 + dir * 20, 111);
        ctx.lineTo(360 + dir * 10, 105);
        ctx.lineTo(360 + dir * 10, 117);
        ctx.fill();
        ctx.textAlign = "center";
        ctx.fillText("pass", 360, 96);
        ctx.textAlign = "left";

        frameOut.set(String(frame));
        drawOut.set("AB"[drawIndex], "gpu");
        readOut.set("AB"[readIndex], "mem");
        if (!swap.value) note.set("No swap: every frame draws A and reads B. No frame draws B, so the history is always empty and the trail is gone.", "bad");
        else if (frame === 0) note.set("Frame 0 draws copy A and reads copy B. No frame drew B yet: it is only cleared.");
        else note.set(`Frame ${frame} reads copy ${"AB"[readIndex]}, which frame ${frame - 1} drew, and draws copy ${"AB"[drawIndex]}. The trail goes from one copy to the other.`, "ok");
    });
});

// The two formulas of the section on the same moving ball, with noise in the input:
// a trail max(c, h × k) and a running average mix(h, c, α), with k = 1 − α.
RasmLab.register("history-blend", function (lab) {
    const W = 100;
    const H = 60;
    const PW = 224;
    const PH = 134;
    const NOISE_ROWS = 14;      // the ball never touches these rows; the noise is measured there
    const ctx = lab.canvas(720, 170, "Three images of a ball that moves over a noisy background: the input of this frame, a decaying trail, and a running average.");
    const alpha = lab.slider({ label: "α (and k = 1 − α)", min: 0.02, max: 0.9, step: 0.01, value: 0.15, format: function (v) { return `${v.toFixed(2)} (k = ${(1 - v).toFixed(2)})`; } });
    const noisy = lab.toggle({ label: "Noise in the input", value: true });
    const fadeOut = lab.readout("frames until the trail falls to 10 %");
    const followOut = lab.readout("how far the average follows the ball in 10 frames");
    const noiseOut = lab.readout("input noise left in the average");
    const note = lab.note();

    const small = document.createElement("canvas");
    small.width = W;
    small.height = H;
    const smallCtx = small.getContext("2d");
    const image = smallCtx.createImageData(W, H);

    let seed = 11;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }

    const input = new Float32Array(W * H);
    const trail = new Float32Array(W * H);
    const average = new Float32Array(W * H);
    let frame = 0;

    function step() {
        const a = alpha.value;
        const k = 1 - a;
        // the ball is 20 pixels wide and moves 2 pixels per frame: it covers a pixel for 10 frames
        const cx = (frame * 2) % 140 - 20;
        frame += 1;
        for (let y = 0; y < H; y++) {
            for (let x = 0; x < W; x++) {
                const p = y * W + x;
                const ball = Math.hypot(x + 0.5 - cx, y + 0.5 - 37) < 10;
                const c = ball ? 1 : 0.25 + (noisy.value ? (random() - 0.5) * 0.4 : 0);
                input[p] = c;
                trail[p] = Math.max(c, trail[p] * k);
                average[p] = average[p] + a * (c - average[p]);
            }
        }
    }

    function deviation(values) {
        let sum = 0;
        let squares = 0;
        const n = NOISE_ROWS * W;
        for (let p = 0; p < n; p++) {
            sum += values[p];
            squares += values[p] * values[p];
        }
        return Math.sqrt(Math.max(0, squares / n - (sum / n) * (sum / n)));
    }

    let wait = 0;
    lab.animate(function (dt) {
        wait += dt;
        while (wait >= 1 / 30) {
            wait -= 1 / 30;
            step();
        }
    });
    for (let k = 0; k < 60; k++) step();

    lab.draw(function () {
        ctx.clearRect(0, 0, 720, 170);
        ctx.font = "13px system-ui, sans-serif";
        ctx.imageSmoothingEnabled = false;
        ctx.fillStyle = lab.css("--fg-soft");
        const panels = [
            { values: input, title: "input c: this frame only" },
            { values: trail, title: "trail: max(c, h · k)" },
            { values: average, title: "average: mix(h, c, α)" },
        ];
        panels.forEach(function (panel, k) {
            for (let p = 0; p < W * H; p++) {
                const v = 255 * panel.values[p];
                image.data[p * 4] = v;
                image.data[p * 4 + 1] = v;
                image.data[p * 4 + 2] = v;
                image.data[p * 4 + 3] = 255;
            }
            smallCtx.putImageData(image, 0, 0);
            const x = 12 + k * (PW + 12);
            ctx.drawImage(small, x, 28, PW, PH);
            ctx.fillText(panel.title, x, 18);
        });

        const a = alpha.value;
        const follow = 1 - Math.pow(1 - a, 10);
        fadeOut.set((Math.log(0.1) / Math.log(1 - a)).toFixed(1));
        followOut.set(`${(100 * follow).toFixed(0)} %`, follow < 0.5 ? "bad" : "ok");
        const inputNoise = deviation(input);
        const left = inputNoise > 1e-6 ? deviation(average) / inputNoise : 0;
        if (inputNoise > 1e-6) noiseOut.set(`${(100 * left).toFixed(0)} %`, left > 0.5 ? "bad" : "ok");
        else noiseOut.set("no noise");
        if (follow < 0.5) note.set("α is small: the average is smooth, but it lags. The ball is a dim ghost with a long tail.", "bad");
        else if (a >= 0.5) note.set("α is large: the average follows the ball at once, but it keeps most of the noise. The trail is short.", "bad");
        else note.set("A balance: the average removes most of the noise and still follows the ball. The trail keeps the ball at full brightness and only fades behind it.", "ok");
    });
});

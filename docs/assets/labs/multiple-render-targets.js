// Labs of docs/guides/multiple-render-targets.html.

// A small scene (three balls on a ground) is found once per pixel with a ray. That one hit writes every
// attachment: albedo, normal, material, and the shared depth. A probe pixel shows the values of one position.
RasmLab.register("mrt-outputs", function (lab) {
    const W = 168;
    const H = 120;
    const GAP = 8;
    const TOP = 22;
    const PROBE_Y = 60;
    const ctx = lab.canvas(4 * W + 5 * GAP, TOP + H + 10, "Four images of the same scene, written by one pass: albedo, normal, material and depth. A small square marks the same pixel in each.");
    const ballX = lab.slider({ label: "Move the front ball", min: -2.4, max: 2.4, step: 0.05, value: 0.2, format: function (v) { return v.toFixed(2); } });
    const probeX = lab.slider({ label: "Probe pixel x", min: 0, max: W - 1, value: 55 });
    const albedoOut = lab.readout("output 0 at the probe: albedo (r, g, b)");
    const normalOut = lab.readout("output 1 at the probe: normal (x, y, z)");
    const materialOut = lab.readout("output 2 at the probe: metallic, roughness");
    const note = lab.note();

    const EYE = [0, 1.3, 6];
    const BALLS = [
        { name: "red ball", c: [-1.3, 0.8, 0], r: 0.8, albedo: [0.8, 0.2, 0.15], metallic: 0, roughness: 0.6 },
        { name: "blue metal ball", c: [1.1, 1.0, -0.8], r: 1.0, albedo: [0.2, 0.4, 0.85], metallic: 1, roughness: 0.3 },
        { name: "front ball", c: [0, 0.5, 1.6], r: 0.5, albedo: [0.3, 0.75, 0.3], metallic: 0, roughness: 0.2 },
    ];
    const TITLES = ["attachment 0 · albedo", "attachment 1 · normal", "attachment 2 · material", "depth · shared, near is bright"];
    const images = TITLES.map(function () { return ctx.createImageData(W, H); });

    // Every surface the ray of pixel (i, j) crosses, nearest first. The first one passes the depth test.
    function hits(i, j) {
        const u = (i + 0.5) / W * 2 - 1;
        const v = 1 - (j + 0.5) / H * 2;
        let d = [u * 0.63, v * 0.45 - 0.12, -1];
        const len = Math.hypot(d[0], d[1], d[2]);
        d = [d[0] / len, d[1] / len, d[2] / len];
        const found = [];
        BALLS.forEach(function (ball) {
            const o = [EYE[0] - ball.c[0], EYE[1] - ball.c[1], EYE[2] - ball.c[2]];
            const b = o[0] * d[0] + o[1] * d[1] + o[2] * d[2];
            const disc = b * b - (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - ball.r * ball.r);
            if (disc < 0) return;
            const t = -b - Math.sqrt(disc);
            if (t <= 0) return;
            const p = [EYE[0] + d[0] * t, EYE[1] + d[1] * t, EYE[2] + d[2] * t];
            found.push({
                t: t, name: ball.name, albedo: ball.albedo, metallic: ball.metallic, roughness: ball.roughness,
                normal: [(p[0] - ball.c[0]) / ball.r, (p[1] - ball.c[1]) / ball.r, (p[2] - ball.c[2]) / ball.r],
            });
        });
        if (d[1] < 0) {
            const t = -EYE[1] / d[1];
            if (t < 30) {
                const x = EYE[0] + d[0] * t;
                const z = EYE[2] + d[2] * t;
                const dark = (Math.floor(x) + Math.floor(z)) % 2 === 0;
                found.push({ t: t, name: "ground", albedo: dark ? [0.45, 0.42, 0.38] : [0.7, 0.67, 0.6], metallic: 0, roughness: 0.9, normal: [0, 1, 0] });
            }
        }
        found.sort(function (a, b) { return a.t - b.t; });
        return found;
    }

    function put(image, k, r, g, b) {
        image.data[k] = r * 255;
        image.data[k + 1] = g * 255;
        image.data[k + 2] = b * 255;
        image.data[k + 3] = 255;
    }

    function triple(a, b, c) {
        return `${a.toFixed(2)}, ${b.toFixed(2)}, ${c.toFixed(2)}`;
    }

    lab.draw(function () {
        BALLS[2].c[0] = ballX.value;
        for (let j = 0; j < H; j++) {
            for (let i = 0; i < W; i++) {
                const k = (j * W + i) * 4;
                const hit = hits(i, j)[0];
                if (!hit) {
                    // nothing drawn here: every attachment keeps its clear value
                    for (let a = 0; a < 4; a++) put(images[a], k, 0, 0, 0);
                    continue;
                }
                // one surface, found once, written to every attachment
                put(images[0], k, hit.albedo[0], hit.albedo[1], hit.albedo[2]);
                put(images[1], k, hit.normal[0] * 0.5 + 0.5, hit.normal[1] * 0.5 + 0.5, hit.normal[2] * 0.5 + 0.5);
                put(images[2], k, hit.metallic, hit.roughness, 0);
                const near = lab.clamp(1 - (hit.t - 3) / 22, 0, 1);
                put(images[3], k, near, near, near);
            }
        }

        ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
        ctx.font = "12px system-ui, sans-serif";
        ctx.textBaseline = "alphabetic";
        const px = probeX.value;
        images.forEach(function (image, a) {
            const x = GAP + a * (W + GAP);
            ctx.putImageData(image, x, TOP);
            ctx.fillStyle = lab.css("--fg-soft");
            ctx.fillText(TITLES[a], x, 14);
            ctx.strokeStyle = lab.css("--line");
            ctx.lineWidth = 1;
            ctx.strokeRect(x - 0.5, TOP - 0.5, W + 1, H + 1);
            ctx.strokeStyle = lab.css("--accent");
            ctx.lineWidth = 2;
            ctx.strokeRect(x + px - 4, TOP + PROBE_Y - 4, 9, 9);
        });

        const crossed = hits(px, PROBE_Y);
        const hit = crossed[0];
        if (!hit) {
            albedoOut.set("cleared");
            normalOut.set("cleared");
            materialOut.set("cleared");
            note.set("The probe pixel is on the sky. No triangle covers it, so no output is written: every attachment keeps its clear value.");
            return;
        }
        albedoOut.set(triple(hit.albedo[0], hit.albedo[1], hit.albedo[2]));
        normalOut.set(triple(hit.normal[0], hit.normal[1], hit.normal[2]));
        materialOut.set(`${hit.metallic.toFixed(2)}, ${hit.roughness.toFixed(2)}`);
        if (crossed.length > 1) note.set(`The probe pixel is on the ${hit.name}. ${crossed.length} surfaces cover this pixel. The depth test keeps the nearest one, once, and all outputs come from it.`, "ok");
        else note.set(`The probe pixel is on the ${hit.name}. The surface is found once, and each output takes its value from it.`, "ok");
    });
});

// The attachments of a G-buffer as a bar of bytes per pixel. Three of them can change their format.
RasmLab.register("gbuffer-bytes", function (lab) {
    const svg = lab.svg(760, 150, "A bar of bytes per pixel, one box per attachment of a G-buffer, and three boxes to click that change the format of the position, the normal and the velocity.");
    const resolution = lab.choice({
        label: "Screen",
        options: [{ value: 1920 * 1080, label: "1920 × 1080" }, { value: 2560 * 1440, label: "2560 × 1440" }, { value: 3840 * 2160, label: "3840 × 2160" }],
    });
    const bytesOut = lab.readout("bytes per pixel");
    const memoryOut = lab.readout("memory");
    const trafficOut = lab.readout("written and read, at 60 frames per second");
    const note = lab.note();

    const PX_PER_BYTE = 15;
    const parts = [
        { name: "position", options: [{ format: "RGBA32F", bytes: 16 }, { format: "rebuilt from depth", bytes: 0 }], pick: 0 },
        { name: "normal", options: [{ format: "RGBA16F", bytes: 8 }, { format: "RG16, octahedral", bytes: 4 }], pick: 0 },
        { name: "albedo", options: [{ format: "RGBA8", bytes: 4 }], pick: 0 },
        { name: "material", options: [{ format: "RGBA8", bytes: 4 }], pick: 0 },
        { name: "velocity", options: [{ format: "not written", bytes: 0 }, { format: "RGBA16F", bytes: 8 }, { format: "RG16F", bytes: 4 }], pick: 0 },
        { name: "depth", options: [{ format: "32-bit", bytes: 4 }], pick: 0 },
    ];

    lab.el(svg, "text", { x: 20, y: 18, class: "small", text: "bytes per pixel, one box = one attachment (15 px per byte)" });
    const bar = lab.el(svg, "g");
    const total = lab.el(svg, "text", { y: 59, class: "title" });
    lab.el(svg, "text", { x: 20, y: 98, class: "small", text: "click a box to change its format" });
    let chipX = 20;
    parts.forEach(function (part) {
        if (part.options.length < 2) return;
        const g = lab.el(svg, "g");
        part.rect = lab.el(g, "rect", { x: chipX, y: 106, width: 230, height: 32, rx: 6 });
        part.text = lab.el(g, "text", { x: chipX + 115, y: 127, class: "mid small" });
        lab.clickable(g, `Change the format of the ${part.name}`, function () { part.pick = (part.pick + 1) % part.options.length; });
        chipX += 245;
    });

    lab.draw(function () {
        lab.clear(bar);
        let x = 20;
        let bytes = 0;
        parts.forEach(function (part) {
            const option = part.options[part.pick];
            if (part.rect) {
                lab.set(part.rect, { class: option.bytes ? "mem" : "box" });
                lab.set(part.text, { text: `${part.name}: ${option.format}, ${option.bytes} B` });
            }
            if (!option.bytes) return;
            const width = option.bytes * PX_PER_BYTE;
            lab.el(bar, "rect", { x: x, y: 38, width: width, height: 32, class: part.name === "depth" ? "box" : "mem" });
            const label = width >= 200 ? `${part.name} · ${option.format} · ${option.bytes}` : (width >= 120 ? `${part.name} · ${option.bytes} B` : part.name);
            lab.el(bar, "text", { x: x + width / 2, y: 58, class: "mid small", text: label });
            x += width;
            bytes += option.bytes;
        });
        lab.set(total, { x: x + 10, text: `${bytes} B`, class: bytes <= 20 ? "title t-ok" : (bytes >= 36 ? "title t-bad" : "title") });

        const pixels = resolution.value;
        const cls = bytes <= 20 ? "ok" : (bytes >= 36 ? "bad" : "");
        bytesOut.set(`${bytes} B`, cls);
        memoryOut.set(`${(bytes * pixels / 1048576).toFixed(1)} MiB`, cls);
        trafficOut.set(`${(bytes * pixels * 2 * 60 / 1e9).toFixed(1)} GB/s`, cls);
        const velocity = parts[4].options[parts[4].pick].bytes > 0;
        if (bytes <= 20 && velocity) note.set("The packed layout: 20 bytes per pixel, and it has a velocity that the fat layout did not have.", "ok");
        else if (bytes <= 20) note.set("Packed, but with no velocity yet. Adding it as RG16F costs 4 bytes per pixel.", "ok");
        else if (bytes >= 36) note.set("A fat layout: the position and the wide normal take most of the bytes. Click them to pack them.", "bad");
        else note.set("Between the two layouts. Each byte removed here is saved at every pixel, once to write and once to read.");
    });
});

// Labs of docs/guides/normal-mapping.html.

// A number with a real minus sign, and no "−0.00".
function normalMappingNum(v, digits) {
    const s = Math.abs(v).toFixed(digits);
    return (v < 0 && Number(s) !== 0 ? "−" : "") + s;
}

// stored = (n + 1) / 2, as a byte; and back, as a shader reads it.
function normalMappingEncode(n) { return Math.round((n + 1) / 2 * 255); }
function normalMappingDecode(byte) { return byte / 255 * 2 - 1; }

// A flat square lit per pixel. Each pixel reads its normal from an 8-bit normal map of round bumps.
RasmLab.register("lit-normal-map", function (lab) {
    const SIZE = 250;
    const MAP = { x: 24, y: 24 };
    const LIT = { x: 326, y: 24 };
    const ctx = lab.canvas(600, 322, "Left: a normal map of round bumps, shown as its stored colors. Right: a flat square lit per pixel with the normals of that map, and a sun that shows where the light comes from.");
    const from = lab.slider({ label: "Light from", min: 0, max: 360, step: 5, value: 135, format: function (v) { return `${v}°`; } });
    const useMap = lab.toggle({ label: "Use the normal map", value: true });
    const wrongGreen = lab.toggle({ label: "Read green with the other convention", value: false });
    const flatOut = lab.readout("n · l on the flat surface");
    const darkOut = lab.readout("n · l, darkest pixel");
    const brightOut = lab.readout("n · l, brightest pixel");
    const note = lab.note();

    // Build the map once: a height field of bumps, its normals, stored as bytes like a texture.
    // The image's up is +y (the "OpenGL style" of the page).
    const CELLS = 5;
    const RADIUS = 0.4;
    const HEIGHT = 0.14;
    const bytes = new Uint8Array(SIZE * SIZE * 3);
    const mapImage = ctx.createImageData(SIZE, SIZE);
    for (let row = 0; row < SIZE; row++) {
        for (let col = 0; col < SIZE; col++) {
            const u = ((col + 0.5) / SIZE * CELLS) % 1 - 0.5;
            const v = ((SIZE - row - 0.5) / SIZE * CELLS) % 1 - 0.5;
            const r = Math.hypot(u, v) / RADIUS;
            // h = HEIGHT · (1 − r²)² inside the bump; the normal is (−dh/du, −dh/dv, 1), normalized
            const slope = r < 1 ? HEIGHT * 4 * (1 - r * r) / (RADIUS * RADIUS) : 0;
            const nx = slope * u;
            const ny = slope * v;
            const length = Math.hypot(nx, ny, 1);
            const k = (row * SIZE + col) * 3;
            bytes[k] = normalMappingEncode(nx / length);
            bytes[k + 1] = normalMappingEncode(ny / length);
            bytes[k + 2] = normalMappingEncode(1 / length);
            const p = (row * SIZE + col) * 4;
            mapImage.data[p] = bytes[k];
            mapImage.data[p + 1] = bytes[k + 1];
            mapImage.data[p + 2] = bytes[k + 2];
            mapImage.data[p + 3] = 255;
        }
    }
    const litImage = ctx.createImageData(SIZE, SIZE);
    const ELEVATION = 45 * Math.PI / 180;

    lab.draw(function () {
        const a = from.value * Math.PI / 180;
        const l = [Math.cos(a) * Math.cos(ELEVATION), Math.sin(a) * Math.cos(ELEVATION), Math.sin(ELEVATION)];
        const flip = wrongGreen.value ? -1 : 1;
        let darkest = 1;
        let brightest = 0;
        for (let i = 0; i < SIZE * SIZE; i++) {
            let nx = 0;
            let ny = 0;
            let nz = 1;
            if (useMap.value) {
                nx = normalMappingDecode(bytes[i * 3]);
                ny = normalMappingDecode(bytes[i * 3 + 1]) * flip;
                nz = normalMappingDecode(bytes[i * 3 + 2]);
                const length = Math.hypot(nx, ny, nz);
                nx /= length;
                ny /= length;
                nz /= length;
            }
            const nl = Math.max(0, nx * l[0] + ny * l[1] + nz * l[2]);
            if (nl < darkest) darkest = nl;
            if (nl > brightest) brightest = nl;
            // linear light to a display value, on a clay-colored surface
            const shade = Math.pow(nl, 1 / 2.2);
            litImage.data[i * 4] = 255 * 0.92 * shade;
            litImage.data[i * 4 + 1] = 255 * 0.8 * shade;
            litImage.data[i * 4 + 2] = 255 * 0.7 * shade;
            litImage.data[i * 4 + 3] = 255;
        }

        ctx.clearRect(0, 0, 600, 322);
        ctx.putImageData(mapImage, MAP.x, MAP.y);
        if (!useMap.value) {
            // putImageData ignores alpha, so fade the unused map with the background color
            ctx.fillStyle = lab.css("--bg-raised");
            ctx.globalAlpha = 0.7;
            ctx.fillRect(MAP.x, MAP.y, SIZE, SIZE);
        }
        ctx.globalAlpha = 1;
        ctx.putImageData(litImage, LIT.x, LIT.y);
        ctx.strokeStyle = lab.css("--line");
        ctx.strokeRect(MAP.x + 0.5, MAP.y + 0.5, SIZE - 1, SIZE - 1);
        ctx.strokeRect(LIT.x + 0.5, LIT.y + 0.5, SIZE - 1, SIZE - 1);

        // the sun, on a square path around the lit picture
        const reach = (SIZE / 2 + 13) / Math.max(Math.abs(Math.cos(a)), Math.abs(Math.sin(a)));
        ctx.beginPath();
        ctx.arc(LIT.x + SIZE / 2 + Math.cos(a) * reach, LIT.y + SIZE / 2 - Math.sin(a) * reach, 8, 0, 2 * Math.PI);
        ctx.fillStyle = lab.css("--light");
        ctx.fill();

        ctx.fillStyle = lab.css("--fg-soft");
        ctx.font = `13px ${lab.css("--sans") || "sans-serif"}`;
        ctx.textAlign = "center";
        ctx.fillText(useMap.value ? "the normal map: its stored colors" : "the normal map: not used", MAP.x + SIZE / 2, MAP.y + SIZE + 34);
        ctx.fillText("the flat square, lit per pixel", LIT.x + SIZE / 2, LIT.y + SIZE + 34);

        flatOut.set(l[2].toFixed(2));
        darkOut.set(darkest.toFixed(2));
        brightOut.set(brightest.toFixed(2));
        if (!useMap.value) note.set("No map: every pixel has the normal (0, 0, 1), so every pixel gets the same brightness. The square looks flat.");
        else if (wrongGreen.value) note.set("Green is read with the other convention, so every normal is flipped in y. The bright sides no longer face the sun: the bumps look wrong, like dents.", "bad");
        else note.set("Each pixel reads its own normal from the map. Slopes that face the sun are bright, slopes that face away are dark: two flat triangles look like bumps.", "ok");
    });
});

// A side view: one texel's normal in tangent space, and the same normal in world space on a surface that tilts.
RasmLab.register("tbn-frame", function (lab) {
    const svg = lab.svg(760, 320, "Left: tangent space, with T to the right and N up, and the normal that one texel stores. Right: a surface seen from the side, tilted in the world, with its own T and N; the world-space normal is x steps along T plus z steps along N.");
    const texel = lab.slider({ label: "Texel x", min: -0.8, max: 0.8, step: 0.02, value: 0.5, format: function (v) { return normalMappingNum(v, 2); } });
    const tilt = lab.slider({ label: "Surface tilt", min: -60, max: 60, step: 1, value: 0, format: function (v) { return `${normalMappingNum(v, 0)}°`; } });
    const bytesOut = lab.readout("the texel's bytes (R, G, B)");
    const tangentOut = lab.readout("normal in tangent space (x, y, z)");
    const worldOut = lab.readout("normal in world space (X, Y), side view");
    const note = lab.note();

    const UNIT = 100;
    const LOCAL = { x: 130, y: 250 };
    const WORLD = { x: 530, y: 190 };

    // tangent space
    lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "Tangent space" });
    const swatch = lab.el(svg, "rect", { x: 20, y: 40, width: 36, height: 36, rx: 4 });
    const swatchText = lab.el(svg, "text", { x: 66, y: 55, class: "mono small" });
    const decodedText = lab.el(svg, "text", { x: 66, y: 72, class: "small" });
    lab.el(svg, "line", { x1: LOCAL.x - 95, y1: LOCAL.y, x2: LOCAL.x + 110, y2: LOCAL.y, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    lab.el(svg, "text", { x: LOCAL.x + 118, y: LOCAL.y + 5, class: "title t-hot", text: "T" });
    lab.el(svg, "line", { x1: LOCAL.x, y1: LOCAL.y, x2: LOCAL.x, y2: LOCAL.y - 120, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: LOCAL.x - 8, y: LOCAL.y - 116, class: "end title", text: "N" });
    lab.el(svg, "path", { d: `M${LOCAL.x - 90},${LOCAL.y} A90,90 0 0 1 ${LOCAL.x + 90},${LOCAL.y}`, class: "dashed" });
    const localN = lab.el(svg, "line", { x1: LOCAL.x, y1: LOCAL.y, class: "arrow mem thick", "marker-end": "url(#ah-mem)" });
    lab.el(svg, "text", { x: LOCAL.x, y: LOCAL.y + 24, class: "mid small", text: "the same for every surface" });

    lab.el(svg, "line", { x1: 268, y1: 170, x2: 322, y2: 170, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 295, y: 160, class: "mid small", text: "T·x + N·z" });

    // world space
    lab.el(svg, "text", { x: 350, y: 24, class: "title", text: "World space (side view)" });
    lab.el(svg, "line", { x1: 356, y1: 300, x2: 392, y2: 300, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: 356, y1: 300, x2: 356, y2: 264, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 398, y: 304, class: "small", text: "+X" });
    lab.el(svg, "text", { x: 362, y: 268, class: "small", text: "+Y" });
    const surface = lab.el(svg, "line", { class: "surface" });
    const worldT = lab.el(svg, "line", { x1: WORLD.x, y1: WORLD.y, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const worldNAxis = lab.el(svg, "line", { x1: WORLD.x, y1: WORLD.y, class: "arrow", "marker-end": "url(#ah)" });
    const stepT = lab.el(svg, "line", { x1: WORLD.x, y1: WORLD.y, class: "arrow hot thick" });
    const stepN = lab.el(svg, "line", { class: "arrow dashed" });
    const worldN = lab.el(svg, "line", { x1: WORLD.x, y1: WORLD.y, class: "arrow mem thick", "marker-end": "url(#ah-mem)" });
    const tText = lab.el(svg, "text", { class: "mid title t-hot halo", text: "T" });
    const nAxisText = lab.el(svg, "text", { class: "mid title halo", text: "N" });
    const nText = lab.el(svg, "text", { class: "mid title t-mem halo", text: "n" });

    lab.draw(function () {
        // the texel as stored, then as a shader reads it back: decode, normalize
        const x = texel.value;
        const stored = [normalMappingEncode(x), normalMappingEncode(0), normalMappingEncode(Math.sqrt(1 - x * x))];
        let n = stored.map(normalMappingDecode);
        const length = Math.hypot(n[0], n[1], n[2]);
        n = n.map(function (c) { return c / length; });

        const angle = tilt.value * Math.PI / 180;
        const T = [Math.cos(angle), Math.sin(angle)];
        const N = [-Math.sin(angle), Math.cos(angle)];
        const world = [T[0] * n[0] + N[0] * n[2], T[1] * n[0] + N[1] * n[2]];
        function at(vx, vy, scale) { return [WORLD.x + vx * scale, WORLD.y - vy * scale]; }

        lab.set(swatch, { fill: `rgb(${stored[0]},${stored[1]},${stored[2]})` });
        lab.set(swatchText, { text: `(${stored[0]}, ${stored[1]}, ${stored[2]})` });
        lab.set(decodedText, { text: `(${normalMappingNum(n[0], 2)}, ${normalMappingNum(n[1], 2)}, ${normalMappingNum(n[2], 2)})` });
        lab.set(localN, { x2: LOCAL.x + n[0] * 90, y2: LOCAL.y - n[2] * 90 });

        const s0 = at(-T[0], -T[1], 150);
        const s1 = at(T[0], T[1], 150);
        lab.set(surface, { x1: s0[0], y1: s0[1], x2: s1[0], y2: s1[1] });
        const tTip = at(T[0], T[1], 120);
        const nTip = at(N[0], N[1], 120);
        lab.set(worldT, { x2: tTip[0], y2: tTip[1] });
        lab.set(worldNAxis, { x2: nTip[0], y2: nTip[1] });
        const tLabel = at(T[0] * 0.8 - N[0] * 0.14, T[1] * 0.8 - N[1] * 0.14, 136);
        const nLabel = at(N[0], N[1], 136);
        lab.set(tText, { x: tLabel[0], y: tLabel[1] + 5 });
        lab.set(nAxisText, { x: nLabel[0], y: nLabel[1] + 5 });
        const corner = at(T[0] * n[0], T[1] * n[0], UNIT);
        const tip = at(world[0], world[1], UNIT);
        lab.set(stepT, { x2: corner[0], y2: corner[1] });
        lab.set(stepN, { x1: corner[0], y1: corner[1], x2: tip[0], y2: tip[1] });
        lab.set(worldN, { x2: tip[0], y2: tip[1] });
        // the label sits past the tip, on the side away from N
        const side = n[0] >= 0 ? 1 : -1;
        const label = at(world[0] + side * T[0] * 0.16, world[1] + side * T[1] * 0.16, UNIT);
        lab.set(nText, { x: label[0], y: label[1] + 5 });

        bytesOut.set(`${stored[0]}, ${stored[1]}, ${stored[2]}`, "mem");
        tangentOut.set(`${normalMappingNum(n[0], 2)}, ${normalMappingNum(n[1], 2)}, ${normalMappingNum(n[2], 2)}`);
        worldOut.set(`${normalMappingNum(world[0], 2)}, ${normalMappingNum(world[1], 2)}`, "mem");
        if (Math.abs(x) < 0.011) note.set("The flat texel, (128, 128, 255): the normal is N itself, whatever way the surface faces.", "ok");
        else if (tilt.value === 0) note.set("The surface is level: T is +X and N is +Y in the world, so the world-space normal has the same numbers as the texel's x and z.");
        else note.set(`The surface is tilted by ${normalMappingNum(tilt.value, 0)}°. The texel did not change: only T and N did, and the world-space normal turned with them.`, "ok");
    });
});

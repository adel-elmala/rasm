// Labs of docs/guides/taa.html.

// One pixel cut by an edge. Each frame the sample point is jittered (Halton base 2 and 3 over 8 phases, as RASM's
// getTaaJitter) and lands inside or outside the edge; the history blends each frame in with weight alpha. The plot
// shows the pixel's value frame by frame against the share of the pixel the edge covers.
RasmLab.register("taa-converge", function (lab) {
    const svg = lab.svg(760, 260, "Left, one pixel with a slanted edge and this frame's jittered sample point. Right, a plot of the pixel's value over 48 frames: the history converges toward the share of the pixel the edge covers.");
    const edge = lab.slider({ label: "Edge position", min: -0.5, max: 0.5, step: 0.05, value: 0.15, format: function (v) { return v.toFixed(2); } });
    const alpha = lab.slider({ label: "This frame's weight", min: 0.05, max: 1, step: 0.05, value: 0.1, format: function (v) { return v.toFixed(2); } });
    const coverOut = lab.readout("true coverage");
    const valueOut = lab.readout("value after 48 frames");
    const note = lab.note();

    const FRAMES = 48;
    function halton(index, base) {
        let result = 0;
        let fraction = 1;
        while (index > 0) {
            fraction /= base;
            result += fraction * (index % base);
            index = Math.floor(index / base);
        }
        return result;
    }
    function jitter(frame) {
        const index = frame % 8 + 1;
        return [halton(index, 2) - 0.5, halton(index, 3) - 0.5];
    }
    // The edge is the line x + 0.5 y = e: inside where x + 0.5 y < e.
    function inside(p, e) {
        return p[0] + 0.5 * p[1] < e;
    }
    function coverage(e) {
        let count = 0;
        const n = 64;
        for (let i = 0; i < n; i++)
            for (let j = 0; j < n; j++)
                if (inside([(i + 0.5) / n - 0.5, (j + 0.5) / n - 0.5], e)) count++;
        return count / (n * n);
    }

    const P = { x: 40, y: 40, s: 180 };
    lab.el(svg, "text", { x: P.x, y: 26, class: "title", text: "one pixel, 8 jitter positions" });
    lab.el(svg, "rect", { x: P.x, y: P.y, width: P.s, height: P.s, class: "paper" });
    const area = lab.el(svg, "polygon", { class: "solid o-50" });
    const dots = [];
    for (let i = 0; i < 8; i++) dots.push(lab.el(svg, "circle", { r: 5 }));

    const G = { x: 300, y: 40, w: 440, h: 180 };
    lab.el(svg, "rect", { x: G.x, y: G.y, width: G.w, height: G.h, class: "zone" });
    lab.el(svg, "text", { x: G.x, y: 26, class: "title", text: "the pixel's value, frame by frame" });
    const truth = lab.el(svg, "line", { class: "arrow ok dashed" });
    const raw = lab.el(svg, "path", { class: "arrow mem" });
    const smooth = lab.el(svg, "path", { class: "arrow gpu thick" });
    lab.el(svg, "text", { x: G.x, y: G.y + G.h + 18, class: "small", text: "frame 0" });
    lab.el(svg, "text", { x: G.x + G.w, y: G.y + G.h + 18, class: "end small", text: `frame ${FRAMES - 1}` });
    lab.el(svg, "text", { x: G.x - 6, y: G.y + 4, class: "end small", text: "1" });
    lab.el(svg, "text", { x: G.x - 6, y: G.y + G.h + 4, class: "end small", text: "0" });

    function gy(v) { return G.y + G.h * (1 - v); }
    function gx(f) { return G.x + G.w * f / (FRAMES - 1); }

    lab.draw(function () {
        const e = edge.value;
        // The polygon of the covered part, clipped to the pixel square: x < e - 0.5 y.
        const pts = [];
        for (let t = 0; t <= 20; t++) {
            const y = -0.5 + t / 20;
            pts.push([Math.max(-0.5, Math.min(0.5, e - 0.5 * y)), y]);
        }
        const poly = [[-0.5, -0.5]].concat(pts).concat([[-0.5, 0.5]]);
        lab.set(area, { points: poly.map(function (p) { return `${P.x + (p[0] + 0.5) * P.s},${P.y + (p[1] + 0.5) * P.s}`; }).join(" ") });
        for (let i = 0; i < 8; i++) {
            const j = jitter(i);
            lab.set(dots[i], { cx: P.x + (j[0] + 0.5) * P.s, cy: P.y + (j[1] + 0.5) * P.s, class: inside(j, e) ? "ok" : "bad" });
        }
        const cover = coverage(e);
        let history = 0;
        let rawPath = "";
        let smoothPath = "";
        for (let f = 0; f < FRAMES; f++) {
            const sample = inside(jitter(f), e) ? 1 : 0;
            history = f === 0 ? sample : history + (sample - history) * alpha.value;
            rawPath += `${f === 0 ? "M" : "L"}${gx(f).toFixed(1)},${gy(sample).toFixed(1)}`;
            smoothPath += `${f === 0 ? "M" : "L"}${gx(f).toFixed(1)},${gy(history).toFixed(1)}`;
        }
        lab.set(truth, { x1: G.x, x2: G.x + G.w, y1: gy(cover), y2: gy(cover) });
        lab.set(raw, { d: rawPath });
        lab.set(smooth, { d: smoothPath });
        coverOut.set(cover.toFixed(2));
        const off = Math.abs(history - cover);
        valueOut.set(history.toFixed(2), off < 0.1 ? "ok" : "bad");
        note.set(alpha.value >= 0.95 ? "Weight 1 keeps only this frame: the pixel jumps between 0 and 1 as the jitter moves, which is the flicker of an edge without anti-aliasing." :
            `Each frame samples another point of the pixel; the history (green) settles near the edge's share of the pixel, ${off < 0.1 ? "within 0.1" : "still off by " + off.toFixed(2)} after ${FRAMES} frames.`, off < 0.1 ? "ok" : "bad");
    });
});

// A row of pixels: a bright object moves right over a dark, still background. The background's motion is 0, so a
// pixel the object has just left finds the object in its history: a ghost trail. Clipping the history to the colors
// around the pixel this frame cuts the trail.
RasmLab.register("taa-clip", function (lab) {
    const svg = lab.svg(760, 220, "A row of 40 pixels. A bright object moves right over a dark background. Top row: this frame. Bottom row: TAA's output, with a ghost trail behind the object when the history is not clipped.");
    const speed = lab.slider({ label: "Object speed (pixels per frame)", min: 0, max: 3, step: 1, value: 1, format: function (v) { return `${v}`; } });
    const clip = lab.toggle({ label: "Clip the history to the 3 pixels around it", value: false });
    const trailOut = lab.readout("ghost pixels (not next to the object)");
    const note = lab.note();

    const N = 40;
    const W = 17;
    const X = 40;
    const OBJECT = 6;
    const FRAMES = 30;
    const ALPHA = 0.1;
    const BACKGROUND = 0.1;
    const BRIGHT = 1.0;
    lab.el(svg, "text", { x: X, y: 24, class: "small", text: "this frame" });
    lab.el(svg, "text", { x: X, y: 114, class: "small", text: "TAA output after 30 frames" });
    const top = [];
    const bottom = [];
    for (let i = 0; i < N; i++) {
        top.push(lab.el(svg, "rect", { x: X + i * W, y: 32, width: W - 1, height: 50 }));
        bottom.push(lab.el(svg, "rect", { x: X + i * W, y: 122, width: W - 1, height: 50 }));
    }
    function gray(v) {
        const b = Math.round(255 * Math.pow(Math.max(0, Math.min(1, v)), 1 / 2.2));
        return `rgb(${b}, ${b}, ${b})`;
    }

    lab.draw(function () {
        let history = new Array(N).fill(BACKGROUND);
        let frame = [];
        let start = 2;
        for (let f = 0; f < FRAMES; f++) {
            start = 2 + f * speed.value;
            frame = [];
            for (let i = 0; i < N; i++) frame.push(i >= start % N && i < start % N + OBJECT ? BRIGHT : BACKGROUND);
            const next = [];
            for (let i = 0; i < N; i++) {
                // The object's pixels move with it; the background's stand still.
                const onObject = frame[i] === BRIGHT;
                const from = onObject ? i - speed.value : i;
                let h = from >= 0 && from < N ? history[from] : frame[i];
                if (clip.value) {
                    const around = [frame[Math.max(0, i - 1)], frame[i], frame[Math.min(N - 1, i + 1)]];
                    h = Math.max(Math.min.apply(null, around), Math.min(Math.max.apply(null, around), h));
                }
                next.push(h + (frame[i] - h) * ALPHA);
            }
            history = next;
        }
        let ghosts = 0;
        for (let i = 0; i < N; i++) {
            lab.set(top[i], { fill: gray(frame[i]) });
            lab.set(bottom[i], { fill: gray(history[i]) });
            // A pixel next to the object has the object in its 3 neighbors, so some of it stays there with clipping too.
            const nearObject = frame[Math.max(0, i - 1)] === BRIGHT || frame[Math.min(N - 1, i + 1)] === BRIGHT;
            if (frame[i] === BACKGROUND && !nearObject && history[i] > BACKGROUND + 0.05) ghosts++;
        }
        trailOut.set(`${ghosts}`, ghosts > 0 ? "bad" : "ok");
        if (speed.value === 0)
            note.set("A still object: the history matches this frame everywhere, and nothing ghosts.", "ok");
        else if (ghosts > 0)
            note.set("Behind the object the background's history still holds the object's light, and fades only by 10 % a frame: a trail.", "bad");
        else
            note.set("Each history value is clipped to the colors around its pixel this frame. Behind the object only background is around, so the trail is cut at once.", "ok");
    });
});

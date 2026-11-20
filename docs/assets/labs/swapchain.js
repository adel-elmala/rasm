// Labs of docs/guides/swapchain.html.

// A scrolling timeline of a GPU and a 60 Hz screen, stepped with the rules of the three present modes, and a
// small screen that shows which frames the display draws in the current refresh.
RasmLab.register("present-modes", function (lab) {
    const svg = lab.svg(760, 150, "A timeline with a GPU row and a screen row, with a vblank every 16.7 milliseconds, and a small screen on the right that shows the frames of the current refresh.");
    const mode = lab.choice({ label: "Mode", options: ["FIFO", "MAILBOX", "IMMEDIATE"], value: "FIFO" });
    const gpuMs = lab.slider({ label: "GPU per frame", min: 4, max: 40, value: 10, format: function (v) { return `${v} ms`; } });
    const images = lab.choice({ label: "Images (FIFO)", options: [{ value: 2, label: "2" }, { value: 3, label: "3" }], value: 2 });
    const shownOut = lab.readout("frames shown per second");
    const lostOut = lab.readout("frames drawn and never shown, per second");
    const tearOut = lab.readout("tear lines per second");
    const note = lab.note();

    const REFRESH = 1000 / 60;
    const EPS = 1e-6;
    const LIMIT = 3000;         // milliseconds to simulate
    const X0 = 110;
    const X1 = 590;
    const PX = 6;               // pixels per millisecond, like the page's figure
    const BEFORE = 25;
    const SPEED = 10;           // simulated milliseconds per second
    const SCREEN = { x: 628, y: 26, w: 112, h: 84 };
    const playX = X0 + BEFORE * PX;
    let now = 40;

    lab.el(svg, "text", { x: 100, y: 54, class: "end small t-gpu", text: "GPU" });
    lab.el(svg, "text", { x: 100, y: 86, class: "end small", text: "screen" });
    lab.el(svg, "text", { x: X1, y: 14, class: "end small", text: "dashed: a vblank, every 16.7 ms" });
    lab.el(svg, "line", { x1: X0, y1: 120, x2: X1, y2: 120, class: "arrow" });
    lab.el(svg, "rect", { x: SCREEN.x, y: SCREEN.y, width: SCREEN.w, height: SCREEN.h, rx: 4, class: "paper" });
    lab.el(svg, "text", { x: SCREEN.x + SCREEN.w / 2, y: 128, class: "mid small", text: "the screen," });
    lab.el(svg, "text", { x: SCREEN.x + SCREEN.w / 2, y: 142, class: "mid small", text: "this refresh" });
    lab.el(svg, "line", { x1: playX, y1: 22, x2: playX, y2: 120, class: "arrow hot" });
    const moving = lab.el(svg, "g");
    lab.el(svg, "text", { x: playX, y: 14, class: "mid small t-hot", text: "now" });

    // Steps one mode. Returns the frames the GPU draws, its waits, and every change of the image on screen.
    function simulate(kind, g, imageCount) {
        const frames = [];      // { start, end, shown }
        const waits = [];       // { from, to }: the GPU has no free image
        const shows = [];       // { frame, at, tear }
        if (kind === "FIFO") {
            let free = imageCount - 1;      // one image is on screen
            const queue = [];
            let rendering = false;
            let waitingSince = -1;
            let vblank = 1;
            let time = 0;
            const begin = function (t) {
                free -= 1;
                frames.push({ start: t, end: t + g, shown: -1 });
                rendering = true;
            };
            begin(0);
            while (time < LIMIT) {
                const end = frames[frames.length - 1].end;
                if (rendering && end < vblank * REFRESH - EPS) {
                    time = end;
                    rendering = false;
                    queue.push(frames.length - 1);
                    if (free > 0) begin(time);
                    else waitingSince = time;
                } else {
                    time = vblank * REFRESH;
                    vblank += 1;
                    if (!queue.length) continue;
                    const f = queue.shift();
                    frames[f].shown = time;
                    shows.push({ frame: f, at: time, tear: false });
                    free += 1;              // the image that was on screen
                    if (!rendering) {
                        if (time - waitingSince > EPS) waits.push({ from: waitingSince, to: time });
                        begin(time);
                    }
                }
            }
            return { frames: frames, waits: waits, shows: shows };
        }
        for (let f = 0; f * g < LIMIT; f++) frames.push({ start: f * g, end: (f + 1) * g, shown: -1 });
        if (kind === "MAILBOX") {
            let newest = -1;
            for (let vblank = 1; vblank * REFRESH < LIMIT; vblank++) {
                const at = vblank * REFRESH;
                let f = newest;
                while (f + 1 < frames.length && frames[f + 1].end < at - EPS) f += 1;
                if (f === newest) continue;
                newest = f;
                frames[f].shown = at;
                shows.push({ frame: f, at: at, tear: false });
            }
        } else {
            frames.forEach(function (frame, f) {
                const phase = frame.end / REFRESH;
                frame.shown = frame.end;
                shows.push({ frame: f, at: frame.end, tear: Math.abs(phase - Math.round(phase)) * REFRESH > 1e-3 });
            });
        }
        return { frames: frames, waits: waits, shows: shows };
    }

    function box(from, to, y, cls, label, labelClass, shortLabel) {
        const x0 = Math.max(X0, playX + (from - now) * PX);
        const x1 = Math.min(X1, playX + (to - now) * PX);
        if (x1 - x0 < 1) return;
        lab.el(moving, "rect", { x: x0.toFixed(1), y: y, width: (x1 - x0).toFixed(1), height: 20, rx: 3, class: cls });
        const text = x1 - x0 >= label.length * 5.5 + 8 ? label : shortLabel;
        if (text && x1 - x0 >= text.length * 5.5 + 8) {
            lab.el(moving, "text", { x: ((x0 + x1) / 2).toFixed(1), y: y + 14, class: labelClass || "mid small", text: text });
        }
    }

    lab.animate(function (dt) {
        now += dt * SPEED;
    });

    let key = "";
    let sim = null;
    lab.draw(function () {
        const g = gpuMs.value;
        if (key !== `${mode.value}/${g}/${images.value}`) {
            key = `${mode.value}/${g}/${images.value}`;
            sim = simulate(mode.value, g, images.value);
        }
        if (now > LIMIT - 400) now -= 120 * REFRESH;

        lab.clear(moving);
        const from = now - BEFORE;
        const to = now + (X1 - playX) / PX;
        for (let v = Math.max(1, Math.ceil(from / REFRESH)); v * REFRESH <= to; v++) {
            const x = (playX + (v * REFRESH - now) * PX).toFixed(1);
            lab.el(moving, "line", { x1: x, y1: 30, x2: x, y2: 124, class: "arrow dashed" });
            lab.el(moving, "text", { x: x, y: 138, class: "mid small", text: (v * REFRESH).toFixed(v % 3 ? 1 : 0) });
        }
        sim.frames.forEach(function (frame, f) {
            if (frame.end < from || frame.start > to) return;
            const never = frame.shown < 0 && mode.value === "MAILBOX";
            box(frame.start, frame.end, 40, never ? "ghost" : "gpu", String(f), never ? "mid small t-faint" : "mid small");
        });
        sim.waits.forEach(function (wait) {
            if (wait.to >= from && wait.from <= to) box(wait.from, wait.to, 40, "bad", "no free image", "mid small", "waits");
        });
        sim.shows.forEach(function (show, k) {
            const until = k + 1 < sim.shows.length ? sim.shows[k + 1].at : LIMIT;
            if (until < from || show.at > to) return;
            box(show.at, until, 72, "box", String(show.frame));
            if (show.tear && show.at > from) {
                const x = (playX + (show.at - now) * PX).toFixed(1);
                lab.el(moving, "line", { x1: x, y1: 68, x2: x, y2: 96, class: "hot" });
            }
        });

        // the small screen: the display draws it from top to bottom during one refresh
        const base = Math.floor(now / REFRESH) * REFRESH;
        let onScreen = -1;
        const cuts = [];
        sim.shows.forEach(function (show) {
            if (show.at <= base + EPS) onScreen = show.frame;
            else if (show.at < base + REFRESH - EPS) cuts.push(show);
        });
        let top = SCREEN.y;
        let frame = onScreen;
        cuts.concat([null]).forEach(function (cut) {
            const bottom = cut ? SCREEN.y + SCREEN.h * (cut.at - base) / REFRESH : SCREEN.y + SCREEN.h;
            if (bottom - top >= 14) {
                lab.el(moving, "text", { x: SCREEN.x + SCREEN.w / 2, y: ((top + bottom) / 2 + 4).toFixed(1), class: "mid mono", text: frame < 0 ? "–" : `frame ${frame}` });
            }
            if (cut) {
                lab.el(moving, "line", { x1: SCREEN.x, y1: bottom.toFixed(1), x2: SCREEN.x + SCREEN.w, y2: bottom.toFixed(1), class: "hot" });
                frame = cut.frame;
            }
            top = bottom;
        });
        const scan = (SCREEN.y + SCREEN.h * (now - base) / REFRESH).toFixed(1);
        lab.el(moving, "line", { x1: SCREEN.x - 8, y1: scan, x2: SCREEN.x, y2: scan, class: "arrow", "marker-end": "url(#ah)" });

        // counts over one second, after the start has settled
        let shown = 0;
        let tears = 0;
        let lost = 0;
        sim.shows.forEach(function (show) {
            if (show.at < 1000 - EPS || show.at >= 2000 - EPS) return;
            shown += 1;
            if (show.tear) tears += 1;
        });
        sim.frames.forEach(function (f) {
            if (f.shown < 0 && f.end >= 1000 - EPS && f.end < 2000 - EPS) lost += 1;
        });
        shownOut.set(String(shown), shown >= 60 ? "ok" : "");
        lostOut.set(String(lost));
        tearOut.set(String(tears), tears ? "bad" : "ok");
        if (mode.value === "IMMEDIATE") {
            note.set(`IMMEDIATE: each frame goes on screen when it is done, in the middle of a refresh. The screen shows parts of two or more frames: ${tears} tear lines per second.`, "bad");
        } else if (mode.value === "MAILBOX") {
            if (lost) note.set(`MAILBOX: the GPU never waits. Each refresh shows the newest finished frame, so ${lost} frames per second are drawn and never shown. No tearing.`, "ok");
            else note.set("MAILBOX with a GPU slower than the display: every frame is shown, and some stay for two refreshes or more. No tearing.", "ok");
        } else if (shown >= 60) {
            note.set(`FIFO: every refresh gets a new frame. The GPU is done early and waits ${(REFRESH - g).toFixed(1)} ms in each refresh. No tearing.`, "ok");
        } else if (images.value === 2) {
            note.set(`FIFO with 2 images: while a finished frame waits for its refresh, the GPU has no free image. Only ${shown} frames per second reach the screen.`, "bad");
        } else {
            note.set(`FIFO with 3 images: the GPU starts the next frame during the wait, so ${shown} frames per second reach the screen. But they do not all stay equally long: judder.`, "bad");
        }
    });
});

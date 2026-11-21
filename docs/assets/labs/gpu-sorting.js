// Labs of docs/guides/gpu-sorting.html.

// The bitonic network on 16 keys, one step at a time. Each step compares place i with i + j where bit j of i is
// 0, sorting up where bit k of i is 0 and down where it is 1, as the sort shader does.
RasmLab.register("bitonic-steps", function (lab) {
    const N = 16;
    const svg = lab.svg(760, 300, "Sixteen keys as bars. The pairs compared in the current step of the bitonic network are joined by arrows, and the bars show the keys after the steps so far.");
    const STEPS = [];
    for (let k = 2; k <= N; k *= 2)
        for (let j = k / 2; j >= 1; j /= 2)
            STEPS.push({ k: k, j: j });
    const step = lab.slider({ label: "Steps done", min: 0, max: STEPS.length, step: 1, value: 0 });
    const stepOut = lab.readout("the next step");
    const sortedOut = lab.readout("sorted");
    const note = lab.note();

    let seed = 7;
    let start = [];
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    function shuffle() {
        start = [];
        for (let i = 0; i < N; i++) start.push(1 + Math.floor(random() * 20));
    }
    shuffle();
    lab.button("Next step", function () { step.set(Math.min(step.value + 1, STEPS.length)); });
    lab.button("Shuffle", function () { shuffle(); step.set(0); });

    const X0 = 40;
    const PITCH = 44;
    const BASE = 230;
    const bars = [];
    const labels = [];
    for (let i = 0; i < N; i++) {
        bars.push(lab.el(svg, "rect", { x: X0 + i * PITCH + 4, width: PITCH - 8, rx: 3 }));
        labels.push(lab.el(svg, "text", { x: X0 + i * PITCH + PITCH / 2, y: BASE + 18, class: "mid mono small" }));
    }
    const pairs = lab.el(svg, "g");
    const title = lab.el(svg, "text", { x: 20, y: 22, class: "title" });
    lab.el(svg, "text", { x: 20, y: 288, class: "small", text: "a bracket joins a pair compared in this step. Green: the larger key goes right (up); amber: it goes left (down)" });

    function run(count) {
        const keys = start.slice();
        for (let s = 0; s < count; s++) {
            const k = STEPS[s].k;
            const j = STEPS[s].j;
            for (let i = 0; i < N; i++) {
                if ((i & j) !== 0) continue;
                const up = (i & k) === 0;
                if ((keys[i] > keys[i + j]) === up) {
                    const t = keys[i];
                    keys[i] = keys[i + j];
                    keys[i + j] = t;
                }
            }
        }
        return keys;
    }

    lab.draw(function () {
        const done = step.value;
        const keys = run(done);
        for (let i = 0; i < N; i++) {
            const h = keys[i] * 9;
            lab.set(bars[i], { y: BASE - h, height: h, class: "gpu" });
            lab.set(labels[i], { text: String(keys[i]) });
        }
        lab.clear(pairs);
        let sorted = true;
        for (let i = 1; i < N; i++) sorted = sorted && keys[i - 1] <= keys[i];
        if (done < STEPS.length) {
            const k = STEPS[done].k;
            const j = STEPS[done].j;
            for (let i = 0; i < N; i++) {
                if ((i & j) !== 0) continue;
                const up = (i & k) === 0;
                const x1 = X0 + i * PITCH + PITCH / 2;
                const x2 = X0 + (i + j) * PITCH + PITCH / 2;
                const y = 44 + (i % 4) * 8;
                lab.el(pairs, "path", { d: `M${x1},${y + 8} V${y} H${x2} V${y + 8}`, class: up ? "arrow gpu" : "arrow mem" });
            }
            lab.set(title, { text: `step ${done + 1} of ${STEPS.length}: merge size k = ${k}, distance j = ${j}` });
            stepOut.set(`k ${k}, j ${j}`);
        } else {
            lab.set(title, { text: `all ${STEPS.length} steps done` });
            stepOut.set("none");
        }
        // the last merge whose steps are all done: pieces of that size are sorted, in turns up and down
        let piece = 1;
        for (let s = 0; s < done; s++)
            if (STEPS[s].j === 1) piece = STEPS[s].k;
        sortedOut.set(sorted ? "yes" : "not yet", sorted ? "ok" : "bad");
        if (done === STEPS.length)
            note.set("The network always ends sorted, whatever the keys were.", "ok");
        else if (sorted)
            note.set("Sorted already, but the network still runs every step: it does not look at the data.", "ok");
        else if (piece === 1)
            note.set("Nothing is sorted yet. The first merges sort pairs: up, down, up, down.");
        else
            note.set(`Pieces of ${piece} keys are sorted, in turns up and down. The next merge joins two of them.`);
    });
});

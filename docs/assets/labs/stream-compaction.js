// Labs of docs/guides/stream-compaction.html.

// Twelve items with a keep / drop flag each. The output is built by the exclusive scan of the flags, or by an
// atomic counter that the threads reach in a new order on every run.
RasmLab.register("compact-flags", function (lab) {
    const svg = lab.svg(760, 276, "Twelve items with a keep or drop flag each, the place each kept item gets, and the packed output with an arrow from every kept item to its place.");
    const method = lab.choice({ label: "Places come from", options: [{ value: "scan", label: "a prefix sum" }, { value: "atomic", label: "an atomic counter" }], value: "scan" });
    const countOut = lab.readout("kept items: the count");
    const orderOut = lab.readout("order of the output");
    const runOut = lab.readout("run");
    const note = lab.note();

    const N = 12;
    const NAMES = "abcdefghijkl";
    const X0 = 190;
    const PITCH = 46;
    const W = 40;
    const flags = [1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0];
    let run = 1;
    let seed = 11;
    let arrival = [];           // the order in which the threads reach the counter in this run

    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    function shuffle() {
        arrival = [];
        for (let i = 0; i < N; i++) arrival.push(i);
        for (let i = N - 1; i > 0; i--) {
            const j = Math.floor(random() * (i + 1));
            const t = arrival[i];
            arrival[i] = arrival[j];
            arrival[j] = t;
        }
    }
    shuffle();
    lab.button("Run again", function () {
        run += 1;
        shuffle();
    });

    function cx(i) { return X0 + i * PITCH + W / 2; }

    lab.el(svg, "text", { x: 20, y: 34, class: "title", text: "items" });
    lab.el(svg, "text", { x: 20, y: 50, class: "small", text: "click: keep or drop" });
    lab.el(svg, "text", { x: 20, y: 86, class: "title", text: "1. flag" });
    lab.el(svg, "text", { x: 20, y: 102, class: "small", text: "1 = keep" });
    const placeTitle = lab.el(svg, "text", { x: 20, y: 138, class: "title" });
    const placeSmall = lab.el(svg, "text", { x: 20, y: 154, class: "small" });
    lab.el(svg, "text", { x: 20, y: 226, class: "title", text: "3. scatter" });
    lab.el(svg, "text", { x: 20, y: 242, class: "small", text: "out[place] = item" });

    const arrows = lab.el(svg, "g");
    const cells = [];
    for (let i = 0; i < N; i++) {
        const x = X0 + i * PITCH;
        const cell = {
            item: lab.el(svg, "rect", { x: x, y: 16, width: W, height: 30, rx: 4 }),
            itemText: lab.el(svg, "text", { x: cx(i), y: 36, class: "mid mono", "pointer-events": "none", text: NAMES[i] }),
            flag: lab.el(svg, "rect", { x: x, y: 68, width: W, height: 30, rx: 4 }),
            flagText: lab.el(svg, "text", { x: cx(i), y: 88 }),
            place: lab.el(svg, "rect", { x: x, y: 120, width: W, height: 30, rx: 4 }),
            placeText: lab.el(svg, "text", { x: cx(i), y: 140 }),
            out: lab.el(svg, "rect", { x: x, y: 208, width: W, height: 30, rx: 4 }),
            outText: lab.el(svg, "text", { x: cx(i), y: 228, class: "mid mono" }),
        };
        lab.clickable(cell.item, `Keep or drop item ${NAMES[i]}`, function () { flags[i] = 1 - flags[i]; });
        cells.push(cell);
    }
    const countText = lab.el(svg, "text", { x: X0, y: 264, class: "mono small" });

    lab.draw(function () {
        const atomic = method.value === "atomic";

        // the place of every item
        const place = [];
        let count = 0;
        if (atomic) {
            // each kept thread takes the counter's old value, in the order the threads arrive
            for (let i = 0; i < N; i++) place.push(-1);
            arrival.forEach(function (i) {
                if (flags[i]) place[i] = count++;
            });
        } else {
            // the exclusive scan of the flags
            for (let i = 0; i < N; i++) {
                place.push(count);
                count += flags[i];
            }
        }

        const out = [];
        for (let i = 0; i < N; i++) if (flags[i]) out[place[i]] = i;
        let inOrder = true;
        for (let k = 1; k < count; k++) if (out[k] < out[k - 1]) inOrder = false;

        lab.set(placeTitle, { text: atomic ? "2. slot" : "2. exclusive" });
        lab.set(placeSmall, { text: atomic ? "from the counter" : "scan of flags" });
        lab.clear(arrows);
        for (let i = 0; i < N; i++) {
            const c = cells[i];
            const keep = flags[i] === 1;
            lab.set(c.item, { class: keep ? "gpu pick" : "ghost pick" });
            lab.set(c.itemText, { class: keep ? "mid mono" : "mid mono t-faint" });
            lab.set(c.flag, { class: keep ? "gpu" : "ghost" });
            lab.set(c.flagText, { text: flags[i], class: keep ? "mid mono" : "mid mono t-faint" });
            lab.set(c.place, { class: keep ? "mem" : "ghost" });
            lab.set(c.placeText, { text: place[i] < 0 ? "" : place[i], class: keep ? "mid mono" : "mid mono t-faint" });
            lab.set(c.out, { class: i < count ? "mem" : "ghost" });
            lab.set(c.outText, { text: i < count ? NAMES[out[i]] : "" });
            if (keep) {
                const color = atomic && !inOrder ? "hot" : "gpu";
                lab.el(arrows, "line", { x1: cx(i), y1: 152, x2: cx(place[i]), y2: 205, class: `arrow ${color}`, "marker-end": `url(#ah-${color})` });
            }
        }
        if (atomic) lab.set(countText, { text: `count = the counter after the last add = ${count}` });
        else lab.set(countText, { text: `count = scan[${N - 1}] + flag[${N - 1}] = ${place[N - 1]} + ${flags[N - 1]} = ${count}` });

        countOut.set(String(count), "gpu");
        orderOut.set(inOrder ? "input order" : "changed", inOrder ? "ok" : "bad");
        runOut.set(String(run));
        if (count === 0) note.set("No item passes: the output is empty and the count is 0.");
        else if (!atomic) note.set("Each kept item goes to the place its scan value names. The places grow from left to right, so the output is packed and in input order, in every run.", "ok");
        else if (inOrder) note.set("Packed, and this run happens to be in input order: the kept threads reached the counter from left to right. Run it again.");
        else {
            const order = arrival.filter(function (i) { return flags[i] === 1; }).map(function (i) { return NAMES[i]; }).join(", ");
            note.set(`Packed, but not in input order: in this run the kept threads reached the counter in the order ${order}.`, "bad");
        }
    });
});

// The three scans of section 4 run on a group of n ones, and every addition and step is counted.
RasmLab.register("scan-cost", function (lab) {
    const svg = lab.svg(760, 196, "Three bars per method: the additions and the steps that the naive loop, the Hillis-Steele scan and the Blelloch scan need for one group.");
    const size = lab.slider({ label: "Items in the group", min: 2, max: 8, step: 1, value: 6, format: function (v) { return String(Math.pow(2, v)); } });
    const naiveOut = lab.readout("additions, naive loop");
    const hillisOut = lab.readout("additions, Hillis–Steele");
    const blellochOut = lab.readout("additions, Blelloch");
    const note = lab.note();

    // each scan really runs; the result is checked against a plain loop
    function naive(x) {
        const out = [];
        let adds = 0;
        for (let i = 0; i < x.length; i++) {
            let sum = 0;
            for (let j = 0; j < i; j++) { sum += x[j]; adds += 1; }
            out.push(sum);
        }
        return { adds: adds, steps: 1, exclusive: out };
    }
    function hillisSteele(x) {
        let a = x.slice();
        let adds = 0;
        let steps = 0;
        for (let d = 1; d < a.length; d *= 2) {
            const b = a.slice();
            for (let i = d; i < a.length; i++) { b[i] = a[i] + a[i - d]; adds += 1; }
            a = b;
            steps += 1;
        }
        return { adds: adds, steps: steps, exclusive: a.map(function (v, i) { return v - x[i]; }) };
    }
    function blelloch(x) {
        const a = x.slice();
        const n = a.length;
        let adds = 0;
        let steps = 0;
        for (let d = 1; d < n; d *= 2) {
            for (let i = 2 * d - 1; i < n; i += 2 * d) { a[i] += a[i - d]; adds += 1; }
            steps += 1;
        }
        a[n - 1] = 0;
        for (let d = n / 2; d >= 1; d /= 2) {
            for (let i = 2 * d - 1; i < n; i += 2 * d) {
                const left = a[i - d];
                a[i - d] = a[i];
                a[i] += left;
                adds += 1;
            }
            steps += 1;
        }
        return { adds: adds, steps: steps, exclusive: a };
    }

    const METHODS = [
        { name: "Naive loop", run: naive },
        { name: "Hillis–Steele", run: hillisSteele },
        { name: "Blelloch", run: blelloch },
    ];
    const BAR = { x: 150, width: 440 };
    lab.el(svg, "text", { x: BAR.x, y: 16, class: "small", text: "additions (bar length), next to the largest of the three" });
    lab.el(svg, "text", { x: 740, y: 16, class: "end small", text: "steps (barriers)" });
    const rows = METHODS.map(function (m, k) {
        const y = 30 + k * 52;
        lab.el(svg, "text", { x: 20, y: y + 22, class: "title", text: m.name });
        return {
            bar: lab.el(svg, "rect", { x: BAR.x, y: y + 4, height: 28, rx: 3, class: "gpu" }),
            adds: lab.el(svg, "text", { y: y + 23, class: "mono" }),
            steps: lab.el(svg, "g"),
            y: y,
        };
    });
    lab.el(svg, "line", { x1: BAR.x, y1: 26, x2: BAR.x, y2: 182, class: "arrow" });

    lab.draw(function () {
        const n = Math.pow(2, size.value);
        const x = [];
        for (let i = 0; i < n; i++) x.push(i % 3 === 1 ? 0 : 1);       // some flags
        const results = METHODS.map(function (m) { return m.run(x); });
        const reference = results[0].exclusive.join(",");
        results.forEach(function (r) {
            if (r.exclusive.join(",") !== reference) throw new Error("a scan gave a wrong result");
        });
        const most = Math.max(results[0].adds, results[1].adds, results[2].adds);
        results.forEach(function (r, k) {
            const row = rows[k];
            const w = Math.max(2, BAR.width * r.adds / most);
            lab.set(row.bar, { width: w.toFixed(1) });
            lab.set(row.adds, { x: (BAR.x + w + 8).toFixed(1), text: r.adds });
            lab.clear(row.steps);
            for (let s = 0; s < r.steps; s++) {
                lab.el(row.steps, "rect", { x: 732 - s * 9, y: row.y + 8, width: 6, height: 20, rx: 1, class: "accent" });
            }
            lab.el(row.steps, "text", { x: 726 - (r.steps - 1) * 9, y: row.y + 23, class: "end small", text: r.steps });
        });
        naiveOut.set(String(results[0].adds));
        hillisOut.set(String(results[1].adds));
        blellochOut.set(String(results[2].adds));
        const least = results[0].adds <= results[2].adds ? "the naive loop" : "Blelloch";
        if (n <= 4) note.set(`For ${n} items the naive loop is about as cheap as the others, and it needs one step only.`);
        else note.set(`For ${n} items ${least} does the fewest additions, but Hillis–Steele needs half of Blelloch's steps: ${results[1].steps} against ${results[2].steps}.`);
    });
});

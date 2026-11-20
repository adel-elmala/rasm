// Labs of docs/guides/memory-aliasing.html.

// The page's five textures. The reader changes their lifetimes; the lab packs them with the page's greedy
// first fit (biggest first) and draws the heap next to the peak.
RasmLab.register("pack-lifetimes", function (lab) {
    const svg = lab.svg(760, 300, "Left: five textures as lifetime bars over six passes; each cell can be clicked to change a lifetime. Right: the heap that greedy first fit builds from them, as offset over passes.");
    const heapOut = lab.readout("heap with aliasing");
    const peakOut = lab.readout("the peak: no layout can use less");
    const savedOut = lab.readout("saved against 68 MiB, one allocation each");
    const note = lab.note();
    lab.button("Reset", function () {
        RES.forEach(function (r) { r.first = r.first0; r.last = r.last0; });
    });

    const PASS_COUNT = 6;
    const RES = [
        { name: "G-buffer", letter: "G", size: 32, first0: 0, last0: 1 },
        { name: "HDR", letter: "H", size: 16, first0: 1, last0: 3 },
        { name: "Bloom", letter: "B", size: 4, first0: 2, last0: 3 },
        { name: "LDR", letter: "L", size: 8, first0: 3, last0: 4 },
        { name: "AA", letter: "A", size: 8, first0: 4, last0: 5 },
    ];
    RES.forEach(function (r) { r.first = r.first0; r.last = r.last0; });
    const TOTAL = 68;

    // left: the lifetimes
    const LX = 136;
    const CW = 40;
    const LY = 46;
    const RH = 30;
    lab.el(svg, "text", { x: 20, y: 18, class: "title", text: "Lifetimes: click a cell" });
    lab.el(svg, "text", { x: 20, y: 38, class: "small", text: "pass" });
    for (let p = 0; p < PASS_COUNT; p++) lab.el(svg, "text", { x: LX + p * CW + CW / 2, y: 38, class: "mid small", text: String(p) });
    const cells = RES.map(function (r, k) {
        lab.el(svg, "text", { x: LX - 8, y: LY + k * RH + 16, class: "end small", text: `${r.name} · ${r.letter} · ${r.size} MiB` });
        const row = [];
        for (let p = 0; p < PASS_COUNT; p++) {
            const cell = lab.el(svg, "rect", { x: LX + p * CW + 1, y: LY + k * RH, width: CW - 2, height: 22, rx: 3 });
            lab.clickable(cell, `${r.name}, pass ${p}`, function () {
                if (p < r.first) r.first = p;
                else if (p > r.last) r.last = p;
                else if (r.first === r.last) return;
                else if (p === r.first) r.first += 1;
                else r.last = p === r.last ? p - 1 : p;
            });
            row.push(cell);
        }
        return row;
    });
    lab.el(svg, "text", { x: LX - 8, y: 218, class: "end small", text: "alive, MiB" });
    const peakBox = lab.el(svg, "rect", { y: 202, width: CW - 6, height: 24, rx: 4, class: "hot" });
    const sums = [];
    for (let p = 0; p < PASS_COUNT; p++) sums.push(lab.el(svg, "text", { x: LX + p * CW + CW / 2, y: 218 }));

    // right: the heap, offset (down) over passes
    const HX = 462;
    const HW = 44;
    const HY = 46;
    const MIB = 3.4;
    lab.el(svg, "text", { x: 420, y: 18, class: "title", text: "The heap: offset over passes" });
    lab.el(svg, "rect", { x: HX, y: HY, width: PASS_COUNT * HW, height: TOTAL * MIB, class: "zone" });
    for (let p = 0; p < PASS_COUNT; p++) lab.el(svg, "text", { x: HX + p * HW + HW / 2, y: 38, class: "mid small", text: String(p) });
    lab.el(svg, "text", { x: HX - 8, y: HY + 4, class: "end small", text: "0" });
    const totalTick = lab.el(svg, "text", { x: HX - 8, y: HY + TOTAL * MIB + 4, class: "end small t-faint", text: "68" });
    const blocks = lab.el(svg, "g");
    const heapLine = lab.el(svg, "line", { x1: HX - 4, x2: HX + PASS_COUNT * HW + 4, class: "hot dashed" });
    const heapTick = lab.el(svg, "text", { x: HX - 8, class: "end small title t-hot" });
    const heapLabel = lab.el(svg, "text", { x: HX + PASS_COUNT * HW / 2, class: "mid small t-hot" });

    function overlap(a, b) {
        return a.first <= b.last && b.first <= a.last;
    }

    lab.draw(function () {
        // greedy first fit, biggest first (a stable sort keeps LDR before AA)
        const order = RES.map(function (r, k) { return k; });
        order.sort(function (a, b) { return RES[b].size - RES[a].size || a - b; });
        const placed = [];
        let heap = 0;
        order.forEach(function (i) {
            const blockers = placed.filter(function (j) { return overlap(RES[i], RES[j]); });
            blockers.sort(function (a, b) { return RES[a].offset - RES[b].offset; });
            let offset = 0;
            for (let k = 0; k < blockers.length; k++) {
                const j = blockers[k];
                if (offset + RES[i].size <= RES[j].offset) break;
                offset = Math.max(offset, RES[j].offset + RES[j].size);
            }
            RES[i].offset = offset;
            heap = Math.max(heap, offset + RES[i].size);
            placed.push(i);
        });

        // the peak: the largest sum of sizes alive in one pass
        let peak = 0;
        let peakPass = 0;
        for (let p = 0; p < PASS_COUNT; p++) {
            let sum = 0;
            RES.forEach(function (r) { if (p >= r.first && p <= r.last) sum += r.size; });
            lab.set(sums[p], { text: String(sum), class: "mid" });
            if (sum > peak) { peak = sum; peakPass = p; }
        }
        lab.set(sums[peakPass], { class: "mid title t-hot" });
        lab.set(peakBox, { x: LX + peakPass * CW + 3 });

        RES.forEach(function (r, k) {
            for (let p = 0; p < PASS_COUNT; p++) lab.set(cells[k][p], { class: p >= r.first && p <= r.last ? "mem pick" : "box o-50 pick" });
        });

        lab.clear(blocks);
        RES.forEach(function (r) {
            const x = HX + r.first * HW + 1;
            const width = (r.last - r.first + 1) * HW - 2;
            const y = HY + r.offset * MIB;
            lab.el(blocks, "rect", { x: x, y: y + 0.5, width: width, height: r.size * MIB - 1, rx: 3, class: "mem" });
            const label = width > 80 && r.size > 4 ? `${r.letter} · ${r.size}` : r.letter;
            lab.el(blocks, "text", { x: x + width / 2, y: y + r.size * MIB / 2 + 4, class: "mid small", text: label });
        });
        const lineY = HY + heap * MIB;
        lab.set(heapLine, { y1: lineY, y2: lineY });
        lab.set(heapTick, { y: lineY + 4, text: String(heap) });
        lab.set(heapLabel, { y: lineY + 15, text: `heap: ${heap} MiB`, class: heap > TOTAL - 5 ? "hide" : "mid small t-hot" });
        lab.set(totalTick, { class: heap > TOTAL - 5 ? "hide" : "end small t-faint" });

        heapOut.set(`${heap} MiB`, heap === peak ? "ok" : "hot");
        peakOut.set(`${peak} MiB`);
        savedOut.set(`${TOTAL - heap} MiB (${Math.round(100 * (TOTAL - heap) / TOTAL)} %)`, heap < TOTAL ? "ok" : "bad");
        if (heap === TOTAL) note.set(`Nothing is shared: all five textures are alive in pass ${peakPass}, so the peak is the sum of all sizes.`, "bad");
        else if (heap === peak) note.set(`The heap equals the peak (pass ${peakPass}): the greedy result is the best possible for these lifetimes.`, "ok");
        else note.set(`The heap is ${heap - peak} MiB above the peak: the greedy method is fast, but it does not always find the best layout.`);
    });
});

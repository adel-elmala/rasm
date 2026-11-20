// Labs of docs/guides/struct-layout.html.

// The Example struct of section 5, laid out twice with the algorithm of section 2: once as C++ writes it, once
// under the rule set the reader picks. The reader changes the order of the fields.
RasmLab.register("layout-rules", function (lab) {
    const svg = lab.svg(760, 240, "The fields of one struct drawn as bytes, twice: where C++ writes them, and where the shader reads them under the chosen layout rules. Padding is dashed.");
    const rule = lab.choice({ label: "Shader layout", options: ["std140", "std430", "scalar"], value: "std430" });
    const sizeOut = lab.readout("struct size in the shader");
    const padOut = lab.readout("padding bytes in the shader");
    const rightOut = lab.readout("fields the shader reads at the right bytes");
    const note = lab.note();

    // count pieces of pieceSize bytes each: a vector is one piece, an array or a matrix has one per element or column
    const FIELDS = [
        { name: "a", decl: "float a", kind: "scalar", pieceSize: 4, count: 1, labels: ["a"] },
        { name: "b", decl: "float3 b", kind: "vector", pieceSize: 12, count: 1, labels: ["b"] },
        { name: "c", decl: "float c", kind: "scalar", pieceSize: 4, count: 1, labels: ["c"] },
        { name: "d", decl: "float2 d[2]", kind: "array", pieceSize: 8, count: 2, labels: ["d0", "d1"] },
        { name: "m", decl: "float3x3 m", kind: "matrix", pieceSize: 12, count: 3, labels: ["m c0", "m c1", "m c2"] },
    ];
    const DATA_BYTES = 84;
    const X0 = 60;
    const PX = 5;
    let order = [0, 1, 2, 3, 4];

    function roundUp(x, a) { return Math.ceil(x / a) * a; }

    // alignment and element stride of one field under a rule set (the table of section 4)
    function rules(field, ruleSet) {
        if (ruleSet === "scalar" || field.kind === "scalar") return { align: 4, stride: field.pieceSize };
        if (field.kind === "vector") return { align: 16, stride: field.pieceSize };
        if (field.kind === "matrix") return { align: 16, stride: 16 };
        // an array of float2: std140 rounds its stride and alignment up to 16
        return ruleSet === "std140" ? { align: 16, stride: 16 } : { align: 8, stride: 8 };
    }

    // the algorithm of section 2
    function layout(ruleSet) {
        let offset = 0;
        let maxAlign = 1;
        const fields = order.map(function (index) {
            const field = FIELDS[index];
            const r = rules(field, ruleSet);
            offset = roundUp(offset, r.align);
            const pieces = [];
            for (let k = 0; k < field.count; k++) pieces.push(offset + k * r.stride);
            const placed = { field: field, offset: offset, pieces: pieces };
            offset += field.count > 1 ? r.stride * field.count : field.pieceSize;
            maxAlign = Math.max(maxAlign, r.align);
            return placed;
        });
        if (ruleSet === "std140") maxAlign = roundUp(maxAlign, 16);
        return { fields: fields, size: roundUp(offset, maxAlign) };
    }

    lab.el(svg, "text", { x: X0, y: 16, class: "small", text: "declaration order: click a field to move it one place earlier" });
    const chips = [0, 1, 2, 3, 4].map(function (k) {
        const g = lab.el(svg, "g");
        lab.el(g, "rect", { x: X0 + k * 132, y: 24, width: 124, height: 26, rx: 5, class: "box" });
        const text = lab.el(g, "text", { x: X0 + k * 132 + 62, y: 42, class: "mid mono" });
        lab.clickable(g, `Move field ${k + 1} one place earlier`, function () {
            if (k === 0) order.push(order.shift());
            else order.splice(k - 1, 2, order[k], order[k - 1]);
        });
        return text;
    });
    const cppTitle = lab.el(svg, "text", { x: X0, y: 84, class: "title t-cpu" });
    const cppRow = lab.el(svg, "g");
    const gpuTitle = lab.el(svg, "text", { x: X0, y: 172, class: "title t-shader" });
    const gpuRow = lab.el(svg, "g");
    lab.button("Declared order", function () { order = [0, 1, 2, 3, 4]; });

    function drawRow(group, y, result, classOf) {
        lab.clear(group);
        let cursor = 0;
        result.fields.forEach(function (placed) {
            placed.pieces.forEach(function (at, k) {
                if (at > cursor) lab.el(group, "rect", { x: X0 + cursor * PX, y: y, width: (at - cursor) * PX, height: 32, rx: 2, class: "ghost" });
                const w = placed.field.pieceSize * PX;
                lab.el(group, "rect", { x: X0 + at * PX, y: y, width: w, height: 32, rx: 2, class: classOf(placed, k) });
                lab.el(group, "text", { x: X0 + at * PX + w / 2, y: y + 20, class: "mid small", text: placed.field.labels[k] });
                cursor = at + placed.field.pieceSize;
            });
            lab.el(group, "text", { x: X0 + placed.offset * PX, y: y + 46, class: "mid mono small", text: placed.offset });
        });
        if (result.size > cursor) lab.el(group, "rect", { x: X0 + cursor * PX, y: y, width: (result.size - cursor) * PX, height: 32, rx: 2, class: "ghost" });
        lab.el(group, "text", { x: X0 + result.size * PX, y: y + 46, class: "mid mono small", text: result.size });
    }

    lab.draw(function () {
        order.forEach(function (index, k) { lab.set(chips[k], { text: FIELDS[index].decl }); });
        const cpp = layout("scalar");
        const gpu = layout(rule.value);

        function same(k) {
            return gpu.fields[k].pieces.every(function (at, p) { return at === cpp.fields[k].pieces[p]; });
        }
        let right = 0;
        let firstWrong = -1;
        gpu.fields.forEach(function (placed, k) {
            if (same(k)) right += 1;
            else if (firstWrong < 0) firstWrong = k;
        });

        lab.set(cppTitle, { text: `C++ writes · ${cpp.size} bytes` });
        lab.set(gpuTitle, { text: `shader reads (${rule.value}) · ${gpu.size} bytes` });
        drawRow(cppRow, 92, cpp, function () { return "cpu"; });
        drawRow(gpuRow, 180, gpu, function (placed, p) {
            const k = gpu.fields.indexOf(placed);
            return placed.pieces[p] === cpp.fields[k].pieces[p] ? "ok" : "bad";
        });

        sizeOut.set(`${gpu.size} bytes`);
        padOut.set(String(gpu.size - DATA_BYTES), gpu.size === DATA_BYTES ? "ok" : "");
        rightOut.set(`${right} / ${FIELDS.length}`, right === FIELDS.length ? "ok" : "bad");

        if (firstWrong < 0) {
            note.set("Both sides agree: every field is at the offset where C++ writes it, in any order.", "ok");
            return;
        }
        const wrong = gpu.fields[firstWrong];
        const cppAt = cpp.fields[firstWrong].offset;
        if (wrong.offset !== cppAt) {
            note.set(`Mismatch: the shader reads ${wrong.field.name} at byte ${wrong.offset}, but C++ writes it at byte ${cppAt}. It gets bytes of other fields, with no error.`, "bad");
        } else if (wrong.field.kind === "matrix") {
            note.set(`Mismatch inside m: its columns are 16 bytes apart in ${rule.value} and 12 bytes apart in C++. No order of the fields fixes this.`, "bad");
        } else {
            note.set(`Mismatch inside ${wrong.field.name}: its elements are 16 bytes apart in ${rule.value} and ${wrong.field.pieceSize} bytes apart in C++.`, "bad");
        }
    });
});

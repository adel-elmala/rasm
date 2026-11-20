// Labs of docs/guides/device-addresses.html.

// The worked example of section 4: a table of 108-byte rows at a base address. The shader computes
// base + index × row size + 64 with the row size it was compiled with, and loads 12 bytes there.
RasmLab.register("pointer-math", function (lab) {
    const svg = lab.svg(760, 222, "A table of seven 108-byte rows in memory. Brackets show the steps the shader takes from the base address, and a zoomed row shows which bytes it loads.");
    const index = lab.slider({ label: "Row index", min: 0, max: 8, value: 5 });
    const size = lab.choice({ label: "Row size the shader uses", options: [{ value: 96, label: "96" }, { value: 108, label: "108 (right)" }, { value: 112, label: "112" }], value: 108 });
    const offsetOut = lab.readout("bytes from the base");
    const addressOut = lab.readout("address the shader loads");
    const landOut = lab.readout("where that is");
    const note = lab.note();

    const BASE = 0x100200000;
    const ROW = 108;            // the real row size
    const ROWS = 7;
    const FIELD = 64;           // offset of m in a row
    const FIELD_SIZE = 12;
    const X0 = 30;
    const PX = 0.72;            // the strip: 972 bytes in 700 px
    const ZX = 110;
    const ZPX = 5;              // the zoomed row: 108 bytes in 540 px

    function hex(value) {
        const digits = value.toString(16).toUpperCase();
        return `0x${digits.slice(0, -8)}_${digits.slice(-8, -4)}_${digits.slice(-4)}`;
    }
    function x(bytes) { return X0 + bytes * PX; }

    lab.el(svg, "text", { x: X0, y: 16, class: "mono small", text: `base = ${hex(BASE)}` });
    const stepText = lab.el(svg, "text", { x: 730, y: 16, class: "end small" });
    lab.el(svg, "rect", { x: x(ROWS * ROW), y: 46, width: x(972) - x(ROWS * ROW), height: 38, class: "ghost" });
    lab.el(svg, "text", { x: (x(ROWS * ROW) + x(972)) / 2, y: 69, class: "mid small t-faint", text: "not the table" });
    for (let k = 0; k < ROWS; k++) {
        lab.el(svg, "rect", { x: x(k * ROW), y: 46, width: ROW * PX, height: 38, class: "mem" });
        lab.el(svg, "rect", { x: x(k * ROW + FIELD), y: 46, width: FIELD_SIZE * PX, height: 38, class: "shader" });
        lab.el(svg, "text", { x: x(k * ROW) + 5, y: 69, class: "small", text: `row ${k}` });
    }
    const steps = lab.el(svg, "path", { class: "arrow" });
    const fieldStep = lab.el(svg, "path", { class: "arrow hot" });
    const load = lab.el(svg, "rect", { y: 42, width: FIELD_SIZE * PX, height: 46, rx: 2 });
    const zoomLeft = lab.el(svg, "line", { y1: 84, x2: ZX, y2: 126, class: "dashed" });
    const zoomRight = lab.el(svg, "line", { y1: 84, x2: ZX + ROW * ZPX, y2: 126, class: "dashed" });
    const zoomRow = lab.el(svg, "rect", { x: ZX, y: 126, width: ROW * ZPX, height: 40 });
    const zoomField = lab.el(svg, "rect", { x: ZX + FIELD * ZPX, y: 126, width: FIELD_SIZE * ZPX, height: 40, class: "shader" });
    const zoomFieldText = lab.el(svg, "text", { x: ZX + (FIELD + FIELD_SIZE / 2) * ZPX, y: 151, class: "mid title", text: "m" });
    const zoomName = lab.el(svg, "text", { x: ZX - 10, y: 143, class: "end small" });
    lab.el(svg, "text", { x: ZX - 10, y: 158, class: "end small", text: "zoomed" });
    lab.el(svg, "text", { x: ZX, y: 182, class: "mid mono small", text: "0" });
    lab.el(svg, "text", { x: ZX + FIELD * ZPX, y: 182, class: "mid mono small", text: FIELD });
    lab.el(svg, "text", { x: ZX + ROW * ZPX, y: 182, class: "mid mono small", text: ROW });
    const zoomLoad = lab.el(svg, "rect", { y: 121, width: FIELD_SIZE * ZPX, height: 50, rx: 3 });
    const zoomLoadText = lab.el(svg, "text", { y: 206, class: "mid small" });

    lab.draw(function () {
        const i = index.value;
        const stride = size.value;
        const offset = i * stride + FIELD;
        const row = Math.floor(offset / ROW);       // the real row the load starts in
        const local = offset - row * ROW;
        const inTable = row < ROWS;
        const right = inTable && row === i && local === FIELD;

        let path = "";
        for (let k = 0; k < i; k++) path += `M${x(k * stride).toFixed(1)},38V30H${x((k + 1) * stride).toFixed(1)}V38`;
        lab.set(steps, { d: path || "M0,0" });
        lab.set(fieldStep, { d: `M${x(i * stride).toFixed(1)},38V24H${x(offset).toFixed(1)}V38` });
        lab.set(stepText, { text: `${i} × ${stride} bytes, then ${FIELD} bytes to m` });
        lab.set(load, { x: x(offset).toFixed(1), class: `${right ? "ok" : "bad"} nofill thick` });

        lab.set(zoomLeft, { x1: x(row * ROW) });
        lab.set(zoomRight, { x1: x((row + 1) * ROW) });
        lab.set(zoomRow, { class: inTable ? "mem" : "ghost" });
        lab.set(zoomField, { class: inTable ? "shader" : "hide" });
        lab.set(zoomFieldText, { class: inTable ? "mid title" : "hide" });
        lab.set(zoomName, { text: inTable ? `row ${row},` : "after the table," });
        lab.set(zoomLoad, { x: ZX + local * ZPX, class: `${right ? "ok" : "bad"} nofill thick` });
        lab.set(zoomLoadText, {
            x: lab.clamp(ZX + (local + FIELD_SIZE / 2) * ZPX, 150, 610),
            class: `mid small ${right ? "t-ok" : "t-bad"}`,
            text: `the load: bytes ${local} to ${local + FIELD_SIZE}`,
        });

        offsetOut.set(`base + ${offset}`);
        addressOut.set(hex(BASE + offset), right ? "ok" : "bad");
        landOut.set(inTable ? `row ${row}, byte ${local}` : "after the table", right ? "ok" : "bad");
        if (right && stride === ROW) note.set(`Right: row ${i} starts ${i} × ${ROW} = ${i * ROW} bytes after the base, and m is ${FIELD} bytes into the row.`, "ok");
        else if (right) note.set("Right, but only by luck: 0 × any row size is 0. Every other row is wrong with this row size.", "ok");
        else if (stride === ROW) note.set(`Row ${i} does not exist: the table has ${ROWS} rows. The shader does not check. It loads the bytes after the table.`, "bad");
        else if (inTable) note.set(`Wrong row size: ${i} × ${stride} + ${FIELD} = ${offset} bytes is byte ${local} of row ${row}, not m of row ${i}. There is no error, only wrong values.`, "bad");
        else note.set(`Wrong row size, and ${i} × ${stride} + ${FIELD} = ${offset} bytes is past the end of the table (${ROWS * ROW} bytes).`, "bad");
    });
});

// Section 6: rows of one 8-byte pointer and one 4-byte count, packed one after another. A pointer may only
// start at a multiple of 8.
RasmLab.register("pointer-align", function (lab) {
    const svg = lab.svg(760, 150, "A byte ruler with a mark every 8 bytes, and rows of a pointer and a count on it. Pointers that start on a mark are aligned; the others are not.");
    const rowSize = lab.slider({ label: "Row size", min: 12, max: 24, step: 4, value: 12, format: function (v) { return `${v} bytes`; } });
    const first = lab.choice({ label: "First in the row", options: [{ value: "ptr", label: "the pointer" }, { value: "count", label: "the count" }], value: "ptr" });
    const alignedOut = lab.readout("pointers at a multiple of 8");
    const padOut = lab.readout("padding bytes per row");
    const note = lab.note();

    const X0 = 68;
    const PX = 6.5;
    const BYTES = 96;
    function x(bytes) { return X0 + bytes * PX; }

    let ruler = "";
    for (let b = 0; b <= BYTES; b += 8) {
        ruler += `M${x(b)},24V96`;
        lab.el(svg, "text", { x: x(b), y: 112, class: "mid mono small", text: b });
    }
    lab.el(svg, "path", { d: ruler, class: "dashed" });
    lab.el(svg, "text", { x: X0, y: 16, class: "small", text: "a mark every 8 bytes: a pointer may only start on a mark" });
    lab.el(svg, "text", { x: X0, y: 138, class: "small", text: "the number in a pointer is the byte where it starts; n is the count" });
    const rows = lab.el(svg, "g");

    lab.draw(function () {
        const size = rowSize.value;
        const pointerAt = first.value === "ptr" ? 0 : 4;
        const countAt = first.value === "ptr" ? 8 : 0;
        const count = Math.floor(BYTES / size);
        let aligned = 0;
        let firstBad = -1;
        lab.clear(rows);
        for (let k = 0; k < count; k++) {
            const start = k * size;
            const at = start + pointerAt;
            const good = at % 8 === 0;
            if (good) aligned += 1;
            else if (firstBad < 0) firstBad = k;
            lab.el(rows, "rect", { x: x(at), y: 40, width: 8 * PX, height: 40, class: good ? "ok" : "bad" });
            lab.el(rows, "text", { x: x(at + 4), y: 65, class: "mid mono", text: at });
            lab.el(rows, "rect", { x: x(start + countAt), y: 40, width: 4 * PX, height: 40, class: "box" });
            lab.el(rows, "text", { x: x(start + countAt + 2), y: 64, class: "mid small", text: "n" });
            if (size > 12) lab.el(rows, "rect", { x: x(start + 12), y: 40, width: (size - 12) * PX, height: 40, class: "ghost" });
            lab.el(rows, "line", { x1: x(start), y1: 32, x2: x(start), y2: 88, class: "arrow thick" });
        }

        alignedOut.set(`${aligned} / ${count}`, aligned === count ? "ok" : "bad");
        padOut.set(String(size - 12));
        if (aligned === count) note.set(`Aligned: a row is ${size} bytes, a multiple of 8, and the pointer is at the row's start. Every pointer starts on a mark.`, "ok");
        else if (pointerAt === 4 && size % 8 === 0) note.set("Misaligned in every row: the pointer starts 4 bytes into the row. Padding at the end of the row does not help; the order of the fields does.", "bad");
        else note.set(`Misaligned: the pointer of row ${firstBad} starts at byte ${firstBad * size + pointerAt}, and ${firstBad * size + pointerAt} is not a multiple of 8.`, "bad");
    });
});

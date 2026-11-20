// Labs of docs/guides/bitmap-text.html.

// The public-domain IBM VGA 8x8 font from '!' (33) to '~' (126), the same data as FONT in
// shaders/passes/statsOverlay.slang: two words per glyph, four rows each, the top row in the low byte,
// and in each row the lowest bit is the leftmost pixel.
const BITMAP_FONT = [
    0x183C3C18, 0x00180018, 0x00003636, 0x00000000, 0x367F3636, 0x0036367F, 0x1E033E0C, 0x000C1F30,
    0x18336300, 0x0063660C, 0x6E1C361C, 0x006E333B, 0x00030606, 0x00000000, 0x06060C18, 0x00180C06,
    0x18180C06, 0x00060C18, 0xFF3C6600, 0x0000663C, 0x3F0C0C00, 0x00000C0C, 0x00000000, 0x060C0C00,
    0x3F000000, 0x00000000, 0x00000000, 0x000C0C00, 0x0C183060, 0x00010306, 0x7B73633E, 0x003E676F,
    0x0C0C0E0C, 0x003F0C0C, 0x1C30331E, 0x003F3306, 0x1C30331E, 0x001E3330, 0x33363C38, 0x0078307F,
    0x301F033F, 0x001E3330, 0x1F03061C, 0x001E3333, 0x1830333F, 0x000C0C0C, 0x1E33331E, 0x001E3333,
    0x3E33331E, 0x000E1830, 0x000C0C00, 0x000C0C00, 0x000C0C00, 0x060C0C00, 0x03060C18, 0x00180C06,
    0x003F0000, 0x00003F00, 0x30180C06, 0x00060C18, 0x1830331E, 0x000C000C, 0x7B7B633E, 0x001E037B,
    0x33331E0C, 0x0033333F, 0x3E66663F, 0x003F6666, 0x0303663C, 0x003C6603, 0x6666361F, 0x001F3666,
    0x1E16467F, 0x007F4616, 0x1E16467F, 0x000F0616, 0x0303663C, 0x007C6673, 0x3F333333, 0x00333333,
    0x0C0C0C1E, 0x001E0C0C, 0x30303078, 0x001E3333, 0x1E366667, 0x00676636, 0x0606060F, 0x007F6646,
    0x7F7F7763, 0x0063636B, 0x7B6F6763, 0x00636373, 0x6363361C, 0x001C3663, 0x3E66663F, 0x000F0606,
    0x3333331E, 0x00381E3B, 0x3E66663F, 0x00676636, 0x0E07331E, 0x001E3338, 0x0C0C2D3F, 0x001E0C0C,
    0x33333333, 0x003F3333, 0x33333333, 0x000C1E33, 0x6B636363, 0x0063777F, 0x1C366363, 0x0063361C,
    0x1E333333, 0x001E0C0C, 0x1831637F, 0x007F664C, 0x0606061E, 0x001E0606, 0x180C0603, 0x00406030,
    0x1818181E, 0x001E1818, 0x63361C08, 0x00000000, 0x00000000, 0xFF000000, 0x00180C0C, 0x00000000,
    0x301E0000, 0x006E333E, 0x3E060607, 0x003B6666, 0x331E0000, 0x001E3303, 0x3E303038, 0x006E3333,
    0x331E0000, 0x001E033F, 0x0F06361C, 0x000F0606, 0x336E0000, 0x1F303E33, 0x6E360607, 0x00676666,
    0x0C0E000C, 0x001E0C0C, 0x30300030, 0x1E333330, 0x36660607, 0x0067361E, 0x0C0C0C0E, 0x001E0C0C,
    0x7F330000, 0x00636B7F, 0x331F0000, 0x00333333, 0x331E0000, 0x001E3333, 0x663B0000, 0x0F063E66,
    0x336E0000, 0x78303E33, 0x6E3B0000, 0x000F0666, 0x033E0000, 0x001F301E, 0x0C3E0C08, 0x00182C0C,
    0x33330000, 0x006E3333, 0x33330000, 0x000C1E33, 0x6B630000, 0x00367F7F, 0x36630000, 0x0063361C,
    0x33330000, 0x1F303E33, 0x193F0000, 0x003F260C, 0x070C0C38, 0x00380C0C, 0x00181818, 0x00181818,
    0x380C0C07, 0x00070C0C, 0x00003B6E, 0x00000000,
];
const FONT_FIRST = 33;
const FONT_LAST = 126;

// The lookup of the page: word, then row byte, then bit.
function bitmapFontBit(code, x, y) {
    const word = BITMAP_FONT[(code - FONT_FIRST) * 2 + Math.floor(y / 4)];
    const row = (word >>> ((y % 4) * 8)) & 0xFF;
    return (row >>> x) & 1;
}

function hexByte(v) {
    return "0x" + v.toString(16).toUpperCase().padStart(2, "0");
}

// An 8 x 8 glyph the reader edits by clicking pixels. The row bytes and the two packed words follow.
RasmLab.register("glyph-editor", function (lab) {
    const svg = lab.svg(760, 250, "An 8 by 8 grid of pixels to click, the byte of each row, and the two 32-bit words that the eight row bytes are packed into.");
    const start = lab.choice({
        label: "Start from",
        options: [{ value: "A", label: "A" }, { value: "P", label: "P" }, { value: "g", label: "g" }, { value: "8", label: "8" }, { value: "", label: "empty" }],
    });
    const word0Out = lab.readout("word 0: rows 0 to 3");
    const word1Out = lab.readout("word 1: rows 4 to 7");
    const countOut = lab.readout("pixels on, out of 64");
    const note = lab.note();

    const X0 = 60;
    const Y0 = 40;
    const CELL = 22;
    const rows = [0, 0, 0, 0, 0, 0, 0, 0];      // one byte per row, bit 0 = leftmost pixel
    let loaded = null;

    function load(letter) {
        for (let y = 0; y < 8; y++) {
            rows[y] = 0;
            for (let x = 0; x < 8; x++) if (letter && bitmapFontBit(letter.charCodeAt(0), x, y)) rows[y] |= 1 << x;
        }
        loaded = letter;
    }

    const cells = [];
    for (let y = 0; y < 8; y++) {
        for (let x = 0; x < 8; x++) {
            const cell = lab.el(svg, "rect", { x: X0 + x * CELL, y: Y0 + y * CELL, width: CELL, height: CELL });
            lab.clickable(cell, `Pixel ${x}, ${y}`, function () { rows[y] ^= 1 << x; });
            cells.push(cell);
        }
    }
    const byteTexts = [];
    for (let k = 0; k < 8; k++) {
        lab.el(svg, "text", { x: X0 + k * CELL + CELL / 2, y: 32, class: "mid mono small", text: String(k) });
        lab.el(svg, "text", { x: 50, y: Y0 + k * CELL + 16, class: "end mono small", text: String(k) });
        byteTexts.push(lab.el(svg, "text", { x: 248, y: Y0 + k * CELL + 16, class: "mono small" }));
    }
    lab.el(svg, "text", { x: 248, y: 32, class: "small", text: "byte" });
    lab.el(svg, "text", { x: 148, y: 238, class: "mid small", text: "x: bit 0 is the leftmost pixel" });

    lab.el(svg, "text", { x: 330, y: 20, class: "small", text: "a 32-bit word, written high byte first" });
    ["bits 31–24", "23–16", "15–8", "7–0"].forEach(function (label, k) {
        lab.el(svg, "text", { x: 366 + k * 72, y: 40, class: "mid small", text: label });
    });
    const boxTexts = [];
    const wordTexts = [];
    for (let w = 0; w < 2; w++) {
        const y = 48 + w * 96;
        for (let k = 0; k < 4; k++) {
            const row = w * 4 + 3 - k;
            lab.el(svg, "rect", { x: 330 + k * 72, y: y, width: 72, height: 46, class: "mem" });
            boxTexts[row] = lab.el(svg, "text", { x: 366 + k * 72, y: y + 20, class: "mid mono" });
            lab.el(svg, "text", { x: 366 + k * 72, y: y + 38, class: "mid small", text: `row ${row}` });
        }
        wordTexts.push(lab.el(svg, "text", { x: 330, y: y + 66, class: "mono" }));
    }
    lab.el(svg, "text", { x: 688, y: 40, class: "mid small", text: "three in a row" });
    lab.el(svg, "rect", { x: 640, y: 48, width: 96, height: 32, class: "paper" });
    const preview = lab.el(svg, "path", { class: "head" });

    lab.draw(function () {
        if (start.value !== loaded) load(start.value);
        let count = 0;
        let previewPath = "";
        for (let y = 0; y < 8; y++) {
            for (let x = 0; x < 8; x++) {
                const on = (rows[y] >> x) & 1;
                lab.set(cells[y * 8 + x], { class: on ? "solid pick" : "paper pick", "aria-pressed": on ? "true" : "false" });
                if (!on) continue;
                count += 1;
                for (let k = 0; k < 3; k++) previewPath += `M${640 + k * 32 + x * 4},${48 + y * 4}h4v4h-4z`;
            }
            lab.set(byteTexts[y], { text: hexByte(rows[y]) });
            lab.set(boxTexts[y], { text: hexByte(rows[y]) });
        }
        lab.set(preview, { d: previewPath || "M0,0" });

        // word 0 = row0 | row1 << 8 | row2 << 16 | row3 << 24, and the same for rows 4 to 7
        const words = [0, 1].map(function (w) {
            const value = (rows[w * 4] | rows[w * 4 + 1] << 8 | rows[w * 4 + 2] << 16 | rows[w * 4 + 3] << 24) >>> 0;
            return "0x" + value.toString(16).toUpperCase().padStart(8, "0");
        });
        lab.set(wordTexts[0], { text: `word 0 = ${words[0]}` });
        lab.set(wordTexts[1], { text: `word 1 = ${words[1]}` });
        word0Out.set(words[0], "mem");
        word1Out.set(words[1], "mem");
        countOut.set(String(count));

        let differs = 0;
        if (loaded) {
            for (let y = 0; y < 8; y++) {
                for (let x = 0; x < 8; x++) if (((rows[y] >> x) & 1) !== bitmapFontBit(loaded.charCodeAt(0), x, y)) differs += 1;
            }
        }
        let touches = rows[7] !== 0;
        for (let y = 0; y < 8; y++) if (rows[y] & 0x80) touches = true;
        if (touches) note.set("Pixels in column 7 or row 7 reach the edge of the cell. Drawn edge to edge, this glyph touches the next one.", "bad");
        else if (loaded && differs === 0) note.set(`This is the '${loaded}' of the font. Its two words are exactly the two integers stored for it.`, "ok");
        else if (count === 0) note.set("An empty glyph: both words are 0. A space needs no glyph for this reason.");
        else note.set(`Your own glyph${loaded ? `: ${differs} ${differs === 1 ? "pixel differs" : "pixels differ"} from the font's '${loaded}'` : ""}. Each click changes one bit of one byte of one word.`);
    });
});

// The panel of the worked example, drawn by running the fragment lookup for every screen pixel.
// The reader drags one pixel and sees each step of its lookup.
RasmLab.register("pixel-lookup", function (lab) {
    const svg = lab.svg(760, 236, "The top-left corner of a window with the text panel, drawn pixel by pixel. One pixel is marked, and its character's glyph is shown enlarged with the bit that the pixel reads.");
    const scaleChoice = lab.choice({ label: "Scale s", options: [{ value: 1, label: "1" }, { value: 2, label: "2" }], value: 2 });
    const cellOut = lab.readout("cell (column, row) and its character");
    const glyphOut = lab.readout("pixel inside the glyph");
    const bitOut = lab.readout("the bit");
    const note = lab.note();

    const LINES = ["Frame 16.67 ms", "GPU 1.31 ms   "];
    const COLUMNS = 14;
    const ROWS = 2;
    const MARGIN = 8;       // screen pixels from the window corner to the panel
    const PADDING = 4;      // font pixels from the panel's edge to the text
    const SCREEN_W = 256;
    const SCREEN_H = 64;
    const X0 = 20;
    const Y0 = 30;
    const UNIT = 2;         // svg units per screen pixel
    const ZX = 580;
    const ZY = 34;
    const ZCELL = 20;

    // What the fragment at pixel (px, py) does, step by step, like the list in the text.
    function lookup(px, py, s) {
        const r = { kind: "outside" };
        const width = (COLUMNS * 8 + 2 * PADDING) * s;
        const height = (ROWS * 8 + 2 * PADDING) * s;
        if (px < MARGIN || py < MARGIN || px >= MARGIN + width || py >= MARGIN + height) return r;
        const o = MARGIN + PADDING * s;
        r.o = o;
        r.t = [px - o, py - o];
        r.kind = "padding";
        if (r.t[0] < 0 || r.t[1] < 0) return r;
        r.f = [Math.floor(r.t[0] / s), Math.floor(r.t[1] / s)];
        r.cell = [Math.floor(r.f[0] / 8), Math.floor(r.f[1] / 8)];
        if (r.cell[0] >= COLUMNS || r.cell[1] >= ROWS) return r;
        r.g = [r.f[0] % 8, r.f[1] % 8];
        r.index = r.cell[1] * COLUMNS + r.cell[0];
        r.code = LINES[r.cell[1]].charCodeAt(r.cell[0]);
        r.kind = "space";
        if (r.code < FONT_FIRST || r.code > FONT_LAST) return r;
        r.lit = bitmapFontBit(r.code, r.g[0], r.g[1]);
        r.kind = r.lit ? "lit" : "dark";
        return r;
    }

    lab.el(svg, "text", { x: X0, y: 20, class: "small", text: "the window's corner, 256 × 64 screen pixels" });
    lab.el(svg, "rect", { x: X0, y: Y0, width: SCREEN_W * UNIT, height: SCREEN_H * UNIT, class: "zone" });
    const panel = lab.el(svg, "rect", { class: "box" });
    const textPath = lab.el(svg, "path", { class: "head" });
    const cellBox = lab.el(svg, "rect", { class: "hot" });
    const line1 = lab.el(svg, "text", { x: X0, y: 184, class: "mono small" });
    const line2 = lab.el(svg, "text", { x: X0, y: 204, class: "mono small" });
    const line3 = lab.el(svg, "text", { x: X0, y: 224, class: "mono small" });

    const zoomTitle = lab.el(svg, "text", { x: ZX + 80, y: 20, class: "mid small" });
    const zoomBits = lab.el(svg, "path", { class: "head" });
    let gridPath = "";
    for (let k = 1; k < 8; k++) gridPath += `M${ZX + k * ZCELL},${ZY}v160M${ZX},${ZY + k * ZCELL}h160`;
    lab.el(svg, "path", { d: gridPath, class: "grid" });
    lab.el(svg, "rect", { x: ZX, y: ZY, width: 160, height: 160, class: "ghost" });
    const zoomPick = lab.el(svg, "rect", { width: ZCELL, height: ZCELL, class: "hot" });
    const zoomNote = lab.el(svg, "text", { x: ZX + 80, y: 214, class: "mid small" });

    // pixel (34, 42) of the worked example
    const probe = lab.handle(svg, {
        x: X0 + 34 * UNIT + 1, y: Y0 + 42 * UNIT + 1, r: 6, label: "Move the pixel",
        minX: X0, maxX: X0 + SCREEN_W * UNIT - 1, minY: Y0, maxY: Y0 + SCREEN_H * UNIT - 1,
    });

    let drawnScale = 0;
    lab.draw(function () {
        const s = scaleChoice.value;
        if (s !== drawnScale) {
            // every screen pixel runs the lookup; lit pixels become runs of one path
            let d = "";
            for (let py = 0; py < SCREEN_H; py++) {
                let from = -1;
                for (let px = 0; px <= SCREEN_W; px++) {
                    const lit = px < SCREEN_W && lookup(px, py, s).kind === "lit";
                    if (lit && from < 0) from = px;
                    if (!lit && from >= 0) {
                        d += `M${X0 + from * UNIT},${Y0 + py * UNIT}h${(px - from) * UNIT}v${UNIT}h${-(px - from) * UNIT}z`;
                        from = -1;
                    }
                }
            }
            lab.set(textPath, { d: d });
            lab.set(panel, { x: X0 + MARGIN * UNIT, y: Y0 + MARGIN * UNIT, width: (COLUMNS * 8 + 2 * PADDING) * s * UNIT, height: (ROWS * 8 + 2 * PADDING) * s * UNIT });
            drawnScale = s;
        }

        const px = lab.clamp(Math.floor((probe.x - X0) / UNIT), 0, SCREEN_W - 1);
        const py = lab.clamp(Math.floor((probe.y - Y0) / UNIT), 0, SCREEN_H - 1);
        const r = lookup(px, py, s);

        lab.set(line1, { text: r.t ? `t = p − o = (${px}, ${py}) − (${r.o}, ${r.o}) = (${r.t[0]}, ${r.t[1]})` : `p = (${px}, ${py}): outside the quad` });
        lab.set(line2, { text: r.cell ? `f = t / ${s} = (${r.f[0]}, ${r.f[1]})   cell = f / 8 = (${r.cell[0]}, ${r.cell[1]})` : "" });
        lab.set(line3, { text: r.g ? `g = f % 8 = (${r.g[0]}, ${r.g[1]})   index = ${r.cell[1]} · ${COLUMNS} + ${r.cell[0]} = ${r.index}` : "" });

        const hasGlyph = r.kind === "lit" || r.kind === "dark";
        if (r.g) {
            const o = MARGIN + PADDING * s;
            lab.set(cellBox, { class: "hot", x: X0 + (o + r.cell[0] * 8 * s) * UNIT, y: Y0 + (o + r.cell[1] * 8 * s) * UNIT, width: 8 * s * UNIT, height: 8 * s * UNIT });
            lab.set(zoomPick, { class: "hot", x: ZX + r.g[0] * ZCELL, y: ZY + r.g[1] * ZCELL });
        } else {
            lab.set(cellBox, { class: "hide" });
            lab.set(zoomPick, { class: "hide" });
        }
        let bits = "";
        if (hasGlyph) {
            for (let y = 0; y < 8; y++) {
                for (let x = 0; x < 8; x++) if (bitmapFontBit(r.code, x, y)) bits += `M${ZX + x * ZCELL},${ZY + y * ZCELL}h${ZCELL}v${ZCELL}h${-ZCELL}z`;
            }
        }
        lab.set(zoomBits, { d: bits || "M0,0" });
        lab.set(zoomTitle, { text: hasGlyph ? `the glyph of '${String.fromCharCode(r.code)}' (code ${r.code})` : (r.kind === "space" ? "code 32, the space: no glyph" : "no glyph is read") });
        lab.set(zoomNote, { text: hasGlyph ? `bit ${r.g[0]} of row ${r.g[1]} is ${r.lit}` : "" });

        const name = r.g ? (r.code === 32 ? "space" : `'${String.fromCharCode(r.code)}'`) : "";
        cellOut.set(r.g ? `(${r.cell[0]}, ${r.cell[1]}) ${name}` : "–");
        glyphOut.set(r.g ? `(${r.g[0]}, ${r.g[1]})` : "–");
        bitOut.set(hasGlyph ? String(r.lit) : "–", r.kind === "lit" ? "ok" : "");

        if (r.kind === "outside") note.set(`Pixel (${px}, ${py}) is outside the quad. No fragment is made there, so the shader does not run.`);
        else if (!r.cell) note.set(`Pixel (${px}, ${py}) is in the padding left of or above the text: t is negative, so it gets the background color.`);
        else if (!r.g) note.set(`Pixel (${px}, ${py}) is in the padding after the last column or row: the cell is outside the grid, so it gets the background color.`);
        else if (r.kind === "space") note.set(`Pixel (${px}, ${py}) is in a cell that holds a space. Code 32 is outside the font, so it gets the background color.`);
        else if (r.lit) note.set(`Pixel (${px}, ${py}) reads bit ${r.g[0]} of row ${r.g[1]} of '${String.fromCharCode(r.code)}'. The bit is 1: text color.`, "ok");
        else note.set(`Pixel (${px}, ${py}) reads bit ${r.g[0]} of row ${r.g[1]} of '${String.fromCharCode(r.code)}'. The bit is 0: background color.`);
    });
});

// Labs of docs/guides/texture-compression.html.

// The format choice of section 6, the same lists as chooseKtxFormat in src/assets/ktxImage.cpp: per use, the formats
// best first, and the first one the GPU samples wins. The memory is the real one: whole 4x4 blocks per level, every
// level down to 1 x 1.
RasmLab.register("pick-format", function (lab) {
    const svg = lab.svg(760, 250, "For the chosen use, its list of formats best first, each marked as sampled by the GPU or not; the chosen one is highlighted. Below, bars compare the texture's memory with its mips in the chosen format and in RGBA8.");
    const GPUS = {
        desktop: { label: "desktop (BC)", has: ["BC7", "BC5", "BC4"] },
        apple: { label: "Apple Silicon (BC, ASTC)", has: ["BC7", "BC5", "BC4", "ASTC 4×4"] },
        mobile: { label: "mobile (ASTC, ETC2)", has: ["ASTC 4×4", "ETC2 RGBA8", "EAC R11", "EAC RG11"] },
        old: { label: "old GPU (none)", has: [] },
    };
    const USES = {
        color: { label: "color", list: ["BC7", "ASTC 4×4", "ETC2 RGBA8", "RGBA8"], srgb: true },
        data: { label: "data, RGBA", list: ["BC7", "ASTC 4×4", "ETC2 RGBA8", "RGBA8"], srgb: false },
        normal: { label: "normal map, RG", list: ["BC5", "EAC RG11", "RGBA8"], srgb: false },
        mask: { label: "mask, R", list: ["BC4", "EAC R11", "RGBA8"], srgb: false },
    };
    // Bytes per block, and the block's side in texels.
    const BLOCK = {
        "BC7": [16, 4], "BC5": [16, 4], "BC4": [8, 4], "ASTC 4×4": [16, 4], "ETC2 RGBA8": [16, 4],
        "EAC RG11": [16, 4], "EAC R11": [8, 4], "RGBA8": [4, 1],
    };
    const gpu = lab.choice({
        label: "GPU",
        options: Object.keys(GPUS).map(function (key) { return { value: key, label: GPUS[key].label }; }),
        value: "desktop",
    });
    const use = lab.choice({
        label: "Use",
        options: Object.keys(USES).map(function (key) { return { value: key, label: USES[key].label }; }),
        value: "color",
    });
    const size = lab.slider({ label: "Texture size", min: 8, max: 12, step: 1, value: 11, format: function (v) { return `${1 << v} × ${1 << v}`; } });
    const formatOut = lab.readout("chosen format");
    const memoryOut = lab.readout("with mips");
    const savedOut = lab.readout("against RGBA8");
    const note = lab.note();

    const MIB = 1024 * 1024;

    function bytesWithMips(format, side) {
        const block = BLOCK[format];
        let total = 0;
        for (let s = side; ; s = Math.max(1, s >> 1)) {
            const blocks = Math.ceil(s / block[1]);
            total += blocks * blocks * block[0];
            if (s === 1) break;
        }
        return total;
    }
    function mib(bytes) {
        return bytes / MIB >= 10 ? (bytes / MIB).toFixed(1) : (bytes / MIB).toFixed(2);
    }

    const listGroup = lab.el(svg, "g", {});
    const barGroup = lab.el(svg, "g", {});
    lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "the use's list, best first" });
    lab.el(svg, "text", { x: 20, y: 150, class: "title", text: "memory, with every mip" });

    lab.draw(function () {
        const g = GPUS[gpu.value];
        const u = USES[use.value];
        const side = 1 << size.value;
        lab.clear(listGroup);
        lab.clear(barGroup);

        let chosen = null;
        u.list.forEach(function (format, i) {
            const has = format === "RGBA8" || g.has.indexOf(format) >= 0;
            const pick = has && chosen === null;
            if (pick) chosen = format;
            const x = 20 + i * 180;
            lab.el(listGroup, "rect", { x: x, y: 40, width: 160, height: 50, rx: 6, class: pick ? "gpu" : has ? "box" : "ghost" });
            const name = format + (u.srgb && format !== "BC5" && format !== "BC4" ? " sRGB" : "");
            lab.el(listGroup, "text", { x: x + 80, y: 62, class: "mid" + (pick ? " title" : ""), text: name });
            lab.el(listGroup, "text", { x: x + 80, y: 80, class: "mid small " + (has ? "t-ok" : "t-bad"), text: pick ? "chosen" : has ? "sampled, not needed" : "not on this GPU" });
            if (i + 1 < u.list.length) lab.el(listGroup, "line", { x1: x + 162, y1: 65, x2: x + 176, y2: 65, class: "arrow", "marker-end": "url(#ah)" });
        });

        const rgba = bytesWithMips("RGBA8", side);
        const picked = bytesWithMips(chosen, side);
        const full = 560;
        [["RGBA8", rgba, "mem"], [chosen, picked, chosen === "RGBA8" ? "mem" : "gpu"]].forEach(function (row, i) {
            const y = 166 + i * 38;
            const w = Math.max(2, full * row[1] / rgba);
            lab.el(barGroup, "rect", { x: 150, y: y, width: w, height: 26, class: row[2] });
            lab.el(barGroup, "text", { x: 140, y: y + 18, class: "end small", text: row[0] });
            lab.el(barGroup, "text", { x: 150 + w + 8 > 700 ? 150 + w - 8 : 150 + w + 8, y: y + 18, class: (150 + w + 8 > 700 ? "end " : "") + "small mono", text: `${mib(row[1])} MiB` });
        });

        formatOut.set(chosen + (u.srgb && chosen !== "BC5" && chosen !== "BC4" ? " sRGB" : ""), chosen === "RGBA8" ? "bad" : "ok");
        memoryOut.set(`${mib(picked)} MiB`, "gpu");
        savedOut.set(`${(rgba / picked).toFixed(1)}× smaller`, chosen === "RGBA8" ? "bad" : "ok");
        if (chosen === "RGBA8")
            note.set("This GPU samples none of the block formats for this use, so the texture is transcoded to RGBA8: it works, at four times the memory of BC7.", "bad");
        else if (u.list[0] !== chosen)
            note.set(`The first choice is missing on this GPU, so ${chosen} is used: same size per texel, another family.`, "ok");
        else
            note.set(`The best format for this use is on this GPU: ${chosen}, ${(rgba / picked).toFixed(0)}× smaller than RGBA8.`, "ok");
    });
});

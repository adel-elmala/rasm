// Labs of docs/guides/shader-caching.html.

// Six cache entries: a top file, an entry point, its defines, and the files it includes. One change to the inputs,
// then the next start looks up each entry by its key. An entry whose real inputs changed must be compiled again; one
// whose key did not change is read from the cache. Read with changed inputs, it is stale: the bug a missing input
// causes. The three keys: the top file and the entry point only; every input the compiler read (a manifest); every
// input, with the whole include folder instead of the files each entry read.
RasmLab.register("cache-keys", function (lab) {
    const ENTRIES = [
        { file: "scene.slang", entry: "vertMain", defines: "", includes: ["common", "lighting"] },
        { file: "scene.slang", entry: "fragMain", defines: "", includes: ["common", "lighting"] },
        { file: "scene.slang", entry: "fragMain", defines: "MASK", includes: ["common", "lighting"] },
        { file: "shadow.slang", entry: "vertMain", defines: "", includes: ["common"] },
        { file: "cull.slang", entry: "main", defines: "", includes: ["common"] },
        { file: "post.slang", entry: "main", defines: "", includes: [] },
    ];
    const CHANGES = [
        { value: "none", label: "nothing" },
        { value: "lighting", label: "edit lighting.slang" },
        { value: "common", label: "edit common.slang" },
        { value: "scene", label: "edit scene.slang" },
        { value: "define", label: "MASK gets a define" },
        { value: "compiler", label: "new compiler" },
        { value: "struct", label: "a CPU struct" },
    ];

    const svg = lab.svg(760, 300, "Six cache entries as rows: top file, entry point, defines and included files. The last column says what the next start does with each: read it from the cache, compile it, or read stale code.");
    const header = ["top file", "entry point", "defines", "includes", "next start"];
    const COLUMNS = [20, 150, 270, 360, 560];
    header.forEach(function (text, i) {
        lab.el(svg, "text", { x: COLUMNS[i], y: 26, class: "small title", text: text });
    });
    const rows = ENTRIES.map(function (item, i) {
        const y = 44 + i * 40;
        const back = lab.el(svg, "rect", { x: 12, y: y, width: 736, height: 34, rx: 5, class: "paper" });
        const file = lab.el(svg, "text", { x: COLUMNS[0], y: y + 22, class: "small mono", text: item.file });
        lab.el(svg, "text", { x: COLUMNS[1], y: y + 22, class: "small mono", text: item.entry });
        const defines = lab.el(svg, "text", { x: COLUMNS[2], y: y + 22, class: "small mono", text: item.defines || "-" });
        const includes = lab.el(svg, "text", { x: COLUMNS[3], y: y + 22, class: "small mono" });
        const names = {};
        if (item.includes.length === 0) lab.el(includes, "tspan", { text: "-" });
        item.includes.forEach(function (name, n) {
            if (n > 0) lab.el(includes, "tspan", { text: ", " });
            names[name] = lab.el(includes, "tspan", { text: name });
        });
        const status = lab.el(svg, "rect", { x: COLUMNS[4] - 8, y: y + 5, width: 186, height: 24, rx: 4 });
        const label = lab.el(svg, "text", { x: COLUMNS[4] + 85, y: y + 22, class: "mid small" });
        return { back: back, file: file, defines: defines, names: names, status: status, label: label };
    });

    const change = lab.choice({ label: "change", options: CHANGES, value: "lighting" });
    const keyMode = lab.choice({
        label: "the key holds",
        options: [
            { value: "top", label: "top file only" },
            { value: "manifest", label: "every input read" },
            { value: "folder", label: "whole include folder" },
        ],
        value: "top",
    });
    const compiledOut = lab.readout("compiled");
    const staleOut = lab.readout("stale hits");
    const note = lab.note();

    // Did the change touch an input of this entry's code?
    function inputChanged(item, what) {
        if (what === "lighting" || what === "common") return item.includes.indexOf(what) >= 0;
        if (what === "scene") return item.file === "scene.slang";
        if (what === "define") return item.defines === "MASK";
        return what === "compiler";
    }

    // Did the change touch something the key holds?
    function keyChanged(item, what, mode) {
        if (mode === "top") return what === "scene" && item.file === "scene.slang";
        if (mode === "manifest") return inputChanged(item, what);
        return inputChanged(item, what) || what === "lighting" || what === "common";
    }

    lab.draw(function () {
        let compiled = 0;
        let stale = 0;
        ENTRIES.forEach(function (item, i) {
            const real = inputChanged(item, change.value);
            const missed = keyChanged(item, change.value, keyMode.value);
            const row = rows[i];
            let text = "read from the cache";
            let cls = "ok o-50";
            if (missed) {
                text = real ? "compiled" : "compiled, same code";
                cls = "gpu o-50";
                compiled += 1;
            } else if (real) {
                text = "stale: old code";
                cls = "bad o-50";
                stale += 1;
            } else if (change.value === "struct") {
                text = "read, layouts checked";
            }
            lab.set(row.status, { class: cls });
            lab.set(row.label, { text: text });
            lab.set(row.back, { class: real ? "zone" : "paper" });
            lab.set(row.file, { class: "small mono" + (change.value === "scene" && item.file === "scene.slang" ? " t-hot" : "") });
            lab.set(row.defines, { text: item.defines ? item.defines + (change.value === "define" ? " + MOTION" : "") : "-", class: "small mono" + (change.value === "define" && item.defines ? " t-hot" : "") });
            Object.keys(row.names).forEach(function (name) {
                lab.set(row.names[name], { class: name === change.value ? "t-hot" : "" });
            });
        });
        compiledOut.set(String(compiled), "gpu");
        staleOut.set(String(stale), stale > 0 ? "bad" : "ok");

        if (stale > 0)
            note.set("Wrong: " + stale + " entries changed but kept their key, so the start reads old code. The edit does not show, with no error.", "bad");
        else if (change.value === "struct")
            note.set("Right: a CPU struct is not an input of the SPIR-V, so every entry is read. The stored layouts are checked against the new struct, so a mismatch is still caught.", "ok");
        else if (change.value === "none")
            note.set("Nothing changed: every entry is read from the cache.", "ok");
        else if (keyMode.value === "folder" && compiled > ENTRIES.filter(function (item) { return inputChanged(item, change.value); }).length)
            note.set("Right, at a cost: the whole folder is in every key, so entries that never read the edited file are compiled too.", "ok");
        else
            note.set("Right: exactly the entries whose inputs changed are compiled again.", "ok");
    });
});

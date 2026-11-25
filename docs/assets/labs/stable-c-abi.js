// Labs of docs/guides/stable-c-abi.html.

// A config struct over three minor versions: 1.0 has a and b, 1.1 adds c, 1.2 adds d. The program passes the struct
// of its header with size = sizeof; the library reads up to that size and treats the rest as the call kind says.
RasmLab.register("struct-versions", function (lab) {
    const svg = lab.svg(760, 236, "A struct over three versions: the fields the program sends, and what the library does with each field of its own struct.");
    const FIELDS = [
        { name: "size", since: 0, bytes: 4 },
        { name: "a", since: 0, bytes: 4 },
        { name: "b", since: 0, bytes: 4 },
        { name: "c", since: 1, bytes: 4 },
        { name: "d", since: 2, bytes: 4 },
    ];
    const VERSIONS = ["1.0", "1.1", "1.2"];
    const X = 150;
    const W = 110;

    const program = lab.choice({ label: "Program built with", options: VERSIONS, value: "1.0" });
    const library = lab.choice({ label: "Library", options: VERSIONS, value: "1.2" });
    const kind = lab.choice({ label: "Call", options: ["create", "update", "get"], value: "create" });
    const sizeOut = lab.readout("size the program sets");
    const resultOut = lab.readout("result");
    const note = lab.note();

    lab.el(svg, "text", { x: 20, y: 46, class: "title", text: "program" });
    lab.el(svg, "text", { x: 20, y: 62, class: "small", text: "its struct" });
    lab.el(svg, "text", { x: 20, y: 146, class: "title", text: "library" });
    lab.el(svg, "text", { x: 20, y: 162, class: "small", text: "its struct" });

    const top = [];
    const bottom = [];
    FIELDS.forEach(function (field, i) {
        const x = X + i * W;
        top.push({
            rect: lab.el(svg, "rect", { x: x, y: 30, width: W - 6, height: 36 }),
            text: lab.el(svg, "text", { x: x + (W - 6) / 2, y: 53, class: "mid mono" }),
        });
        bottom.push({
            rect: lab.el(svg, "rect", { x: x, y: 130, width: W - 6, height: 36 }),
            text: lab.el(svg, "text", { x: x + (W - 6) / 2, y: 153, class: "mid mono" }),
            arrow: lab.el(svg, "line", { x1: x + (W - 6) / 2, y1: 68, x2: x + (W - 6) / 2, y2: 126 }),
            what: lab.el(svg, "text", { x: x + (W - 6) / 2, y: 186, class: "mid small" }),
        });
    });
    const footer = lab.el(svg, "text", { x: 380, y: 222, class: "mid small" });

    lab.draw(function () {
        const p = VERSIONS.indexOf(program.value);
        const l = VERSIONS.indexOf(library.value);
        const size = FIELDS.filter(function (f) { return f.since <= p; }).reduce(function (s, f) { return s + f.bytes; }, 0);
        // Only a creation call checks the version; the others run on an object that was created, so they see a
        // program the library accepted.
        const refused = p > l;
        sizeOut.set(`${size} bytes`);

        FIELDS.forEach(function (field, i) {
            const sent = field.since <= p;
            const known = field.since <= l;
            lab.set(top[i].rect, { class: sent ? "mem" : "hide" });
            lab.set(top[i].text, { text: sent ? (i === 0 ? `size=${size}` : field.name) : "" });
            lab.set(bottom[i].rect, { class: known ? (refused ? "ghost" : "cpu") : "hide" });
            lab.set(bottom[i].text, { text: known ? field.name : "" });
            let arrow = "hide";
            let what = "";
            let whatClass = "mid small";
            if (refused) {
                if (sent && !known) {
                    what = "unknown here";
                    whatClass = "mid small t-bad";
                }
            } else if (i === 0) {
                what = "the program's";
            } else if (known && sent) {
                arrow = "arrow ok";
                what = kind.value === "get" ? "written back" : "read";
                whatClass = "mid small t-ok";
            } else if (known) {
                what = kind.value === "create" ? "its default" : kind.value === "update" ? "kept as it is" : "not written";
                whatClass = "mid small t-gpu";
            }
            lab.set(bottom[i].arrow, { class: arrow, "marker-end": arrow === "hide" ? "" : "url(#ah-ok)" });
            lab.set(bottom[i].what, { class: whatClass, text: what });
        });

        if (refused) {
            resultOut.set("RASM_ERROR_VERSION", "bad");
            lab.set(footer, { class: "mid small t-bad", text: `creation refuses: the program's minor version ${p} is newer than the library's ${l}` });
            note.set("The program may set fields or call functions this library does not have, so creation stops it with a clear error instead of ignoring them.", "bad");
        } else {
            resultOut.set("RASM_OK", "ok");
            const missing = FIELDS.filter(function (f) { return f.since > p && f.since <= l; }).map(function (f) { return f.name; });
            lab.set(footer, { class: "mid small", text: `the library reads ${size} bytes and never touches past them` });
            if (missing.length === 0)
                note.set("Program and library know the same fields: every field comes from the program.", "ok");
            else if (kind.value === "create")
                note.set(`An older program: ${missing.join(" and ")} take their defaults, as if it had called the init function of its own version.`, "ok");
            else if (kind.value === "update")
                note.set(`An update keeps ${missing.join(" and ")}: an old program does not reset a setting it never knew.`, "ok");
            else
                note.set(`A get writes only ${size} bytes, so the program's smaller struct is never overrun.`, "ok");
        }
    });
});

// Labs of docs/guides/library-copies.html.

// Two copies of Dear ImGui 1.92.6 (64-bit) and one context, made by copy A. Each copy places the context's members by
// its own headers: ImGuiIO is 3000 bytes, or 3032 with the old fields (IMGUI_DISABLE_OBSOLETE_FUNCTIONS off), and
// every member after it moves with it. Copy B reads one member at the offset its headers give; the picture shows what
// copy A keeps there, and what the version and size check says.
RasmLab.register("copy-layout", function (lab) {
    const svg = lab.svg(760, 250, "Two bars: the start of the context as copy A lays it out, and as copy B does. An arrow goes from the member copy B reads down to what copy A keeps at that offset.");
    const oldA = lab.toggle({ label: "copy A: old fields", value: true });
    const oldB = lab.toggle({ label: "copy B: old fields", value: false });
    const field = lab.choice({ label: "B reads", options: ["IO.DisplaySize", "PlatformIO", "Style", "Windows"], value: "Style" });
    const readOut = lab.readout("B reads at byte");
    const keptOut = lab.readout("A keeps there");
    const checkOut = lab.readout("the check");
    const note = lab.note();

    const X0 = 100;
    const SCALE = 640 / 5400;
    const HEADER = 40;
    const PLATFORM_IO = 96;
    const STYLE = 1296;
    const FONTS_AND_MORE = 680;
    const WINDOWS = 16;

    // The members of the context from byte 0, as one copy's headers place them.
    function layout(oldFields) {
        const io = 3000 + (oldFields ? 32 : 0);
        const members = [
            { name: "start", size: HEADER, cls: "paper" },
            { name: "IO", size: io, cls: "cpu" },
            { name: "PlatformIO", size: PLATFORM_IO, cls: "mem" },
            { name: "Style", size: STYLE, cls: "gpu" },
            { name: "fonts, …", size: FONTS_AND_MORE, cls: "paper" },
            { name: "Windows", size: WINDOWS, cls: "accent" },
        ];
        let offset = 0;
        for (const m of members) {
            m.offset = offset;
            offset += m.size;
        }
        return { io: io, members: members };
    }

    function find(l, name) {
        return l.members.find(function (m) { return m.name === name; });
    }

    // The offset of what B reads: DisplaySize is 8 bytes into ImGuiIO, the others are whole members.
    function offsetOf(l, name) {
        if (name === "IO.DisplaySize") return find(l, "IO").offset + 8;
        return find(l, name).offset;
    }

    function memberAt(l, offset) {
        for (const m of l.members) {
            if (offset >= m.offset && offset < m.offset + m.size) return { member: m, inside: offset - m.offset };
        }
        return null;
    }

    lab.el(svg, "text", { x: 10, y: 20, class: "small t-faint", text: "bytes from the start of the context →" });
    lab.el(svg, "text", { x: 10, y: 62, class: "title", text: "copy A" });
    lab.el(svg, "text", { x: 10, y: 176, class: "title", text: "copy B" });
    const barA = lab.el(svg, "g");
    const barB = lab.el(svg, "g");
    const arrow = lab.el(svg, "path", { d: "", class: "arrow", "marker-end": "url(#ah)" });
    const label = lab.el(svg, "text", { x: 0, y: 122, class: "small", text: "" });
    const label2 = lab.el(svg, "text", { x: 0, y: 136, class: "small", text: "" });
    const scaleText = lab.el(svg, "text", { x: 745, y: 240, class: "end small t-faint", text: "the first 5400 of the context's bytes" });

    function drawBar(group, l, y) {
        lab.clear(group);
        for (const m of l.members) {
            const x = X0 + m.offset * SCALE;
            const w = Math.max(m.size * SCALE, 2);
            lab.el(group, "rect", { x: x, y: y, width: w, height: 34, class: m.cls });
            if (m.name.length * 5.5 + 8 <= w) lab.el(group, "text", { x: x + w / 2, y: y + 21, class: "mid small", text: m.name === "start" ? "" : m.name });
        }
        lab.el(group, "text", { x: X0, y: y + 50, class: "small t-faint", text: `ImGuiIO ${l.io} B, PlatformIO at ${find(l, "PlatformIO").offset}, Style at ${find(l, "Style").offset}` });
    }

    lab.draw(function () {
        const a = layout(oldA.value);
        const b = layout(oldB.value);
        drawBar(barA, a, 44);
        drawBar(barB, b, 158);
        lab.set(scaleText, { text: "the first 5400 of the context's bytes" });

        const offset = offsetOf(b, field.value);
        const at = memberAt(a, offset);
        const right = offset === offsetOf(a, field.value);
        const x = X0 + offset * SCALE;
        lab.set(arrow, { d: `M${x},156 V82`, class: right ? "arrow ok" : "arrow bad", "marker-end": right ? "url(#ah-ok)" : "url(#ah-bad)" });
        const textX = x > 520 ? x - 8 : x + 8;
        const anchor = x > 520 ? "end small" : "small";
        const kept = at ? (at.inside === 0 ? `${at.member.name}` : `${at.member.name} + ${at.inside} B`) : "past the end";
        lab.set(label, { x: textX, class: anchor + (right ? " t-ok" : " t-bad"), text: `B reads ${field.value} at ${offset}` });
        lab.set(label2, { x: textX, class: anchor + (right ? " t-ok" : " t-bad"), text: right ? "A keeps it there" : `A keeps ${kept} there` });

        readOut.set(`${offset}`);
        keptOut.set(kept, right ? "ok" : "bad");
        const same = a.io === b.io;
        checkOut.set(same ? "passes" : `ImGuiIO ${b.io} ≠ ${a.io}`, same ? "ok" : "bad");
        if (same) note.set("Both copies place every member at the same byte: copy B may use copy A's context.", "ok");
        else if (right) note.set(`${field.value} sits before the old fields, so this read is right, but every member after ImGuiIO is 32 bytes off. The check refuses the pair.`, "bad");
        else note.set(`Copy B reads ${kept} of copy A's context as its ${field.value}. The check stops this before the first call: ImGuiIO is ${b.io} bytes in B and ${a.io} in A.`, "bad");
    });
});

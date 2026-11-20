// Labs of docs/guides/vertex-pulling.html.

// The page's mega-buffer around mesh B: index buffer positions 32 to 45, vertices 18 to 31. The reader picks a
// corner of B's draw and sets the vertex offset; the lab follows firstIndex + corner, then index + offset, then
// vertex × 64 bytes, and says which mesh owns the vertex that is read.
RasmLab.register("pull-vertex", function (lab) {
    const svg = lab.svg(760, 292, "A part of the shared index buffer and of the shared vertex buffer. An arrow goes from the picked index of mesh B to the vertex that the shader reads with the chosen vertex offset.");
    const offset = lab.slider({ label: "vertexOffset", min: 20, max: 28, value: 24 });
    const indexOut = lab.readout("index buffer position: firstIndex + corner");
    const vertexOut = lab.readout("vertex: index value + vertexOffset");
    const byteOut = lab.readout("byte address: vertex × 64");
    const note = lab.note();

    const FIRST_INDEX = 36;         // mesh B's range of the index buffer
    const INDEX_COUNT = 6;
    const B_VERTICES = [24, 27];    // mesh B's range of the vertex buffer
    const STRIDE = 64;
    const INDEX_FROM = 32;
    const INDICES = [22, 20, 22, 23, 0, 1, 2, 0, 2, 3, 0, 1, 2, 2];     // A's last 4, B's 6, C's first 4
    const VERTEX_FROM = 18;
    const CELLS = 14;
    function cellX(k) { return 30 + k * 50; }
    function owner(vertex) { return vertex < B_VERTICES[0] ? "A" : (vertex > B_VERTICES[1] ? "C" : "B"); }

    let corner = 5;

    lab.el(svg, "text", { x: 20, y: 14, class: "title", text: "Index buffer" });
    lab.el(svg, "text", { x: cellX(2) - 2, y: 33, class: "mid small t-faint", text: "mesh A" });
    lab.el(svg, "text", { x: cellX(7) - 2, y: 33, class: "mid small t-mem", text: "mesh B: firstIndex 36, 6 indices" });
    lab.el(svg, "text", { x: cellX(12) - 2, y: 33, class: "mid small t-faint", text: "mesh C" });
    const indexPick = lab.el(svg, "rect", { y: 36, width: 54, height: 42, rx: 6, class: "hot" });
    INDICES.forEach(function (value, k) {
        const position = INDEX_FROM + k;
        const mine = position >= FIRST_INDEX && position < FIRST_INDEX + INDEX_COUNT;
        const g = lab.el(svg, "g");
        lab.el(g, "rect", { x: cellX(k), y: 40, width: 46, height: 34, rx: 3, class: mine ? "mem" : "box" });
        lab.el(g, "text", { x: cellX(k) + 23, y: 62, class: mine ? "mid mono" : "mid mono t-faint", text: String(value) });
        lab.el(svg, "text", { x: cellX(k) + 23, y: 88, class: "mid mono small t-faint", text: String(position) });
        if (mine) lab.clickable(g, `Corner ${position - FIRST_INDEX} of the draw of mesh B`, function () { corner = position - FIRST_INDEX; });
    });

    const mathA = lab.el(svg, "text", { y: 128, class: "mono" });
    const mathB = lab.el(svg, "text", { y: 148, class: "mono" });
    const follow = lab.el(svg, "line", { class: "arrow ok", "marker-end": "url(#ah-ok)" });

    lab.el(svg, "text", { x: 20, y: 176, class: "title", text: "Vertex buffer" });
    lab.el(svg, "text", { x: cellX(3) - 2, y: 193, class: "mid small t-faint", text: "mesh A" });
    lab.el(svg, "text", { x: cellX(8) - 2, y: 193, class: "mid small t-mem", text: "mesh B: vertices 24 … 27" });
    lab.el(svg, "text", { x: cellX(12) - 2, y: 193, class: "mid small t-faint", text: "mesh C" });
    for (let k = 0; k < CELLS; k++) {
        const vertex = VERTEX_FROM + k;
        lab.el(svg, "rect", { x: cellX(k), y: 200, width: 46, height: 34, rx: 3, class: owner(vertex) === "B" ? "mem" : "box" });
    }
    const vertexPick = lab.el(svg, "rect", { y: 200, width: 46, height: 34, rx: 3 });
    for (let k = 0; k < CELLS; k++) {
        const vertex = VERTEX_FROM + k;
        lab.el(svg, "text", { x: cellX(k) + 23, y: 222, class: owner(vertex) === "B" ? "mid mono" : "mid mono t-faint", text: String(vertex) });
        lab.el(svg, "text", { x: cellX(k) + 23, y: 248, class: "mid mono small t-faint", text: String(vertex * STRIDE) });
    }
    const reach = lab.el(svg, "line", { y1: 260, y2: 260, class: "hot" });
    const reachText = lab.el(svg, "text", { y: 280, class: "mid small t-hot", text: "the 4 vertices this draw reads" });

    lab.draw(function () {
        const position = FIRST_INDEX + corner;                  // the hardware reads the index
        const value = INDICES[position - INDEX_FROM];
        const vertex = value + offset.value;                    // the draw adds the offset
        const good = owner(vertex) === "B";
        const tone = good ? "ok" : "bad";

        const fromX = cellX(position - INDEX_FROM) + 23;
        const toX = cellX(vertex - VERTEX_FROM) + 23;
        lab.set(indexPick, { x: fromX - 27 });
        lab.set(vertexPick, { x: toX - 23, class: tone });
        lab.set(follow, { x1: fromX, y1: 94, x2: toX, y2: 197, class: `arrow ${tone}`, "marker-end": `url(#ah-${tone})` });
        // the two lines of address math go on the side the arrow does not use
        const left = (fromX + toX) / 2 > 380;
        const side = { x: left ? 30 : 730, class: left ? "mono" : "mono end" };
        lab.set(mathA, { x: side.x, class: side.class, text: `index buffer[${FIRST_INDEX} + ${corner}] = ${value}` });
        lab.set(mathB, { x: side.x, class: side.class, text: `${value} + vertexOffset ${offset.value} = vertex ${vertex}` });
        const reachFrom = cellX(offset.value - VERTEX_FROM);
        lab.set(reach, { x1: reachFrom, x2: reachFrom + 196 });
        lab.set(reachText, { x: reachFrom + 98 });

        indexOut.set(`${FIRST_INDEX} + ${corner} = ${position}`);
        vertexOut.set(`${value} + ${offset.value} = ${vertex}`, tone);
        byteOut.set(String(vertex * STRIDE), tone);
        if (good && offset.value === B_VERTICES[0]) note.set(`Right: the offset is mesh B's first vertex, so corner ${corner} reads vertex ${vertex}, one of B's own.`, "ok");
        else if (good) note.set(`Wrong offset, but corner ${corner} still reads a vertex of B (${vertex}), not the one it should. Other corners leave the mesh.`, "bad");
        else note.set(`Wrong: vertex ${vertex} belongs to mesh ${owner(vertex)}. The draw of B shows a piece of ${owner(vertex)}, and nothing reports an error.`, "bad");
    });
});

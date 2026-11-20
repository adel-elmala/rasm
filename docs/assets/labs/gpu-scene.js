// Labs of docs/guides/gpu-scene.html.

// A small instance table: the CPU copy and the two frame-slot buffers. It follows GpuTable (setRow marks a row
// dirty in both slots, upload sends one slot's dirty rows as merged ranges) and the row allocation of syncGpuScene.
RasmLab.register("table-sync", function (lab) {
    const CAPACITY = 8;
    const SLOTS = 2;
    const CELL_X = 170;
    const CELL_STEP = 72;
    const CELL_W = 66;
    const CELL_H = 40;
    const ROW_Y = [24, 88, 152];

    const svg = lab.svg(760, 232, "An instance table of 8 rows, three times: the CPU copy, the buffer of frame slot 0 and the buffer of frame slot 1. Rows that a buffer has not received yet are marked dirty.");

    // ---- the state, with the names of the code
    const entities = [];            // { name, subMeshes, x, firstInstance, instanceCount }
    const waitingForMesh = [];      // indices into entities
    const shadow = [];              // the CPU copy: { entity, x } per row
    const buffers = [[], []];       // what each slot's buffer holds
    const dirtySlots = [];
    const dirtyRows = [[], []];
    let instanceCount = 0;
    let frameCount = 0;
    let frame = 1;
    let last = null;                // the last frame: { frame, slot, rows, ranges }
    let error = "";
    let hint = "";
    for (let row = 0; row < CAPACITY; row++) {
        shadow.push(null);
        buffers[0].push(null);
        buffers[1].push(null);
        dirtySlots.push(0);
    }

    function setRow(row, data) {
        shadow[row] = data;
        for (let i = 0; i < SLOTS; i++) {
            const bit = 1 << i;
            if ((dirtySlots[row] & bit) === 0) dirtyRows[i].push(row);
            dirtySlots[row] |= bit;
        }
    }

    // Sends the slot's dirty rows, neighbours merged into one range. Returns the rows and the range count.
    function upload(slot) {
        const rows = dirtyRows[slot].slice().sort(function (a, b) { return a - b; });
        let ranges = 0;
        rows.forEach(function (row, k) {
            if (k === 0 || row !== rows[k - 1] + 1) ranges += 1;
            buffers[slot][row] = shadow[row];
            dirtySlots[row] &= ~(1 << slot);
        });
        dirtyRows[slot] = [];
        return { rows: rows, ranges: ranges };
    }

    function writeInstanceRows(entity) {
        for (let i = 0; i < entity.instanceCount; i++) setRow(entity.firstInstance + i, { entity: entity.name, x: entity.x });
    }

    function createEntity() {
        const index = entities.length;
        entities.push({ name: `e${index + 1}`, subMeshes: index % 2 === 0 ? 2 : 1, x: 0, firstInstance: 0, instanceCount: 0 });
        waitingForMesh.push(index);
        error = "";
        hint = "";
    }

    function updateTransform(row) {
        error = "";
        hint = "";
        if (!shadow[row]) {
            hint = `Row ${row} is empty: no entity owns it yet.`;
            return;
        }
        const entity = entities.filter(function (e) { return e.name === shadow[row].entity; })[0];
        entity.x += 1;
        writeInstanceRows(entity);
    }

    function syncGpuScene() {
        while (waitingForMesh.length) {
            const entity = entities[waitingForMesh.shift()];
            if (entity.subMeshes > CAPACITY - instanceCount) {
                error = `The table is full: ${entity.name} needs ${entity.subMeshes} ${entity.subMeshes === 1 ? "row" : "rows"}, and ${instanceCount} of ${CAPACITY} are in use. It logs an error and is never drawn.`;
                continue;
            }
            entity.firstInstance = instanceCount;
            entity.instanceCount = entity.subMeshes;
            instanceCount += entity.subMeshes;
            writeInstanceRows(entity);
        }
        return upload(frameCount);
    }

    function nextFrame() {
        error = "";
        hint = "";
        const sent = syncGpuScene();
        last = { frame: frame, slot: frameCount, rows: sent.rows, ranges: sent.ranges };
        frameCount = (frameCount + 1) % SLOTS;
        frame += 1;
    }

    // two entities that both slots already hold
    createEntity();
    createEntity();
    nextFrame();
    nextFrame();

    // ---- the picture
    lab.el(svg, "text", { x: 10, y: ROW_Y[0] + 18, class: "title", text: "CPU copy" });
    lab.el(svg, "text", { x: 10, y: ROW_Y[0] + 33, class: "small", text: "click a row to move it" });
    const status = [];
    for (let i = 0; i < SLOTS; i++) {
        lab.el(svg, "text", { x: 10, y: ROW_Y[i + 1] + 18, class: "title", text: `Instance Table ${i}` });
        status.push(lab.el(svg, "text", { x: 10, y: ROW_Y[i + 1] + 33, class: "small" }));
    }
    const cells = [];               // cells[line][row] = { box, name, value }
    for (let line = 0; line < 3; line++) {
        cells.push([]);
        for (let row = 0; row < CAPACITY; row++) {
            const x = CELL_X + row * CELL_STEP;
            if (line === 0) lab.el(svg, "text", { x: x + CELL_W / 2, y: 16, class: "mid small", text: `row ${row}` });
            const box = lab.el(svg, "rect", { x: x, y: ROW_Y[line], width: CELL_W, height: CELL_H, rx: 4 });
            const name = lab.el(svg, "text", { x: x + CELL_W / 2, y: ROW_Y[line] + 17, class: "mid mono" });
            const value = lab.el(svg, "text", { x: x + CELL_W / 2, y: ROW_Y[line] + 33, class: "mid small" });
            cells[line].push({ box: box, name: name, value: value });
        }
    }
    cells[0].forEach(function (cell, row) {
        lab.clickable(cell.box, `Move the entity of row ${row} (updateTransform)`, function () { updateTransform(row); });
    });
    const waitingText = lab.el(svg, "text", { x: 10, y: 220, class: "mono" });
    const countText = lab.el(svg, "text", { x: 740, y: 220, class: "end mono" });

    lab.button("createEntity", createEntity);
    lab.button("Next frame", nextFrame);
    const frameOut = lab.readout("the next frame, and its slot (frameCount)");
    const sentOut = lab.readout("uploaded by the last frame");
    const dirtyOut = lab.readout("dirty rows: slot 0 / slot 1");
    const note = lab.note();

    function rowList(rows) {
        const parts = [];
        rows.forEach(function (row, k) {
            if (k > 0 && row === rows[k - 1] + 1) parts[parts.length - 1].to = row;
            else parts.push({ from: row, to: row });
        });
        const text = parts.map(function (p) { return p.from === p.to ? String(p.from) : `${p.from}–${p.to}`; }).join(", ");
        return `${rows.length === 1 ? "Row" : "Rows"} ${text}`;
    }

    lab.draw(function () {
        for (let row = 0; row < CAPACITY; row++) {
            const cpu = cells[0][row];
            lab.set(cpu.box, { class: shadow[row] ? "cpu pick" : "zone pick" });
            lab.set(cpu.name, { text: shadow[row] ? shadow[row].entity : "" });
            lab.set(cpu.value, { text: shadow[row] ? `x = ${shadow[row].x}` : "" });
            for (let i = 0; i < SLOTS; i++) {
                const cell = cells[i + 1][row];
                const held = buffers[i][row];
                const dirty = (dirtySlots[row] & (1 << i)) !== 0;
                const fresh = last.slot === i && last.rows.indexOf(row) >= 0;
                lab.set(cell.box, { class: dirty ? "accent" : (fresh ? "ok" : (held ? "mem" : "zone")) });
                lab.set(cell.name, { text: held ? held.entity : (dirty ? "–" : "") });
                lab.set(cell.value, { text: held ? `x = ${held.x}` : (dirty ? "no data" : "") });
            }
        }
        for (let i = 0; i < SLOTS; i++) {
            const next = i === frameCount;
            lab.set(status[i], { class: next ? "small t-hot" : "small t-gpu", text: next ? `frame ${frame} writes it next` : `frame ${last.frame} drew from it` });
        }
        const waiting = waitingForMesh.map(function (index) { return entities[index].name; });
        lab.set(waitingText, { text: `entitiesWaitingForMesh: ${waiting.length ? waiting.join(", ") : "empty"}` });
        lab.set(countText, { text: `instanceCount = ${instanceCount} of ${CAPACITY}` });

        const both = [];
        const one = [];
        for (let row = 0; row < CAPACITY; row++) {
            if (dirtySlots[row] === 3) both.push(row);
            else if (dirtySlots[row] !== 0) one.push(row);
        }
        frameOut.set(`${frame} · slot ${frameCount}`);
        sentOut.set(`${last.rows.length} ${last.rows.length === 1 ? "row" : "rows"} · ${last.ranges} fillBuffer`, last.rows.length ? "ok" : "");
        dirtyOut.set(`${dirtyRows[0].length} / ${dirtyRows[1].length}`, both.length + one.length ? "" : "ok");

        if (error) note.set(error, "bad");
        else if (hint) note.set(hint);
        else if (waiting.length) note.set(`${waiting.join(", ")} ${waiting.length === 1 ? "waits" : "wait"} for rows. The next syncGpuScene gives rows to an entity whose mesh is ready, and writes them with setRow.`);
        else if (both.length) note.set(`${rowList(both)} changed on the CPU, and both buffers still hold the old data. Each slot uploads in its own frame, so it takes two frames.`);
        else if (one.length) note.set(`${rowList(one)}: slot ${frameCount} still holds the old data. That is safe, because frame ${frame} uploads it before it draws from slot ${frameCount}.`);
        else note.set("Clean: both buffers equal the CPU copy. A frame uploads nothing now.", "ok");
    });
});

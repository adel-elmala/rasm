// Labs of docs/guides/gpu-sync.html.

// One offscreen color target and the three passes that use it in every frame. The reader places the image
// barriers. The lab tracks the layout and the uses that no barrier has ordered yet, over repeated frames.
RasmLab.register("layout-chain", function (lab) {
    const svg = lab.svg(760, 232, "Three passes in a row use one color target: a scene pass draws into it, a blur pass and a post pass sample it. A barrier slot sits before each pass. Under each pass is the layout the target has while the pass runs.");
    const STEPS = [
        { name: "scene pass", does: "draws into the target", write: true, need: "COLOR_ATTACHMENT" },
        { name: "blur pass", does: "samples the target", write: false, need: "SHADER_READ_ONLY" },
        { name: "post pass", does: "samples the target", write: false, need: "SHADER_READ_ONLY" },
    ];
    const LAYOUT_CLASS = { UNDEFINED: "ghost", COLOR_ATTACHMENT: "gpu", SHADER_READ_ONLY: "shader" };
    const HAZARD_TEXT = {
        RAW: "It may also sample before the draw is finished: a read after write.",
        WAR: "It may also draw while the last frame's passes still sample the target: a write after read.",
        WAW: "It may also draw while the last frame's draw still runs: a write after write.",
    };
    const barriers = [true, true, false];

    const countOut = lab.readout("barriers per frame");
    const layoutOut = lab.readout("passes that find a wrong layout");
    const hazardOut = lab.readout("hazards: uses that nothing ordered");
    const note = lab.note();

    lab.el(svg, "text", { x: 70, y: 108, class: "small", text: "its layout during the pass:" });
    lab.el(svg, "text", { x: 20, y: 222, class: "small", text: "Every frame runs the same three passes, so the first barrier comes after the post pass of the frame before." });
    const parts = STEPS.map(function (step, i) {
        const x = 70 + i * 240;
        const slotX = 30 + i * 240;
        const part = {
            slot: lab.el(svg, "rect", { x: slotX, y: 30, width: 22, height: 124, rx: 3 }),
            slotLabel: lab.el(svg, "text", { x: slotX + 11, y: 20 }),
            pass: lab.el(svg, "rect", { x: x, y: 30, width: 180, height: 52, rx: 6 }),
            layout: lab.el(svg, "rect", { x: x, y: 116, width: 180, height: 38, rx: 6 }),
            layoutText: lab.el(svg, "text", { x: x + 90, y: 140, class: "mid mono" }),
            verdict: lab.el(svg, "text", { x: x + 90, y: 194 }),
        };
        lab.el(svg, "text", { x: x + 90, y: 52, class: "mid", text: step.name });
        lab.el(svg, "text", { x: x + 90, y: 70, class: "mid small", text: step.does });
        lab.el(svg, "text", { x: x + 90, y: 174, class: "mid small", text: `needs ${step.need}` });
        lab.clickable(part.slot, `Barrier before the ${step.name}`, function () { barriers[i] = !barriers[i]; });
        return part;
    });

    lab.draw(function () {
        // run the frame three times, so the first pass sees what the frame before left behind
        let layout = "UNDEFINED";
        let openWrite = false;      // a write that no barrier has waited for yet
        let openRead = false;
        let results = [];
        for (let frame = 0; frame < 3; frame++) {
            results = STEPS.map(function (step, i) {
                let useless = false;
                if (barriers[i]) {
                    useless = layout === step.need && !openWrite && !(step.write && openRead);
                    layout = step.need;
                    openWrite = false;
                    openRead = false;
                }
                let hazard = "";
                if (openWrite) hazard = step.write ? "WAW" : "RAW";
                else if (step.write && openRead) hazard = "WAR";
                if (step.write) openWrite = true;
                else openRead = true;
                return { layout: layout, wrong: layout !== step.need, hazard: hazard, useless: useless };
            });
        }

        let wrong = 0;
        let hazards = 0;
        let useless = -1;
        let firstBad = -1;
        results.forEach(function (r, i) {
            const bad = r.wrong || r.hazard;
            if (r.wrong) wrong += 1;
            if (r.hazard) hazards += 1;
            if (bad && firstBad < 0) firstBad = i;
            if (r.useless) useless = i;
            lab.set(parts[i].slot, { class: barriers[i] ? "accent pick" : "ghost pick" });
            lab.set(parts[i].slotLabel, {
                class: `mid small ${barriers[i] && !r.useless ? "t-hot" : "t-faint"}`,
                text: barriers[i] ? (r.useless ? "not needed" : "barrier") : "no barrier",
            });
            lab.set(parts[i].pass, { class: bad ? "bad" : "ok" });
            lab.set(parts[i].layout, { class: LAYOUT_CLASS[r.layout] });
            lab.set(parts[i].layoutText, { text: r.layout });
            lab.set(parts[i].verdict, {
                class: `mid small ${bad ? "t-bad" : "t-ok"}`,
                text: bad ? [r.wrong ? "wrong layout" : "", r.hazard ? `${r.hazard} hazard` : ""].filter(Boolean).join(" · ") : "layout and order are right",
            });
        });

        countOut.set(String(barriers.filter(Boolean).length));
        layoutOut.set(String(wrong), wrong ? "bad" : "ok");
        hazardOut.set(String(hazards), hazards ? "bad" : "ok");
        if (firstBad >= 0) {
            const r = results[firstBad];
            const step = STEPS[firstBad];
            let text = r.wrong ? `The ${step.name} finds the target in ${r.layout}, but it needs ${step.need}. ` : `The ${step.name} finds the right layout. `;
            if (r.hazard) text += HAZARD_TEXT[r.hazard].replace("It may also", r.wrong ? "It may also" : "But it may");
            note.set(text, "bad");
        } else if (useless >= 0) {
            note.set(`Correct, but the barrier before the ${STEPS[useless].name} is not needed: the layout is already right, and a read after a read is no hazard.`, "ok");
        } else {
            note.set("Correct with two barriers: one before the draw, which waits for the last frame's samples, and one between the draw and the first sample.", "ok");
        }
    });
});

// Labs of docs/guides/vulkan-loader.html.

// A program that uses one Vulkan 1.4 command (vkCopyMemoryToImage), needs instance version 1.3 for its instance-level
// features, and needs a GPU with Vulkan 1.4. It gets its functions one of two ways: linked against the loader's import
// library, or loaded at run time with a device table. The lab walks its start-up and stops where the page's rules
// stop it.
RasmLab.register("vulkan-start", function (lab) {
    const STEPS = ["OS loads .exe", "main()", "open the loader", "create instance", "pick a 1.4 GPU", "draw frames"];
    const BOX_W = 105;
    const BOX_STEP = 121;
    const svg = lab.svg(760, 190, "The start-up of a Vulkan program as six steps in a row. Steps it passed are filled, the step where it stops is red, and the steps after it are faint. Under them, the path a draw call takes.");
    const boxes = STEPS.map(function (name, i) {
        const x = 20 + i * BOX_STEP;
        const rect = lab.el(svg, "rect", { x: x, y: 30, width: BOX_W, height: 56, rx: 6 });
        lab.el(svg, "text", { x: x + BOX_W / 2, y: 62, class: "mid small", text: name });
        if (i > 0) lab.el(svg, "line", { x1: x - 16, y1: 58, x2: x - 2, y2: 58, class: "arrow", "marker-end": "url(#ah)" });
        return rect;
    });
    const why = lab.el(svg, "text", { x: 380, y: 114, class: "mid small" });
    lab.el(svg, "text", { x: 20, y: 156, class: "small t-faint", text: "a draw call:" });
    const path = lab.el(svg, "g", {});

    const linking = lab.choice({
        label: "the program gets Vulkan",
        options: [
            { value: "linked", label: "linked (import library)" },
            { value: "loaded", label: "loaded at run time" },
        ],
        value: "loaded",
    });
    const loader = lab.choice({ label: "loader on the machine", options: ["1.2", "1.3", "1.4"], value: "1.3" });
    const gpu = lab.choice({ label: "GPU driver", options: ["1.3", "1.4"], value: "1.4" });
    const resultOut = lab.readout("result");
    const hopsOut = lab.readout("jumps per draw call");
    const note = lab.note();

    // The step the program stops at (STEPS.length when it runs), and why.
    function startUp(linked, loaderVersion, gpuVersion) {
        if (linked && loaderVersion < 1.4)
            return { step: 0, why: `The OS finds no vkCopyMemoryToImage in a ${loaderVersion} loader and stops the program.` };
        if (!linked && loaderVersion < 1.3)
            return { step: 2, why: `The program reads the loader's version, ${loaderVersion}, and says it needs 1.3.` };
        if (gpuVersion < 1.4)
            return { step: 4, why: `The GPU's driver has Vulkan ${gpuVersion}; the program names it and says it needs 1.4.` };
        return { step: STEPS.length, why: "" };
    }

    function drawPath(linked) {
        lab.clear(path);
        const hops = linked ? ["program", "loader trampoline", "layers?", "driver"] : ["program", "device table", "layers?", "driver"];
        hops.forEach(function (name, i) {
            const x = 110 + i * 160;
            const trampoline = name === "loader trampoline";
            lab.el(path, "rect", { x: x, y: 138, width: 130, height: 28, rx: 5, class: trampoline ? "hot o-50" : name === "driver" ? "gpu" : "box" });
            lab.el(path, "text", { x: x + 65, y: 156, class: "mid small", text: name });
            if (i > 0) lab.el(path, "line", { x1: x - 28, y1: 152, x2: x - 4, y2: 152, class: "arrow", "marker-end": "url(#ah)" });
        });
    }

    lab.draw(function () {
        const linked = linking.value === "linked";
        const result = startUp(linked, Number(loader.value), Number(gpu.value));
        boxes.forEach(function (rect, i) {
            lab.set(rect, { class: i < result.step ? "zone" : i === result.step ? "bad o-50" : "ghost" });
        });
        lab.set(why, { text: result.why, class: "mid small t-bad" });
        drawPath(linked);

        const runs = result.step === STEPS.length;
        resultOut.set(runs ? "runs" : result.step === 0 ? "stopped before main" : "clear error", runs ? "ok" : "bad");
        hopsOut.set(linked ? "2: trampoline, then the driver" : "1: straight to the driver", linked ? "bad" : "ok");
        if (result.step === 0)
            note.set("Wrong: the program never reaches main, so it cannot say what is missing. The user sees only a system error.", "bad");
        else if (!runs)
            note.set("Right: the program started and said what to update. That is the best it can do on this machine.", "ok");
        else if (linked)
            note.set("It runs, but only because the loader is new enough, and every device call pays the trampoline.", "bad");
        else if (loader.value === "1.3")
            note.set("Right: a 1.3 loader is enough. Vulkan 1.4 added only device commands, and they come from the driver.", "ok");
        else
            note.set("Right: it runs, and each device call goes straight from the device table to the driver.", "ok");
    });
});

// Labs of docs/guides/big-picture.html.

// Follow one call of the application down the layers, one step per layer, as the code does it.
RasmLab.register("layer-calls", function (lab) {
    const LAYERS = ["Application", "Engine", "RenderContext", "rhi::Device", "VulkanDevice", "GPU"];
    const CLASSES = ["box", "cpu", "cpu", "cpu", "cpu", "gpu"];
    const BOX_X = 60;
    const BOX_W = 170;
    const BOX_H = 38;
    const PITCH = 58;
    const TEXT_X = 250;
    function top(layer) { return 8 + layer * PITCH; }

    // one step per layer the call enters: via = what crosses the boundary into it
    const CALLS = {
        createEntity: {
            steps: [
                { code: "engine.createEntity(\"crate\", mesh, material, transform)", text: "The application asks for an entity. It names the mesh and the material by handle." },
                { via: "handles", code: "handleManager.getNextEntityHandle()", text: "Stores the Entity in registry.entities. queueInstanceRows puts it on a waiting list." },
            ],
            back: "EntityHandle",
            gpu: "none",
            end: "The call stops at Engine: no GPU object is made. syncGpuScene writes the entity's rows in a later beginFrame.",
        },
        createBuffer: {
            steps: [
                { code: "engine.createBuffer({.name = \"Particle Positions\", ...})", text: "The application describes the buffer in a BufferDesc." },
                { via: "BufferDesc", code: "ctx.createBuffer(desc)", text: "Engine keeps nothing for a buffer. It passes the descriptor on." },
                { via: "BufferDesc", code: "impl->device->createBuffer(desc)", text: "No dedup: always a new buffer. Its table maps a new BufferHandle to the rhi::Buffer*." },
                { via: "BufferDesc", code: "virtual Buffer* createBuffer(const BufferDesc& desc) = 0", text: "The interface only. It names no Vulkan type." },
                { via: "virtual call", code: "vmaCreateBuffer(allocator, ...)", text: "VMA makes the VkBuffer and its memory. A VulkanBuffer goes back up as an rhi::Buffer*." },
            ],
            back: "BufferHandle",
            gpu: "none: memory only",
            end: "The call goes down to VulkanDevice and comes back at once. The pointer stays in RenderContext; the application gets the handle.",
        },
        render: {
            steps: [
                { code: "engine.render(scene, camera)", text: "Once per frame. Follow one of its draws." },
                { via: "handles", code: "renderer->render(...)  →  graph.execute(camera)", text: "The graph runs beginFrame, each pass and endFrame. A scene pass calls drawSceneView." },
                { via: "handles", code: "ctx.drawIndexedIndirectCount(commandBuffer, drawInfoBuffer, ...)", text: "Finds the rhi object behind each handle: lookup(impl->buffers, drawInfoBuffer, ...)." },
                { via: "rhi:: pointers", code: "rhiCommandBuffer->drawIndexedIndirectCount(rhiBuffer, ...)", text: "A virtual call on rhi::CommandBuffer, declared in src/rhi/device.h." },
                { via: "virtual call", code: "vkCmdDrawIndexedIndirectCount(commandBuffer, ...)", text: "VulkanCommandBuffer records the Vulkan command. Nothing is drawn yet." },
                { via: "Vulkan calls", code: "vkQueueSubmit2(queue, ...)", text: "endFrame submits the command buffer (ctx.submit). Only now the GPU runs the draws." },
            ],
            back: "nothing (void)",
            gpu: "the whole frame",
            end: "The call crosses every layer. Recording is CPU work; the GPU starts when endFrame submits, and the frame is presented.",
        },
    };

    const svg = lab.svg(760, 346, "The six layers as a column of boxes. The chosen call moves down one layer per step, with the line of code that runs in each layer.");
    const call = lab.choice({ label: "Call", options: ["createEntity", "createBuffer", "render"], value: "createEntity" });
    let step = 0;
    let shown = call.value;
    lab.button("‹ Back", function () { step = Math.max(0, step - 1); });
    lab.button("Next step ›", function () {
        const count = CALLS[call.value].steps.length;
        step = step >= count ? 0 : step + 1;
    });
    const stepOut = lab.readout("layers the call has entered");
    const backOut = lab.readout("what the application gets back");
    const gpuOut = lab.readout("GPU work in this call");
    const note = lab.note();

    const boxes = LAYERS.map(function (name, layer) {
        const box = lab.el(svg, "rect", { x: BOX_X, y: top(layer), width: BOX_W, height: BOX_H, rx: 6 });
        const label = lab.el(svg, "text", { x: BOX_X + BOX_W / 2, y: top(layer) + 24, class: "mid title", text: name });
        return { box: box, label: label };
    });
    const moving = lab.el(svg, "g");

    lab.draw(function () {
        if (call.value !== shown) {
            shown = call.value;
            step = 0;
        }
        const path = CALLS[call.value];
        const count = path.steps.length;
        const done = step >= count;                    // the call has returned
        const reached = Math.min(step, count - 1);     // the deepest layer entered so far

        boxes.forEach(function (b, layer) {
            lab.set(b.box, { class: CLASSES[layer] + (layer > reached ? " off" : "") });
            lab.set(b.label, { class: "mid title" + (layer > reached ? " t-faint" : "") });
        });

        lab.clear(moving);
        for (let layer = 0; layer <= reached; layer++) {
            const s = path.steps[layer];
            const y = top(layer);
            if (layer > 0) {
                const gpu = layer === LAYERS.length - 1;
                lab.el(moving, "line", { x1: BOX_X + 40, y1: y - PITCH + BOX_H, x2: BOX_X + 40, y2: y - 2, class: gpu ? "arrow gpu" : "arrow cpu", "marker-end": gpu ? "url(#ah-gpu)" : "url(#ah-cpu)" });
                lab.el(moving, "text", { x: BOX_X + 50, y: y - 6, class: "small", text: s.via });
            }
            lab.el(moving, "text", { x: TEXT_X, y: y + 15, class: "mono", text: s.code });
            lab.el(moving, "text", { x: TEXT_X, y: y + 32, class: "small", text: s.text });
        }
        if (done) {
            lab.el(moving, "path", { d: `M${BOX_X},${top(reached) + BOX_H / 2} H24 V${top(0) + BOX_H / 2} H${BOX_X - 2}`, class: "arrow ok thick", "marker-end": "url(#ah-ok)" });
        } else {
            lab.el(moving, "rect", { x: BOX_X - 3, y: top(reached) - 3, width: BOX_W + 6, height: BOX_H + 6, rx: 8, class: "hot" });
        }

        stepOut.set(`${reached + 1} of ${LAYERS.length}`, reached === LAYERS.length - 1 ? "gpu" : "cpu");
        backOut.set(done ? path.back : "…", done ? "ok" : "");
        gpuOut.set(path.gpu, path.gpu === "none" ? "" : (call.value === "render" ? "gpu" : ""));
        if (done) note.set(`${call.value} returns: ${path.back}. ${path.end}`, "ok");
        else if (step === 0) note.set(`The application calls ${call.value}. Press "Next step" to follow the call down.`);
        else note.set(`Now in ${LAYERS[reached]}. What crossed the boundary into it: ${path.steps[reached].via}.`);
    });
});

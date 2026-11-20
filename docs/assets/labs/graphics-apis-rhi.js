// Labs of docs/guides/graphics-apis-rhi.html.

// The picture of the shared model, with each part named in the words of one API. The names are the rows of the
// table in the same section.
RasmLab.register("api-names", function (lab) {
    const svg = lab.svg(760, 270, "The parts every graphics API has: device, resources, pipelines, bindings, command buffer, queue, fence and swapchain, each with its name in the chosen API.");
    const api = lab.choice({ label: "API", options: ["Vulkan", "Direct3D 12", "Metal", "WebGPU"], value: "Vulkan" });
    const shaderOut = lab.readout("shader format");
    const orderOut = lab.readout("who orders two uses of the same memory");
    const runsOut = lab.readout("runs on");
    const note = lab.note();

    const BOXES = [
        { key: "device", title: "Device", x: 20, y: 104, w: 140, cls: "cpu" },
        { key: "resources", title: "Resources", x: 196, y: 20, w: 200, cls: "mem" },
        { key: "pipeline", title: "Pipeline", x: 196, y: 104, w: 200, cls: "shader" },
        { key: "binding", title: "Binding", x: 196, y: 188, w: 200, cls: "box" },
        { key: "commands", title: "Command recording", x: 432, y: 104, w: 160, cls: "cpu" },
        { key: "ordering", title: "Ordering between uses", x: 432, y: 188, w: 160, cls: "cpu" },
        { key: "queue", title: "Queue", x: 616, y: 104, w: 132, cls: "gpu" },
        { key: "output", title: "Window output", x: 616, y: 20, w: 132, cls: "mem" },
        { key: "fence", title: "CPU waits for GPU", x: 616, y: 188, w: 132, cls: "mem" },
    ];
    const APIS = {
        "Vulkan": {
            device: ["VkDevice"], resources: ["VkBuffer, VkImage", "+ memory"], pipeline: ["VkPipeline", "+ pipeline layout"],
            binding: ["descriptor sets"], commands: ["VkCommandBuffer,", "from a VkCommandPool"], ordering: ["pipeline barriers,", "image layouts"],
            queue: ["VkQueue"], output: ["VkSwapchainKHR"], fence: ["VkFence,", "timeline semaphore"],
            shader: "SPIR-V", who: "the application", runs: "Windows, Linux, Android",
            note: "Vulkan: the application records every barrier and tracks the layout of every image.",
        },
        "Direct3D 12": {
            device: ["ID3D12Device"], resources: ["ID3D12Resource", "in heaps"], pipeline: ["pipeline state object", "+ root signature"],
            binding: ["descriptor heaps", "and tables"], commands: ["ID3D12GraphicsCommandList,", "from an allocator"], ordering: ["resource barriers,", "resource states"],
            queue: ["ID3D12CommandQueue"], output: ["DXGI swap chain"], fence: ["ID3D12Fence", "(a counter)"],
            shader: "DXIL", who: "the application", runs: "Windows, Xbox",
            note: "Direct3D 12: the application records every barrier and tracks the state of every resource.",
        },
        "Metal": {
            device: ["MTLDevice"], resources: ["MTLBuffer, MTLTexture"], pipeline: ["MTLRenderPipelineState"],
            binding: ["argument buffers,", "or direct set calls"], commands: ["MTLCommandBuffer", "with encoders"], ordering: ["automatic by default"],
            queue: ["MTLCommandQueue"], output: ["CAMetalLayer", "drawables"], fence: ["MTLSharedEvent,", "completion handlers"],
            shader: "MSL", who: "the driver", runs: "Apple only",
            note: "Metal: the driver orders the uses by itself, unless a resource opts out. A layer on top has no barriers to place here.",
        },
        "WebGPU": {
            device: ["GPUDevice"], resources: ["GPUBuffer, GPUTexture"], pipeline: ["GPURenderPipeline"],
            binding: ["bind groups"], commands: ["GPUCommandEncoder", "with pass encoders"], ordering: ["automatic, always"],
            queue: ["GPUQueue"], output: ["canvas context"], fence: ["onSubmittedWorkDone", "(a promise)"],
            shader: "WGSL", who: "the API", runs: "browsers, native libraries",
            note: "WebGPU: the API always orders the uses. A layer on top has no barriers to place here.",
        },
    };

    // the arrows of the page's figure
    [[160, 122, 194, 56], [160, 135, 194, 135], [160, 148, 194, 214]].forEach(function (p) {
        lab.el(svg, "line", { x1: p[0], y1: p[1], x2: p[2], y2: p[3], class: "arrow", "marker-end": "url(#ah)" });
    });
    [[396, 56, 430, 122], [396, 135, 430, 135], [396, 214, 430, 148]].forEach(function (p) {
        lab.el(svg, "line", { x1: p[0], y1: p[1], x2: p[2], y2: p[3], class: "arrow dashed" });
    });
    lab.el(svg, "line", { x1: 512, y1: 166, x2: 512, y2: 188, class: "arrow dashed" });
    lab.el(svg, "line", { x1: 592, y1: 135, x2: 614, y2: 135, class: "arrow gpu", "marker-end": "url(#ah-gpu)" });
    lab.el(svg, "line", { x1: 682, y1: 104, x2: 682, y2: 84, class: "arrow mem", "marker-end": "url(#ah-mem)" });
    lab.el(svg, "line", { x1: 682, y1: 166, x2: 682, y2: 186, class: "arrow mem", "marker-end": "url(#ah-mem)" });
    lab.el(svg, "text", { x: 20, y: 262, class: "small", text: "top line of a box: the shared part. Below it: the name in this API." });

    const parts = {};
    BOXES.forEach(function (b) {
        const rect = lab.el(svg, "rect", { x: b.x, y: b.y, width: b.w, height: 62, rx: 6, class: b.cls });
        lab.el(svg, "text", { x: b.x + b.w / 2, y: b.y + 20, class: "mid title", text: b.title });
        parts[b.key] = {
            rect: rect,
            lines: [lab.el(svg, "text", { x: b.x + b.w / 2, class: "mid small" }), lab.el(svg, "text", { x: b.x + b.w / 2, class: "mid small" })],
            y: b.y,
        };
    });

    lab.draw(function () {
        const names = APIS[api.value];
        BOXES.forEach(function (b) {
            const lines = names[b.key];
            const part = parts[b.key];
            lab.set(part.lines[0], { y: part.y + (lines.length === 1 ? 44 : 38), text: lines[0] });
            lab.set(part.lines[1], { y: part.y + 52, text: lines[1] || "" });
        });
        const byApp = names.who === "the application";
        lab.set(parts.ordering.rect, { class: byApp ? "cpu" : "box" });
        shaderOut.set(names.shader, "shader");
        orderOut.set(names.who, byApp ? "cpu" : "");
        runsOut.set(names.runs);
        note.set(names.note);
    });
});

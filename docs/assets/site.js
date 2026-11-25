// RASM docs: builds the sidebar (with its filter), the "on this page" list, the home page catalog, the prev/next
// links, the steppers, the diagram zoom and the copy buttons. Works from file:// (no fetch).
// A new page is added to SECTIONS and nowhere else. "blurb" is the one line shown on the home page.

const SECTIONS = [
    {
        title: "Start",
        pages: [
            { path: "index.html", title: "Home" },
        ],
    },
    {
        title: "Engine tour",
        guide: true,
        blurb: "How RASM fits together. Read these three first.",
        pages: [
            { path: "guides/big-picture.html",           title: "The big picture",                     blurb: "The layers from your app down to Vulkan, and who owns what." },
            { path: "guides/frame-loop.html",            title: "One frame, step by step",             blurb: "Everything that happens between two presents." },
            { path: "guides/gpu-scene.html",             title: "The GPU scene tables",                blurb: "How entities become rows the GPU reads." },
        ],
    },
    {
        title: "Guides: foundations",
        guide: true,
        blurb: "Ideas every explicit-API renderer is built on.",
        pages: [
            { path: "guides/graphics-apis-rhi.html",     title: "Graphics APIs and the RHI",           blurb: "What Vulkan, D3D12, Metal and WebGPU share, and the layer on top." },
            { path: "guides/vulkan-loader.html",         title: "The Vulkan loader",                   blurb: "How a call reaches the driver, and why RASM loads Vulkan at run time." },
            { path: "guides/shader-caching.html",        title: "Shader and pipeline caches",          blurb: "Keep compiled shaders and pipelines on disk, and know when they are stale." },
            { path: "guides/frames-in-flight.html",      title: "Frames in flight",                    blurb: "The CPU and the GPU work on different frames at once." },
            { path: "guides/swapchain.html",             title: "The swapchain and presenting",        blurb: "The ring of images that reaches the screen." },
            { path: "guides/headless-readback.html",     title: "Headless rendering and readback",     blurb: "Render with no window, and copy the pixels back to the CPU without a stall." },
            { path: "guides/platform-layer.html",        title: "The platform layer",                  blurb: "Draw into a window the application owns: native handles, points and pixels, input as events, and what the UI takes." },
            { path: "guides/stable-c-abi.html",          title: "A stable C ABI",                      blurb: "Offer a C++ library to C and other languages, and keep old programs working with new builds." },
            { path: "guides/gpu-sync.html",              title: "GPU synchronization: barriers, stages, layouts", blurb: "Why GPU work needs barriers, and what they say." },
            { path: "guides/generational-handles.html",  title: "Generational handles",                blurb: "Safe references: an index plus a generation." },
            { path: "guides/resource-lifetime.html",     title: "Deferred destruction",                blurb: "Destroy what the GPU may still read: the handle now, the memory later." },
            { path: "guides/job-system.html",            title: "Job systems and worker pools",        blurb: "A few threads, many jobs: split work, never wait inside a job, and cap the memory in flight." },
            { path: "guides/frame-allocations.html",     title: "Heap memory in the frame",            blurb: "What malloc and free cost, keep a container's capacity, arenas, free big blocks on a worker, and measure first." },
        ],
    },
    {
        title: "Guides: GPU data",
        guide: true,
        blurb: "Getting bytes to the GPU and reading them in shaders.",
        pages: [
            { path: "guides/gpu-memory.html",            title: "GPU memory and uploads",              blurb: "Heaps, staging buffers and batched copies." },
            { path: "guides/device-addresses.html",      title: "Buffer device addresses",             blurb: "GPU pointers instead of bindings." },
            { path: "guides/struct-layout.html",         title: "Struct layout rules",                 blurb: "How C++ and shaders agree on every byte." },
            { path: "guides/bindless.html",              title: "Bindless resources",                  blurb: "One big texture array, indexed in the shader." },
            { path: "guides/mipmaps-sampling.html",      title: "Mipmaps and texture sampling",        blurb: "Why small textures shimmer, and the mip chain that fixes it." },
            { path: "guides/texture-uploads.html",       title: "Texture uploads",                     blurb: "From a file to video memory: mips on a worker, staging or host image copy, shared memory blocks." },
            { path: "guides/color-spaces.html",          title: "Color spaces: sRGB and linear",       blurb: "Where to store color, and where to do the math." },
            { path: "guides/texture-compression.html",   title: "Texture compression: blocks, KTX2 and Basis", blurb: "4×4 blocks the GPU samples, and one file that becomes BC7, ASTC or ETC2." },
            { path: "guides/asset-streaming.html",       title: "Streaming assets in the background",  blurb: "Load meshes and textures without freezing the frame." },
            { path: "guides/tile-streaming.html",        title: "Tile streaming under a memory budget", blurb: "A tree of tiles, the screen-space error and the cut, parents that stand in, and a residency cache under a byte budget." },
            { path: "guides/transfer-queue.html",        title: "Transfer queues and ownership transfers", blurb: "Copies on the GPU's copy engine, next to the frames: use the data once its copy is done, release and acquire." },
            { path: "guides/mesh-simplification.html",   title: "Mesh simplification",                 blurb: "Building LODs by collapsing edges." },
        ],
    },
    {
        title: "Guides: drawing",
        guide: true,
        blurb: "From a vertex to a shaded pixel.",
        pages: [
            { path: "guides/gpu-driven.html",            title: "GPU-driven rendering and indirect draws", blurb: "The GPU builds its own draw list." },
            { path: "guides/vertex-pulling.html",        title: "Vertex pulling and mega-buffers",     blurb: "The shader reads vertices by index from one big buffer." },
            { path: "guides/uber-shaders.html",          title: "Uber shaders and shader permutations", blurb: "One shader with branches against many variants." },
            { path: "guides/pbr.html",                   title: "PBR shading",                         blurb: "The light math behind metal and roughness." },
            { path: "guides/transparency.html",          title: "Transparency and alpha modes",        blurb: "Cut-outs, see-through surfaces, blend order and see-through shadows." },
            { path: "guides/vector-rendering.html",      title: "Vector lines, polygons and points",   blurb: "Lines spread from their center line, joins and caps, edges smoothed by distance, polygons with holes, draping and culling for pixel widths." },
            { path: "guides/text-and-labels.html",       title: "Text and labels",                     blurb: "Glyphs as signed distances, a halo, text laid out from a font's metrics, labels anchored in the world and placed each frame by priority so they never overlap." },
            { path: "guides/reference-grid.html",       title: "Reference grids",                     blurb: "An endless grid from one triangle: a ray per pixel, lines a pixel wide, levels for any zoom, exact far from the origin." },
            { path: "guides/point-clouds.html",         title: "Point clouds",                        blurb: "Millions of points as splats: 16-bit positions in a chunk's box, a triangle a point, LOD by drawing a well-ordered prefix, colormaps." },
            { path: "guides/point-octree.html",         title: "Streaming point clouds",              blurb: "A cloud bigger than memory as an octree of samples: a cut by spacing on screen, nodes that fade in and out, points that grow where the sample is sparse, LAS and LAZ files." },
            { path: "guides/terrain.html",               title: "Terrain from height maps",            blurb: "One shared grid lifted by each tile's heights, skirts between levels, matching normals, and tiles bent onto a planet." },
            { path: "guides/image-based-lighting.html",  title: "Image-based lighting",                blurb: "Light from a photo of the whole sky: irradiance, prefiltered mips and the split sum." },
            { path: "guides/tone-mapping.html",          title: "Tone mapping and exposure",           blurb: "Bring unbounded scene light into the screen's 0 to 1." },
            { path: "guides/msaa.html",                  title: "MSAA and alpha-to-coverage",          blurb: "Several samples per pixel smooth edges; a weighted resolve keeps bright edges smooth too." },
            { path: "guides/taa.html",                   title: "Temporal anti-aliasing",              blurb: "One jittered sample per frame, blended over frames along motion vectors, without ghosts." },
            { path: "guides/bloom.html",                 title: "Bloom",                               blurb: "Light brighter than white spreads into a glow: blurs at many sizes from a chain of half-size images." },
            { path: "guides/shadow-mapping.html",        title: "Shadow mapping",                      blurb: "Depth from the light answers \"is this point lit?\"." },
            { path: "guides/cascaded-shadows.html",      title: "Cascaded shadow maps",                blurb: "One shadow map per slice of the view: sharp near, still there far." },
            { path: "guides/local-light-shadows.html",   title: "Shadows of spot and point lights",    blurb: "A perspective map along a cone, and a cube map of six faces." },
            { path: "guides/normal-mapping.html",        title: "Normal mapping and tangent space",    blurb: "Fine surface detail from a texture." },
            { path: "guides/coordinate-spaces.html",     title: "Coordinate spaces and the view matrix", blurb: "Object, world, view, clip, NDC, pixels." },
            { path: "guides/large-world-precision.html", title: "Large worlds: float precision",       blurb: "Doubles on the CPU, the camera at the origin, and a high/low split: a planet-sized world with no jitter." },
            { path: "guides/camera-controllers.html",    title: "Camera controllers and easing",       blurb: "Mouse and keys become a smooth camera." },
            { path: "guides/projection-depth.html",      title: "Projection and reversed-Z depth",     blurb: "The projection matrix and where depth precision goes." },
            { path: "guides/face-culling.html",          title: "Face culling and winding order",      blurb: "Skipping triangles that face away." },
            { path: "guides/keyframe-animation.html",    title: "Keyframe animation",                  blurb: "Keys, channels and the node tree: a model's parts move over time." },
            { path: "guides/skinning.html",              title: "Skinning",                            blurb: "Joints, weights and bind poses: a mesh that bends, skinned once per frame for every view." },
            { path: "guides/morph-targets.html",         title: "Morph targets",                       blurb: "Deltas and weights: a mesh that blends between shapes, morphed before skinning, once per frame." },
        ],
    },
    {
        title: "Guides: culling and the frame",
        guide: true,
        blurb: "Drawing less, and organizing the passes of a frame.",
        pages: [
            { path: "guides/frustum-culling.html",       title: "Frustum culling",                     blurb: "Skip what is outside the view." },
            { path: "guides/picking.html",               title: "Picking",                             blurb: "What is under a pixel: object ids drawn for a small rect through a narrow frustum, read back a few frames later, exact in a large world." },
            { path: "guides/clustered-lighting.html",    title: "Many lights: clustered lighting",     blurb: "Each pixel shades only the lights of its small box of the view." },
            { path: "guides/stream-compaction.html",     title: "Stream compaction",                   blurb: "Keep the survivors, packed and in order, on the GPU." },
            { path: "guides/gpu-sorting.html",           title: "Sorting on the GPU",                  blurb: "A bitonic network: thousands of threads, one compare each, in steps." },
            { path: "guides/occlusion-culling.html",     title: "Occlusion culling with Hi-Z",         blurb: "Skip what is hidden behind other things." },
            { path: "guides/lod-selection.html",         title: "LOD selection",                       blurb: "Pick the cheapest mesh nobody can tell apart." },
            { path: "guides/meshlets.html",              title: "Meshlets and cluster culling",        blurb: "Cut a big mesh into small pieces, and cull each piece." },
            { path: "guides/render-graphs.html",         title: "Render graphs",                       blurb: "Passes declare what they read and write; the graph does the rest." },
            { path: "guides/async-compute.html",         title: "Async compute and multiple queues",   blurb: "Compute work next to graphics work." },
            { path: "guides/memory-aliasing.html",       title: "Transient memory and aliasing",       blurb: "Render targets that share memory." },
            { path: "guides/load-store-ops.html",        title: "Load and store ops",                  blurb: "What a pass does with its targets at start and end." },
            { path: "guides/multiple-render-targets.html", title: "Multiple render targets",           blurb: "One pass, several output images." },
            { path: "guides/temporal-history.html",      title: "History buffers: last frame's image", blurb: "Reading what the last frame drew." },
            { path: "guides/multiple-views.html",        title: "Many views of one scene",             blurb: "Split screen, a map in the corner and render to texture: viewports, a target per view, and history kept by the view." },
            { path: "guides/gpu-timing.html",            title: "GPU timing and frame statistics",     blurb: "Measuring where a frame's time goes." },
            { path: "guides/bitmap-text.html",           title: "Text from a bitmap font",             blurb: "Debug text from bits packed into integers." },
            { path: "guides/immediate-mode-ui.html",     title: "Immediate-mode UI",                   blurb: "Windows and sliders as code each frame: draw lists, clip rectangles and an atlas." },
            { path: "guides/debug-views.html",           title: "Debug views",                         blurb: "Normals, UVs, overdraw, LODs, culling and light counts in place of the shading." },
            { path: "guides/inspecting-gpu-resources.html", title: "Inspecting GPU resources",         blurb: "Show a target, depth, Hi-Z or light clusters mid-frame, and GPU table rows from reflection." },
            { path: "guides/image-tests.html",           title: "Image tests: golden images",          blurb: "Compare each frame with a stored image, with a threshold for driver noise." },
        ],
    },
    {
        title: "API reference",
        pages: [
            { path: "api/index.html",                 title: "Getting started" },
            { path: "api/engine.html",                title: "Engine and Renderer" },
            { path: "api/scene.html",                 title: "Scenes, entities, meshes, lights" },
            { path: "api/materials.html",             title: "Materials and textures" },
            { path: "api/camera.html",                title: "Cameras and input" },
            { path: "api/render-graph.html",          title: "Render graph and custom passes" },
            { path: "api/low-level.html",             title: "RenderContext and resources" },
            { path: "api/types.html",                 title: "Types, handles and limits" },
            { path: "api/c-api.html",                 title: "C API" },
        ],
    },
    {
        title: "Project docs (Markdown)",
        pages: [
            { path: "architecture.md",                title: "architecture.md" },
            { path: "code_review.md",                 title: "code_review.md" },
            { path: "development_plan.md",            title: "development_plan.md" },
            { path: "coding_style.md",                title: "coding_style.md" },
            { path: "build_guide.md",                 title: "build_guide.md" },
        ],
    },
];

(function () {
    const root = document.body.dataset.root || "";
    const here = document.body.dataset.page || "";

    function storageGet(key) {
        try { return window.localStorage.getItem(key); } catch (e) { return null; }
    }
    function storageSet(key, value) {
        try { window.localStorage.setItem(key, value); } catch (e) { /* private window */ }
    }
    function escapeText(s) {
        return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    }

    // theme
    const savedTheme = storageGet("rasm-docs-theme");
    if (savedTheme) document.documentElement.dataset.theme = savedTheme;

    // ---------------------------------------------------------------- sidebar
    const nav = document.getElementById("sidebar");
    const flat = [];
    if (nav) {
        let html = `<a class="brand" href="${root}index.html">RASM <small>docs</small></a>` +
                   `<div class="search"><input type="search" id="nav-filter" placeholder="Find a page" aria-label="Find a page" autocomplete="off"><kbd>/</kbd></div>` +
                   `<p class="no-match">No page matches.</p>`;
        for (const section of SECTIONS) {
            html += `<section><h4>${section.title}</h4><ul>`;
            section.pages.forEach(function (page) {
                if (section.guide) flat.push(page);
                const n = section.guide ? `<span class="num">${flat.length}</span>` : "";
                const current = page.path === here ? ' class="current" aria-current="page"' : "";
                const words = escapeText((page.title + " " + (page.blurb || "")).toLowerCase());
                html += `<li data-words="${words}"><a${current} href="${root}${page.path}">${n}<span>${escapeText(page.title)}</span></a></li>`;
            });
            html += "</ul></section>";
        }
        html += `<div class="tools"><button type="button" id="theme-toggle">Light / dark</button></div>`;
        nav.innerHTML = html;

        const current = nav.querySelector("a.current");
        function scrollToCurrent() {
            if (current) nav.scrollTop = Math.max(0, current.offsetTop - nav.clientHeight / 2);
        }
        scrollToCurrent();

        const toggle = document.getElementById("theme-toggle");
        toggle.addEventListener("click", function () {
            const theme = document.documentElement.dataset.theme;
            const dark = theme ? theme === "dark" : window.matchMedia("(prefers-color-scheme: dark)").matches;
            const next = dark ? "light" : "dark";
            document.documentElement.dataset.theme = next;
            storageSet("rasm-docs-theme", next);
        });

        // filter: type to narrow the list, Enter opens the first match, "/" focuses it from anywhere
        const filter = document.getElementById("nav-filter");
        const noMatch = nav.querySelector(".no-match");
        filter.addEventListener("input", function () {
            const words = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
            let shown = 0;
            nav.querySelectorAll("section").forEach(function (section) {
                let any = false;
                section.querySelectorAll("li").forEach(function (li) {
                    const hit = words.every(function (w) { return li.dataset.words.indexOf(w) >= 0; });
                    li.hidden = !hit;
                    any = any || hit;
                    if (hit) shown += 1;
                });
                section.hidden = !any;
            });
            nav.classList.toggle("filtering", words.length > 0);
            noMatch.classList.toggle("show", shown === 0);
            if (words.length === 0) scrollToCurrent();
        });
        filter.addEventListener("keydown", function (e) {
            if (e.key === "Enter") {
                const first = nav.querySelector("li:not([hidden]) a");
                if (first && filter.value.trim()) window.location.href = first.href;
            }
            if (e.key === "Escape") {
                filter.value = "";
                filter.dispatchEvent(new Event("input"));
                filter.blur();
                document.body.classList.remove("nav-open");
            }
        });
        document.addEventListener("keydown", function (e) {
            const tag = (e.target.tagName || "").toLowerCase();
            if (e.key === "/" && !e.ctrlKey && !e.metaKey && !e.altKey && tag !== "input" && tag !== "textarea") {
                e.preventDefault();
                document.body.classList.add("nav-open");
                filter.focus();
            }
        });

        const menu = document.createElement("button");
        menu.className = "menu-button";
        menu.type = "button";
        menu.textContent = "Menu";
        menu.addEventListener("click", function () { document.body.classList.toggle("nav-open"); });
        document.body.appendChild(menu);
    }

    const main = document.querySelector("main");

    // ---------------------------------------------------------------- home page catalog: every guide, by section
    const catalog = document.getElementById("catalog");
    if (catalog) {
        let html = "";
        let n = 0;
        for (const section of SECTIONS) {
            if (!section.guide) continue;
            html += `<h3>${section.title.replace("Guides: ", "").replace(/^./, function (c) { return c.toUpperCase(); })}` +
                    `<small>${escapeText(section.blurb || "")}</small></h3><div class="cards">`;
            section.pages.forEach(function (page) {
                n += 1;
                html += `<a class="card" href="${root}${page.path}"><span class="num">${n}</span><b>${escapeText(page.title)}</b>` +
                        `<span>${escapeText(page.blurb || "")}</span></a>`;
            });
            html += "</div>";
        }
        catalog.innerHTML = html;
    }

    // ---------------------------------------------------------------- guide number, reading time, prev/next
    const index = flat.findIndex(function (p) { return p.path === here; });
    if (main && index >= 0) {
        const eyebrow = main.querySelector(".eyebrow");
        if (eyebrow && !eyebrow.dataset.fixed) eyebrow.textContent = `Guide ${index + 1} · ${eyebrow.textContent}`;

        const lead = main.querySelector(".lead");
        if (lead) {
            // prose only: labels inside drawings and code are not read as text
            let words = 0;
            const walker = document.createTreeWalker(main, NodeFilter.SHOW_TEXT);
            while (walker.nextNode()) {
                const node = walker.currentNode;
                if (node.parentElement.closest("svg, pre")) continue;
                words += node.textContent.split(/\s+/).filter(Boolean).length;
            }
            const figures = main.querySelectorAll("svg[role=img]").length;
            const info = document.createElement("p");
            info.className = "read-time";
            info.textContent = `About ${Math.max(1, Math.round(words / 220))} min to read · ${figures} pictures`;
            lead.insertAdjacentElement("afterend", info);
        }

        const prev = flat[index - 1];
        const next = flat[index + 1];
        const pageNav = document.createElement("nav");
        pageNav.className = "page-nav";
        pageNav.innerHTML =
            (prev ? `<a class="prev" href="${root}${prev.path}"><small>← Previous</small>${escapeText(prev.title)}</a>` : "") +
            (next ? `<a class="next" href="${root}${next.path}"><small>Next →</small>${escapeText(next.title)}</a>` : "");
        main.appendChild(pageNav);
    }

    // sync stamp, written by .claude/skills/sync-docs/scripts/docsync.py
    if (main && window.RASM_DOCS_SYNC) {
        const stamp = document.createElement("p");
        stamp.className = "sync-stamp";
        stamp.textContent = `These docs match the code at commit ${window.RASM_DOCS_SYNC.commit.slice(0, 7)} (${window.RASM_DOCS_SYNC.date}).`;
        main.appendChild(stamp);
    }

    // ---------------------------------------------------------------- "on this page" + heading links + progress bar
    if (main) {
        const used = new Set(Array.from(document.querySelectorAll("[id]")).map(function (el) { return el.id; }));
        const heads = Array.from(main.querySelectorAll("h2, h3"));
        heads.forEach(function (h) {
            if (h.closest(".tldr, .api, .stepper, .vcard, .split")) return;
            if (!h.id) {
                const label = Array.from(h.childNodes).filter(function (c) { return c.nodeName !== "SMALL" && !(c.classList && c.classList.contains("n")); })
                    .map(function (c) { return c.textContent; }).join("");
                const base = label.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "section";
                let id = base;
                for (let k = 2; used.has(id); k++) id = `${base}-${k}`;
                used.add(id);
                h.id = id;
            }
            const a = document.createElement("a");
            a.className = "anchor";
            a.href = `#${h.id}`;
            a.textContent = "#";
            a.setAttribute("aria-label", "Link to this section");
            h.appendChild(a);
        });

        const h2s = heads.filter(function (h) { return h.tagName === "H2" && h.id; });
        const layout = document.querySelector(".layout");
        if (layout && h2s.length >= 2) {
            const toc = document.createElement("aside");
            toc.id = "toc";
            let html = "<h4>On this page</h4><ol>";
            h2s.forEach(function (h) {
                const label = Array.from(h.childNodes)
                    .filter(function (c) { return !(c.classList && (c.classList.contains("n") || c.classList.contains("anchor"))); })
                    .map(function (c) { return c.textContent; }).join("").trim();
                html += `<li><a href="#${h.id}"${h.id === "in-rasm" ? ' class="in-rasm"' : ""}>${escapeText(label)}</a></li>`;
            });
            html += `</ol><a class="top" href="#">↑ Top</a>`;
            toc.innerHTML = html;
            layout.appendChild(toc);

            const links = Array.from(toc.querySelectorAll("ol a"));
            function spy() {
                let at = -1;
                h2s.forEach(function (h, k) { if (h.getBoundingClientRect().top < 120) at = k; });
                links.forEach(function (a, k) { a.classList.toggle("active", k === at); });
            }
            window.addEventListener("scroll", spy, { passive: true });
            spy();
        }

        const bar = document.createElement("div");
        bar.id = "progress";
        document.body.appendChild(bar);
        function progress() {
            const span = document.documentElement.scrollHeight - window.innerHeight;
            bar.style.width = span > 0 ? `${Math.min(100, (window.scrollY / span) * 100)}%` : "0";
        }
        window.addEventListener("scroll", progress, { passive: true });
        window.addEventListener("resize", progress);
        progress();
    }

    // ---------------------------------------------------------------- diagram zoom
    function closeLightbox() {
        const box = document.getElementById("lightbox");
        if (box) box.remove();
    }
    document.querySelectorAll("figure.diagram").forEach(function (figure) {
        const svg = figure.querySelector(":scope > svg");
        if (!svg) return;
        const button = document.createElement("button");
        button.type = "button";
        button.className = "zoom";
        button.textContent = "⤢";
        button.title = "Enlarge this picture";
        button.setAttribute("aria-label", "Enlarge this picture");
        button.addEventListener("click", function () {
            closeLightbox();
            const box = document.createElement("div");
            box.id = "lightbox";
            const inner = document.createElement("div");
            inner.appendChild(svg.cloneNode(true));
            box.appendChild(inner);
            box.addEventListener("click", closeLightbox);
            document.body.appendChild(box);
        });
        figure.appendChild(button);
    });
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") closeLightbox(); });

    // ---------------------------------------------------------------- copy buttons on code blocks
    document.querySelectorAll("main pre").forEach(function (pre) {
        if (pre.classList.contains("sig") || !navigator.clipboard) return;
        const wrap = document.createElement("div");
        wrap.className = "code-wrap";
        pre.parentNode.insertBefore(wrap, pre);
        wrap.appendChild(pre);
        const button = document.createElement("button");
        button.type = "button";
        button.className = "copy";
        button.textContent = "Copy";
        button.addEventListener("click", function () {
            navigator.clipboard.writeText(pre.textContent).then(function () {
                button.textContent = "Copied";
                window.setTimeout(function () { button.textContent = "Copy"; }, 1200);
            });
        });
        wrap.appendChild(button);
    });

    // ---------------------------------------------------------------- steppers
    // <div class="stepper"><div class="step"><h4>Step title</h4> ... </div> ... </div>
    document.querySelectorAll(".stepper").forEach(function (stepper) {
        const steps = Array.from(stepper.querySelectorAll(":scope > .step"));
        if (steps.length === 0) return;
        stepper.classList.add("js");
        const bar = document.createElement("div");
        bar.className = "stepper-bar";
        bar.innerHTML = '<button type="button" class="prev">◀ Back</button><span class="label"></span>' +
                        '<span class="dots"></span><span class="count"></span><button type="button" class="next">Next ▶</button>';
        stepper.insertBefore(bar, steps[0]);
        const label = bar.querySelector(".label");
        const count = bar.querySelector(".count");
        const prev = bar.querySelector(".prev");
        const next = bar.querySelector(".next");
        const dots = steps.map(function (s, k) {
            const dot = document.createElement("i");
            dot.addEventListener("click", function () { show(k); });
            bar.querySelector(".dots").appendChild(dot);
            return dot;
        });
        let at = 0;
        function show(i) {
            at = Math.max(0, Math.min(steps.length - 1, i));
            steps.forEach(function (s, k) { s.classList.toggle("active", k === at); });
            dots.forEach(function (d, k) { d.classList.toggle("on", k === at); });
            const h = steps[at].querySelector("h4");
            label.textContent = h ? h.textContent : "";
            count.textContent = `${at + 1} / ${steps.length}`;
            prev.disabled = at === 0;
            next.disabled = at === steps.length - 1;
        }
        prev.addEventListener("click", function () { show(at - 1); });
        next.addEventListener("click", function () { show(at + 1); });
        stepper.tabIndex = 0;
        stepper.addEventListener("keydown", function (e) {
            if (e.key === "ArrowRight") show(at + 1);
            if (e.key === "ArrowLeft") show(at - 1);
        });
        show(0);
    });
})();

// ---------------------------------------------------------------- syntax highlighting
// A small offline highlighter: C++ and Slang (default), CMake and shell. A block picks its language with
// <code class="lang-cpp | lang-slang | lang-cmake | lang-shell | lang-text">; without a class, CMake and shell
// are recognized and everything else is C++. lang-text is left as is (logs, math).

const C_KEYWORDS = new Set((
    "alignas alignof auto break case catch class const constexpr const_cast continue decltype default delete do " +
    "dynamic_cast else enum explicit extern false for friend goto if inline mutable namespace new noexcept nullptr " +
    "operator override private protected public reinterpret_cast return sizeof static static_assert static_cast " +
    "struct switch template this throw true try typedef typename union using virtual volatile while " +
    "in out inout uniform groupshared import module let var func").split(" "));

const C_TYPES = new Set((
    "void bool char short int long float double unsigned signed size_t int8_t int16_t int32_t int64_t uint8_t " +
    "uint16_t uint32_t uint64_t uintptr_t uint uint2 uint3 uint4 int2 int3 int4 float2 float3 float4 float3x3 " +
    "float4x4 half vec2 vec3 vec4 ivec2 ivec3 uvec2 mat3 mat4 quat string vector function unique_ptr shared_ptr " +
    "array unordered_map optional pair").split(" "));

function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function token(kind, text) {
    return kind ? `<span class="tok-${kind}">${escapeHtml(text)}</span>` : escapeHtml(text);
}

function languageOf(el) {
    const m = (el.className || "").match(/\blang-(\w+)/);
    if (m) return m[1] === "slang" ? "cpp" : m[1];
    if (el.classList.contains("sig")) return "cpp";
    const text = el.textContent;
    if (/^\s*(cmake_minimum_required|find_package|target_link_libraries|add_executable|add_subdirectory)\s*\(/m.test(text)) return "cmake";
    if (/^\s*(cmake|ctest|git|python|pip|vcpkg|\.\\|\.\/)\s/m.test(text) && !/;\s*$/m.test(text)) return "shell";
    return "cpp";
}

function highlight(text, lang) {
    if (lang === "cmake") return highlightCMake(text);
    if (lang === "shell") return highlightShell(text);
    return highlightC(text);
}

function highlightC(src) {
    const rules = [
        ["com",  /\/\/[^\n]*|\/\*[\s\S]*?\*\//y],
        ["attr", /\[\[[^\]\n]*\]\]/y],
        ["str",  /"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])'/y],
        ["num",  /(?:0[xX][0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)[uUlLfF]*/y],
        ["id",   /[A-Za-z_]\w*/y],
        ["ws",   /\s+/y],
    ];
    let out = "";
    let i = 0;
    let lineStart = true;
    while (i < src.length) {
        // a preprocessor line: #include "x" keeps its string colored
        if (lineStart && src[i] === "#") {
            const m = /#[ \t]*\w+/y;
            m.lastIndex = i;
            const hit = m.exec(src);
            if (hit) {
                out += token("pre", hit[0]);
                i += hit[0].length;
                lineStart = false;
                continue;
            }
        }
        let matched = false;
        for (const [kind, re] of rules) {
            re.lastIndex = i;
            const hit = re.exec(src);
            if (!hit || hit[0].length === 0) continue;
            const text = hit[0];
            if (kind === "ws") {
                out += escapeHtml(text);
                if (text.indexOf("\n") >= 0) lineStart = true;
            } else if (kind === "id") {
                out += token(classifyIdentifier(text, src, i + text.length), text);
                lineStart = false;
            } else {
                out += token(kind, text);
                lineStart = false;
            }
            i += text.length;
            matched = true;
            break;
        }
        if (!matched) {
            out += escapeHtml(src[i]);
            lineStart = false;
            i += 1;
        }
    }
    return out;
}

function classifyIdentifier(name, src, end) {
    if (C_KEYWORDS.has(name)) return "kw";
    if (C_TYPES.has(name)) return "type";
    const after = src.slice(end, end + 2);
    const nextChar = (src.slice(end).match(/^\s*(.)/) || [])[1];
    if (after === "::") return /^[A-Z]/.test(name) ? "type" : "ns";
    if (/^[A-Z][A-Z0-9_]+$/.test(name)) return "const";
    if (nextChar === "(" && !/^[A-Z]/.test(name)) return "fn";
    if (/^[A-Z]/.test(name)) return "type";
    return "";
}

function highlightCMake(src) {
    return src.split("\n").map(function (line) {
        return line.replace(/(#.*$)|("(?:\\.|[^"\\])*")|(\$\{[^}]*\})|(\b[A-Za-z_]\w*)(?=\s*\()|(\b[A-Z][A-Z0-9_]{2,}\b)|([^#"$A-Za-z_]+|[\s\S])/g,
            function (all, com, str, variable, command, keyword) {
                if (com) return token("com", com);
                if (str) return token("str", str);
                if (variable) return token("const", variable);
                if (command) return token("fn", command);
                if (keyword) return token("kw", keyword);
                return escapeHtml(all);
            });
    }).join("\n");
}

function highlightShell(src) {
    return src.split("\n").map(function (line) {
        let first = true;
        return line.replace(/(\s*#.*$)|("(?:\\.|[^"\\])*"|'[^']*')|(\s--?[A-Za-z][\w-]*)|(\S+)|(\s+)/g,
            function (all, com, str, flag, word) {
                if (com) return token("com", com);
                if (str) { first = false; return token("str", str); }
                if (flag) return token("kw", flag);
                if (word) {
                    const kind = first ? "fn" : "";
                    first = false;
                    return token(kind, word);
                }
                return escapeHtml(all);
            });
    }).join("\n");
}

document.querySelectorAll("pre.sig, pre > code").forEach(function (el) {
    const lang = languageOf(el);
    if (lang !== "text") el.innerHTML = highlight(el.textContent, lang);
});

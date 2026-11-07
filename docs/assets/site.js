// RASM docs: builds the sidebar, the prev/next links and the steppers. Works from file:// (no fetch).
// A new page is added to SECTIONS and nowhere else.

const SECTIONS = [
    {
        title: "Start",
        pages: [
            { path: "index.html", title: "Home" },
        ],
    },
    {
        title: "Guides: foundations",
        guide: true,
        pages: [
            { path: "guides/big-picture.html",        title: "The big picture" },
            { path: "guides/frame-loop.html",         title: "One frame, step by step" },
            { path: "guides/rhi.html",                title: "The RHI and the Vulkan backend" },
        ],
    },
    {
        title: "Guides: data",
        guide: true,
        pages: [
            { path: "guides/gpu-memory.html",         title: "How data reaches the GPU" },
            { path: "guides/gpu-scene.html",          title: "The GPU scene tables" },
            { path: "guides/asset-loading.html",      title: "Loading assets in the background" },
            { path: "guides/materials-textures.html", title: "Materials and bindless textures" },
        ],
    },
    {
        title: "Guides: rendering",
        guide: true,
        pages: [
            { path: "guides/uber-shader.html",        title: "Drawing with the uber shader" },
            { path: "guides/culling.html",            title: "GPU frustum culling" },
            { path: "guides/camera-depth.html",       title: "Camera, coordinates and depth" },
            { path: "guides/render-graph.html",       title: "The render graph" },
            { path: "guides/stats-overlay.html",      title: "Frame stats and the overlay" },
            { path: "guides/shaders-structs.html",    title: "Shaders and the CPU↔GPU struct contract" },
        ],
    },
    {
        title: "API reference",
        pages: [
            { path: "api/index.html",                 title: "Getting started" },
            { path: "api/engine.html",                title: "Engine" },
            { path: "api/scene.html",                 title: "Scenes, entities, meshes, lights" },
            { path: "api/materials.html",             title: "Materials and textures" },
            { path: "api/camera.html",                title: "Cameras and input" },
            { path: "api/render-graph.html",          title: "Render graph and custom passes" },
            { path: "api/low-level.html",             title: "RenderContext and resources" },
            { path: "api/types.html",                 title: "Types, handles and limits" },
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

    // theme
    const savedTheme = storageGet("rasm-docs-theme");
    if (savedTheme) document.documentElement.dataset.theme = savedTheme;

    // sidebar
    const nav = document.getElementById("sidebar");
    const flat = [];
    if (nav) {
        let html = `<a class="brand" href="${root}index.html">RASM <small>docs</small></a>`;
        for (const section of SECTIONS) {
            html += `<h4>${section.title}</h4><ul>`;
            section.pages.forEach(function (page) {
                if (section.guide) flat.push(page);
                const n = section.guide ? `<span class="num">${flat.length}</span>` : "";
                const current = page.path === here ? ' class="current"' : "";
                html += `<li><a${current} href="${root}${page.path}">${n}${page.title}</a></li>`;
            });
            html += "</ul>";
        }
        html += `<div class="tools"><button type="button" id="theme-toggle">Light / dark</button></div>`;
        nav.innerHTML = html;

        const toggle = document.getElementById("theme-toggle");
        toggle.addEventListener("click", function () {
            const dark = getComputedStyle(document.documentElement).getPropertyValue("--bg").trim() === "#16171a";
            const next = dark ? "light" : "dark";
            document.documentElement.dataset.theme = next;
            storageSet("rasm-docs-theme", next);
        });

        const menu = document.createElement("button");
        menu.className = "menu-button";
        menu.type = "button";
        menu.textContent = "Menu";
        menu.addEventListener("click", function () { document.body.classList.toggle("nav-open"); });
        document.body.appendChild(menu);
    }

    const main = document.querySelector("main");

    // guide number in the eyebrow + prev/next between guides
    const index = flat.findIndex(function (p) { return p.path === here; });
    if (main && index >= 0) {
        const eyebrow = main.querySelector(".eyebrow");
        if (eyebrow && !eyebrow.dataset.fixed) eyebrow.textContent = `Guide ${index + 1} · ${eyebrow.textContent}`;
        const prev = flat[index - 1];
        const next = flat[index + 1];
        const pageNav = document.createElement("nav");
        pageNav.className = "page-nav";
        pageNav.innerHTML =
            (prev ? `<a class="prev" href="${root}${prev.path}"><small>Previous</small>${prev.title}</a>` : "") +
            (next ? `<a class="next" href="${root}${next.path}"><small>Next</small>${next.title}</a>` : "");
        main.appendChild(pageNav);
    }

    // sync stamp, written by .claude/skills/sync-docs/scripts/docsync.py
    if (main && window.RASM_DOCS_SYNC) {
        const stamp = document.createElement("p");
        stamp.className = "sync-stamp";
        stamp.textContent = `These docs match the code at commit ${window.RASM_DOCS_SYNC.commit} (${window.RASM_DOCS_SYNC.date}).`;
        main.appendChild(stamp);
    }

    // steppers: <div class="stepper" data-title="..."><div class="step"><h4>Step title</h4> ... </div> ... </div>
    document.querySelectorAll(".stepper").forEach(function (stepper) {
        const steps = Array.from(stepper.querySelectorAll(":scope > .step"));
        if (steps.length === 0) return;
        stepper.classList.add("js");
        const bar = document.createElement("div");
        bar.className = "stepper-bar";
        bar.innerHTML = '<button type="button" class="prev">◀ Back</button><span class="label"></span>' +
                        '<span class="count"></span><button type="button" class="next">Next ▶</button>';
        stepper.insertBefore(bar, steps[0]);
        const label = bar.querySelector(".label");
        const count = bar.querySelector(".count");
        const prev = bar.querySelector(".prev");
        const next = bar.querySelector(".next");
        let at = 0;
        function show(i) {
            at = Math.max(0, Math.min(steps.length - 1, i));
            steps.forEach(function (s, k) { s.classList.toggle("active", k === at); });
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

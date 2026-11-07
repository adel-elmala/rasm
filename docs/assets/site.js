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
            { path: "guides/culling.html",            title: "GPU culling" },
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
            const theme = document.documentElement.dataset.theme;
            const dark = theme ? theme === "dark" : window.matchMedia("(prefers-color-scheme: dark)").matches;
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
        stamp.textContent = `These docs match the code at commit ${window.RASM_DOCS_SYNC.commit.slice(0, 7)} (${window.RASM_DOCS_SYNC.date}).`;
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

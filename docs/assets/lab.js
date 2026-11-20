// RASM docs: interactive pictures ("labs"). A guide places
//     <figure class="lab" data-lab="name" data-title="Title"><figcaption>What to try.</figcaption></figure>
// and its script docs/assets/labs/<guide>.js calls RasmLab.register("name", function (lab) { ... }).
// The setup function builds the picture and its controls with the helpers below, then gives lab.draw() one
// function that updates the picture from the controls. Works from file://, no libraries.
// Open a page with ?labtest to move every control once; a lab that throws shows the error in its box.

const RasmLab = (function () {
    const SVG_NS = "http://www.w3.org/2000/svg";
    const registry = {};
    const labs = [];
    const testing = /[?&]labtest\b/.test(window.location.search);
    const calm = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    function html(parent, tag, cls, text) {
        const el = document.createElement(tag);
        if (cls) el.className = cls;
        if (text !== undefined) el.textContent = text;
        if (parent) parent.appendChild(el);
        return el;
    }

    function set(el, attrs) {
        for (const key in attrs) {
            if (key === "text") el.textContent = attrs[key];
            else if (attrs[key] === null || attrs[key] === false) el.removeAttribute(key);
            else el.setAttribute(key, attrs[key]);
        }
        return el;
    }

    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    function create(root, setup) {
        const caption = root.querySelector(":scope > figcaption");
        const head = html(null, "div", "lab-head");
        html(head, "span", "lab-badge", "Try it");
        html(head, "b", "", root.dataset.title || "");
        const stage = html(null, "div", "lab-stage");
        const controls = html(null, "div", "lab-controls");
        const readouts = html(null, "div", "lab-readouts");
        root.insertBefore(readouts, root.firstChild);
        root.insertBefore(controls, root.firstChild);
        root.insertBefore(stage, root.firstChild);
        root.insertBefore(head, root.firstChild);

        let drawFn = null;
        let ready = false;
        const tests = [];      // functions that move one control, for ?labtest
        const tickers = [];

        const lab = {
            root: root,
            stage: stage,
            clamp: clamp,
            set: set,

            // ---- pictures
            // An <svg> in the stage. Use the classes of docs/assets/style.css, like a static diagram.
            svg: function (width, height, label) {
                const svg = document.createElementNS(SVG_NS, "svg");
                set(svg, { viewBox: `0 0 ${width} ${height}`, role: "img", "aria-label": label || root.dataset.title || "" });
                stage.appendChild(svg);
                return svg;
            },
            // An SVG child: lab.el(svg, "circle", { cx: 10, cy: 10, r: 4, class: "gpu" }). "text" sets the content.
            el: function (parent, tag, attrs) {
                const el = document.createElementNS(SVG_NS, tag);
                if (attrs) set(el, attrs);
                parent.appendChild(el);
                return el;
            },
            clear: function (el) {
                while (el.firstChild) el.removeChild(el.firstChild);
            },
            // A <canvas> for per-pixel pictures (a shaded sphere, a texture). Returns its 2D context.
            canvas: function (width, height, label) {
                const canvas = html(stage, "canvas");
                canvas.width = width;
                canvas.height = height;
                set(canvas, { role: "img", "aria-label": label || root.dataset.title || "" });
                return canvas.getContext("2d");
            },
            // The theme's value of a CSS variable, for canvas drawing: lab.css("--gpu").
            css: function (name) {
                return getComputedStyle(root).getPropertyValue(name).trim();
            },

            // ---- controls (each returns an object with .value and .set(v))
            slider: function (o) {
                const wrap = html(controls, "label", "lab-control");
                html(wrap, "span", "", o.label);
                const input = html(wrap, "input");
                input.type = "range";
                input.min = o.min;
                input.max = o.max;
                input.step = o.step || 1;
                input.value = o.value;
                const out = html(wrap, "output");
                const format = o.format || function (v) { return String(v); };
                const control = {
                    get value() { return Number(input.value); },
                    set: function (v) { input.value = v; out.textContent = format(Number(input.value)); lab.redraw(); },
                };
                out.textContent = format(Number(input.value));
                input.addEventListener("input", function () { control.set(input.value); });
                tests.push(function () {
                    const start = input.value;
                    [o.min, o.max, (Number(o.min) + Number(o.max)) / 2, start].forEach(control.set);
                });
                return control;
            },
            // A row of buttons, one active: options are [{ value, label }] or plain strings.
            choice: function (o) {
                const wrap = html(controls, "div", "lab-control");
                if (o.label) html(wrap, "span", "", o.label);
                const seg = html(wrap, "div", "lab-seg");
                seg.setAttribute("role", "group");
                if (o.label) seg.setAttribute("aria-label", o.label);
                const options = o.options.map(function (opt) { return typeof opt === "string" ? { value: opt, label: opt } : opt; });
                let value = o.value !== undefined ? o.value : options[0].value;
                const buttons = options.map(function (opt) {
                    const b = html(seg, "button", "", opt.label);
                    b.type = "button";
                    b.addEventListener("click", function () { control.set(opt.value); });
                    return b;
                });
                function paint() {
                    buttons.forEach(function (b, k) { b.setAttribute("aria-pressed", options[k].value === value ? "true" : "false"); });
                }
                const control = {
                    get value() { return value; },
                    set: function (v) { value = v; paint(); lab.redraw(); },
                };
                paint();
                tests.push(function () {
                    const start = value;
                    options.forEach(function (opt) { control.set(opt.value); });
                    control.set(start);
                });
                return control;
            },
            toggle: function (o) {
                const wrap = html(controls, "label", "lab-control lab-check");
                const input = html(wrap, "input");
                input.type = "checkbox";
                input.checked = !!o.value;
                html(wrap, "span", "", o.label);
                const control = {
                    get value() { return input.checked; },
                    set: function (v) { input.checked = !!v; lab.redraw(); },
                };
                input.addEventListener("change", function () { lab.redraw(); });
                tests.push(function () { control.set(!control.value); control.set(!control.value); });
                return control;
            },
            button: function (label, onClick) {
                const b = html(controls, "button", "lab-button", label);
                b.type = "button";
                b.addEventListener("click", function () { onClick(); lab.redraw(); });
                tests.push(function () { for (let k = 0; k < 3; k++) { onClick(); lab.redraw(); } });
                return b;
            },

            // ---- direct manipulation
            // A draggable point in an svg: lab.handle(svg, { x, y, label, minX, maxX, minY, maxY }).
            // Returns { x, y, el, moveTo(x, y) }. Arrow keys move it too. Leave min = max to lock an axis.
            handle: function (svg, o) {
                const minX = o.minX !== undefined ? o.minX : 0;
                const maxX = o.maxX !== undefined ? o.maxX : svg.viewBox.baseVal.width;
                const minY = o.minY !== undefined ? o.minY : 0;
                const maxY = o.maxY !== undefined ? o.maxY : svg.viewBox.baseVal.height;
                const circle = lab.el(svg, "circle", { r: o.r || 9, class: "handle", tabindex: 0, role: "slider", "aria-label": o.label || "Drag" });
                const h = {
                    x: o.x,
                    y: o.y,
                    el: circle,
                    moveTo: function (x, y) {
                        h.x = clamp(x, minX, maxX);
                        h.y = clamp(y, minY, maxY);
                        set(circle, { cx: h.x, cy: h.y });
                        lab.redraw();
                    },
                };
                set(circle, { cx: h.x, cy: h.y });
                function fromEvent(e) {
                    const pt = svg.createSVGPoint();
                    pt.x = e.clientX;
                    pt.y = e.clientY;
                    const p = pt.matrixTransform(svg.getScreenCTM().inverse());
                    h.moveTo(p.x, p.y);
                }
                circle.addEventListener("pointerdown", function (e) {
                    circle.setPointerCapture(e.pointerId);
                    circle.classList.add("dragging");
                    e.preventDefault();
                });
                circle.addEventListener("pointermove", function (e) {
                    if (circle.hasPointerCapture(e.pointerId)) fromEvent(e);
                });
                circle.addEventListener("pointerup", function () { circle.classList.remove("dragging"); });
                circle.addEventListener("pointercancel", function () { circle.classList.remove("dragging"); });
                circle.addEventListener("keydown", function (e) {
                    const step = e.shiftKey ? 20 : 5;
                    const move = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }[e.key];
                    if (!move) return;
                    e.preventDefault();
                    h.moveTo(h.x + move[0], h.y + move[1]);
                });
                tests.push(function () {
                    const x = h.x;
                    const y = h.y;
                    [[minX, minY], [maxX, minY], [maxX, maxY], [minX, maxY], [x, y]].forEach(function (p) { h.moveTo(p[0], p[1]); });
                });
                return h;
            },
            // Make any element act as a button (mouse, Enter, Space).
            clickable: function (el, label, onClick) {
                set(el, { tabindex: 0, role: "button", "aria-label": label });
                el.classList.add("pick");
                el.addEventListener("click", function () { onClick(); lab.redraw(); });
                el.addEventListener("keydown", function (e) {
                    if (e.key !== "Enter" && e.key !== " ") return;
                    e.preventDefault();
                    onClick();
                    lab.redraw();
                });
                tests.push(function () { onClick(); lab.redraw(); });
            },

            // ---- output
            // A number tile under the picture: const r = lab.readout("Drawn"); r.set("12 / 40", "ok").
            readout: function (label) {
                const tile = html(readouts, "div");
                const value = html(tile, "b", "", "–");
                html(tile, "span", "", label);
                return {
                    set: function (text, cls) {
                        value.textContent = text;
                        value.className = cls ? `c-${cls}` : "";
                    },
                };
            },
            // One line of plain text under the controls that says what the picture shows now.
            note: function () {
                const p = html(null, "p", "lab-note");
                p.setAttribute("aria-live", "polite");
                root.insertBefore(p, caption);
                return { set: function (text, cls) { p.textContent = text; p.className = "lab-note" + (cls ? ` c-${cls}` : ""); } };
            },

            // ---- drawing and time
            // The one function that updates the picture. Called after setup and after every control change.
            draw: function (fn) {
                drawFn = fn;
            },
            redraw: function () {
                if (ready && drawFn) drawFn();
            },
            // fn(dt, t) in seconds, called each frame while the lab is on screen and playing. Adds a Play/Pause button.
            // Returns { playing, play(), pause() }. With "reduce motion" on, it starts paused.
            animate: function (fn, o) {
                const ticker = { fn: fn, playing: !calm && !(o && o.paused), t: 0, visible: false };
                const b = html(controls, "button", "lab-button lab-play");
                b.type = "button";
                function paint() { b.textContent = ticker.playing ? "❚❚ Pause" : "▶ Play"; }
                ticker.play = function () { ticker.playing = true; paint(); };
                ticker.pause = function () { ticker.playing = false; paint(); };
                b.addEventListener("click", function () { ticker.playing = !ticker.playing; paint(); });
                paint();
                tickers.push(ticker);
                tests.push(function () {
                    for (let k = 0; k < 40; k++) { ticker.t += 0.1; fn(0.1, ticker.t); lab.redraw(); }
                });
                return ticker;
            },
        };

        function fail(error) {
            console.error(`lab "${root.dataset.lab}":`, error);
            root.classList.add("lab-error");
            let box = root.querySelector(":scope > .lab-error-text");
            if (!box) box = html(root, "p", "lab-error-text");
            box.textContent = `This interactive picture failed: ${error && error.message ? error.message : error}`;
        }

        try {
            setup(lab);
            ready = true;
            lab.redraw();
            root.dataset.mounted = "1";
        } catch (error) {
            fail(error);
            return;
        }
        if (!controls.firstChild) controls.remove();
        if (!readouts.firstChild) readouts.remove();

        if (tickers.length) {
            let last = 0;
            let visible = false;
            function frame(now) {
                const dt = Math.min(0.05, (now - last) / 1000);
                last = now;
                if (visible) {
                    try {
                        tickers.forEach(function (ticker) {
                            if (!ticker.playing) return;
                            ticker.t += dt;
                            ticker.fn(dt, ticker.t);
                        });
                        lab.redraw();
                    } catch (error) {
                        fail(error);
                        return;
                    }
                }
                window.requestAnimationFrame(frame);
            }
            new IntersectionObserver(function (entries) { visible = entries[entries.length - 1].isIntersecting; }).observe(root);
            window.requestAnimationFrame(function (now) { last = now; frame(now); });
        }

        labs.push(lab);
        if (testing) {
            try {
                tests.forEach(function (test) { test(); });
                root.dataset.tested = String(tests.length);
            } catch (error) {
                fail(error);
            }
        }
    }

    // For screenshots: ?labonly=name shows that lab alone, &theme=dark|light forces a theme.
    function isolate() {
        const only = (window.location.search.match(/[?&]labonly=([\w-]+)/) || [])[1];
        const theme = (window.location.search.match(/[?&]theme=(dark|light)/) || [])[1];
        if (theme) document.documentElement.dataset.theme = theme;
        const root = only && document.querySelector(`figure.lab[data-lab="${only}"]`);
        if (!root) return;
        const legend = root.previousElementSibling;
        const defs = document.querySelector("body > svg");      // the arrowhead markers
        document.body.textContent = "";
        if (defs) document.body.appendChild(defs);
        const main = html(document.body, "main");
        if (legend && legend.classList.contains("legend")) main.appendChild(legend);
        main.appendChild(root);
    }

    function mountAll() {
        isolate();
        document.querySelectorAll("figure.lab[data-lab]").forEach(function (root) {
            const setup = registry[root.dataset.lab];
            if (!setup) {
                root.classList.add("lab-error");
                html(root, "p", "lab-error-text", `No lab named "${root.dataset.lab}" is registered.`);
                return;
            }
            create(root, setup);
        });
        // canvas pictures read theme colors, so draw again when the theme changes
        new MutationObserver(function () { labs.forEach(function (lab) { lab.redraw(); }); })
            .observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
        window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", function () {
            labs.forEach(function (lab) { lab.redraw(); });
        });
    }

    // deferred scripts run before DOMContentLoaded, so every guide script has registered its labs by then
    if (document.readyState === "complete") mountAll();
    else document.addEventListener("DOMContentLoaded", mountAll);

    return {
        register: function (name, setup) { registry[name] = setup; },
    };
})();

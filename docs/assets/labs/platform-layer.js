// Labs of docs/guides/platform-layer.html.

// A window with a UI panel. A press over the panel belongs to the UI until the release; a press outside starts the
// camera's drag, which keeps going over the panel. The drag is measured as the camera reads it: points divided by the
// smaller window side, y up.
RasmLab.register("route-input", function (lab) {
    const svg = lab.svg(760, 330, "A window with a UI panel in its top-left corner and a mouse pointer you can move. A press over the panel goes to the UI; a press outside starts a camera drag, drawn as an arrow from the press to the pointer.");
    const button = lab.choice({ label: "Mouse button", options: ["up", "down"], value: "up" });
    const ownerOut = lab.readout("who has the mouse");
    const captureOut = lab.readout("the UI wants the mouse");
    const dragOut = lab.readout("camera drag, in NDC");
    const note = lab.note();

    const WIN = { x: 20, y: 20, w: 720, h: 290 };
    const PANEL = { x: 40, y: 40, w: 220, h: 130 };

    lab.el(svg, "rect", { x: WIN.x, y: WIN.y, width: WIN.w, height: WIN.h, rx: 6, class: "paper" });
    lab.el(svg, "text", { x: WIN.x + WIN.w - 10, y: WIN.y + WIN.h - 12, class: "end small t-faint", text: "the scene: the camera's area" });
    const panel = lab.el(svg, "rect", { x: PANEL.x, y: PANEL.y, width: PANEL.w, height: PANEL.h, rx: 5, class: "box" });
    lab.el(svg, "text", { x: PANEL.x + 12, y: PANEL.y + 22, class: "small title", text: "UI window" });
    lab.el(svg, "rect", { x: PANEL.x + 12, y: PANEL.y + 40, width: 150, height: 16, rx: 3, class: "zone" });
    lab.el(svg, "text", { x: PANEL.x + 170, y: PANEL.y + 53, class: "small", text: "slider" });
    lab.el(svg, "rect", { x: PANEL.x + 12, y: PANEL.y + 70, width: 16, height: 16, rx: 3, class: "zone" });
    lab.el(svg, "text", { x: PANEL.x + 36, y: PANEL.y + 83, class: "small", text: "checkbox" });
    const drag = lab.el(svg, "g");
    const pointer = lab.handle(svg, { x: 520, y: 200, label: "Move the mouse pointer", minX: WIN.x + 4, maxX: WIN.x + WIN.w - 4, minY: WIN.y + 4, maxY: WIN.y + WIN.h - 4 });

    let owner = null;
    let press = null;

    function overPanel(x, y) {
        return x >= PANEL.x && x <= PANEL.x + PANEL.w && y >= PANEL.y && y <= PANEL.y + PANEL.h;
    }
    // Window points to the camera's NDC: divided by the smaller side, y up.
    function toNdc(x, y) {
        const px = x - WIN.x;
        const py = y - WIN.y;
        const side = Math.min(WIN.w, WIN.h);
        return [(2 * px - WIN.w) / side, (WIN.h - 2 * py) / side];
    }

    lab.draw(function () {
        const down = button.value === "down";
        if (!down) {
            owner = null;
            press = null;
        } else if (owner === null) {
            // The press is decided where it happens, with the last frame's capture.
            owner = overPanel(pointer.x, pointer.y) ? "ui" : "camera";
            press = { x: pointer.x, y: pointer.y };
        }
        const hovered = overPanel(pointer.x, pointer.y);
        const wants = owner === "ui" || (owner === null && hovered);

        lab.set(panel, { class: wants ? "shader" : "box" });
        lab.clear(drag);
        if (owner === "camera") {
            lab.el(drag, "line", { x1: press.x, y1: press.y, x2: pointer.x, y2: pointer.y, class: "arrow gpu thick", "marker-end": "url(#ah-gpu)" });
            lab.el(drag, "circle", { cx: press.x, cy: press.y, r: 5, class: "gpu" });
            lab.el(drag, "text", { x: press.x, y: press.y + 20, class: "mid small t-gpu halo", text: "press" });
        }

        captureOut.set(wants ? "yes" : "no", wants ? "shader" : "");
        if (owner === "camera") {
            const a = toNdc(press.x, press.y);
            const b = toNdc(pointer.x, pointer.y);
            ownerOut.set("the camera", "gpu");
            dragOut.set(`(${(b[0] - a[0]).toFixed(2)}, ${(b[1] - a[1]).toFixed(2)})`, "gpu");
            if (hovered) note.set("The drag started outside the UI, so it stays the camera's over the panel: the UI does not want the mouse.", "ok");
            else note.set("A press outside the UI: the camera drags. The arrow is this drag in the camera's units.", "ok");
        } else if (owner === "ui") {
            ownerOut.set("the UI", "shader");
            dragOut.set("none");
            if (hovered) note.set("The press began over the panel: the UI has it until the release, and the camera gets nothing.", "ok");
            else note.set("The pointer left the panel, but the press began on it: the UI keeps it (a slider drag), and the camera still gets nothing.", "ok");
        } else {
            ownerOut.set("nobody");
            dragOut.set("none");
            if (hovered) note.set("Over the panel with no button: the UI wants the mouse, so a press here or the wheel will not reach the camera.", "ok");
            else note.set("Over the scene: a press here starts a camera drag. Set the button to down.", "ok");
        }
    });
});

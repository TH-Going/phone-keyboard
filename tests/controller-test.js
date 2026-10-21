// Run with: node --test tests/controller-test.js
// Load the real HTML and controller with a deterministic clock and WebSocket.
// No browser packages, network connection or PC input injection are required.
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const html = fs.readFileSync(path.join(__dirname, "../web/controller.html"), "utf8");
const script = fs.readFileSync(path.join(__dirname, "../web/controller.js"), "utf8");

function controller() {
  let now = 1000;
  let timerId = 0;
  const timers = new Map();
  const messages = [];
  const sockets = [];
  const later = (callback, delay = 0) => {
    timers.set(++timerId, { at: now + delay, callback });
    return timerId;
  };
  function advance(ms) {
    const until = now + ms;
    for (;;) {
      const due = [...timers.entries()].filter(([, timer]) => timer.at <= until).sort((a, b) => a[1].at - b[1].at)[0];
      if (!due) break;
      timers.delete(due[0]);
      now = due[1].at;
      due[1].callback();
    }
    now = until;
  }
  class Target {
    constructor() { this.listeners = new Map(); }
    addEventListener(type, listener) {
      if (!this.listeners.has(type)) this.listeners.set(type, []);
      this.listeners.get(type).push(listener);
    }
    emit(type, values = {}) {
      const event = { type, preventDefault() {}, ...values };
      (this.listeners.get(type) || []).forEach((listener) => listener(event));
    }
  }
  class Element extends Target {
    constructor(tag, attributes = {}, text = "") {
      super();
      this.tag = tag;
      this.attributes = { ...attributes };
      this.dataset = Object.fromEntries(Object.entries(attributes).filter(([key]) => key.startsWith("data-")).map(([key, value]) =>
        [key.slice(5).replace(/-([a-z])/g, (_, char) => char.toUpperCase()), value]));
      this.textContent = text.replace(/&amp;/g, "&");
      this.hidden = "hidden" in attributes;
      this.captures = new Set();
      const classes = new Set((attributes.class || "").split(/\s+/));
      this.classList = {
        add: (...names) => names.forEach((name) => classes.add(name)),
        remove: (...names) => names.forEach((name) => classes.delete(name)),
        contains: (name) => classes.has(name),
        toggle: (name, active = !classes.has(name)) => active ? classes.add(name) : classes.delete(name),
      };
      this.style = { setProperty(name, value) { this[name] = value; }, removeProperty(name) { delete this[name]; } };
    }
    setAttribute(name, value) { this.attributes[name] = value; }
    getAttribute(name) { return this.attributes[name] ?? null; }
    removeAttribute(name) { delete this.attributes[name]; }
    setPointerCapture(id) { this.captures.add(id); }
    hasPointerCapture(id) { return this.captures.has(id); }
    releasePointerCapture(id) {
      if (this.captures.delete(id)) this.emit("lostpointercapture", { pointerId: id });
    }
    getBoundingClientRect() { return { left: 0, top: 0, width: 400, height: 300 }; }
    appendChild() {}
    remove() {}
  }
  const elements = [...html.matchAll(/<(button|div|section|main|output)\b([^>]*)>([^<]*)/g)].map(([, tag, attrs, text]) => {
    const attributes = Object.fromEntries([...attrs.matchAll(/([\w-]+)(?:="([^"]*)")?/g)].map(([, name, value]) => [name, value ?? ""]));
    return new Element(tag, attributes, text);
  });
  const matches = (element, selector) => selector.startsWith("#")
    ? element.attributes.id === selector.slice(1) : element.classList.contains(selector.slice(1));
  const document = new Target();
  document.hidden = false;
  document.querySelector = (selector) => elements.find((element) => matches(element, selector));
  document.querySelectorAll = (selector) => elements.filter((element) => matches(element, selector));
  document.createElement = (tag) => new Element(tag);
  const window = new Target();
  window.setTimeout = later;
  window.clearTimeout = (id) => timers.delete(id);
  class Socket extends Target {
    static OPEN = 1;
    static CONNECTING = 0;
    constructor() {
      super();
      this.readyState = Socket.OPEN;
      sockets.push(this);
      later(() => this.emit("open"));
    }
    send(message) {
      if (this.readyState !== Socket.OPEN) throw new Error("closed");
      messages.push(JSON.parse(message));
    }
    close() { this.readyState = 3; this.emit("close"); }
  }
  vm.runInNewContext(script, {
    document, window, navigator: { vibrate() {} }, WebSocket: Socket,
    location: { pathname: "/c/test", protocol: "http:", host: "localhost" },
    performance: { now: () => now },
    requestAnimationFrame: (callback) => later(callback, 16),
    cancelAnimationFrame: (id) => timers.delete(id),
  });
  advance(0);
  messages.length = 0;
  const key = (code, shift) => elements.find((element) => element.tag === "button" && element.dataset.code === code &&
    (shift === undefined || element.dataset.shift === String(shift)));
  const action = (name) => elements.find((element) => element.dataset.action === name);
  const pointer = (element, type, id = 1, x = 100, y = 100) => {
    element.emit(type, { pointerId: id, pointerType: "touch", button: 0, clientX: x, clientY: y });
    if (type === "pointerup" || type === "pointercancel") element.releasePointerCapture(id);
  };
  const tap = (element) => {
    assert.ok(element, "button exists in the actual HTML");
    pointer(element, "pointerdown");
    pointer(element, "pointerup");
  };
  return {
    window, document, messages, sockets, advance, key, action, pointer, tap,
    pad: document.querySelector("#pad"), divider: document.querySelector(".mode-divider"),
    remote: document.querySelector(".remote"),
    keys: () => messages.filter((message) => message.t === "key").map(({ action, code }) => `${action}:${code}`),
  };
}

test("short tap clicks once, including before the long-press threshold", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.advance(449);
  assert.equal(c.messages.length, 0);
  c.pointer(c.pad, "pointerup");
  c.advance(500);
  assert.deepEqual(c.messages, [{ t: "click", button: "left" }]);
});

test("long press tolerates jitter, then sends down, final movement, up without a click", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.pointer(c.pad, "pointermove", 1, 104, 100);
  c.advance(449);
  assert.equal(c.messages.length, 0);
  c.advance(1);
  assert.deepEqual(c.messages, [{ t: "button", button: "left", down: true }]);
  c.pointer(c.pad, "pointermove", 1, 114, 100);
  c.pointer(c.pad, "pointerup", 1, 124, 100);
  assert.equal(c.messages.at(-1).down, false);
  assert.equal(c.messages.filter((m) => m.t === "button").length, 2);
  assert.equal(c.messages.filter((m) => m.t === "move").reduce((sum, m) => sum + m.dx, 0), 39);
  assert.ok(c.messages.every((m) => m.t !== "click"));
  assert.equal(c.pad.classList.contains("is-dragging"), false);
});

test("holding still and releasing is a down/up pair without an extra click", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.advance(900);
  c.pointer(c.pad, "pointerup");
  assert.deepEqual(c.messages.map((m) => m.down), [true, false]);
});

test("moving before the threshold remains ordinary pointer motion", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.pointer(c.pad, "pointermove", 1, 108, 100);
  c.advance(600);
  c.pointer(c.pad, "pointerup", 1, 120, 100);
  assert.ok(c.messages.length > 0 && c.messages.every((m) => m.t === "move"));
});

test("two-finger tap cancels long press and retains right click", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.advance(200);
  c.pointer(c.pad, "pointerdown", 2, 150, 100);
  c.pointer(c.pad, "pointerup");
  c.pointer(c.pad, "pointerup", 2, 150, 100);
  c.advance(500);
  assert.deepEqual(c.messages, [{ t: "click", button: "right" }]);
});

test("crossing the hold dead zone starts with a small move instead of a cursor jump", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.pointer(c.pad, "pointermove", 1, 106, 100);
  assert.equal(c.messages.length, 0);
  c.pointer(c.pad, "pointermove", 1, 107, 100);
  c.advance(16);
  assert.deepEqual(c.messages, [{ t: "move", dx: 1, dy: 0 }]);
});

test("two-finger scrolling sends wheel input and no drag or click", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.pointer(c.pad, "pointerdown", 2, 150, 100);
  c.pointer(c.pad, "pointermove", 1, 100, 220);
  c.pointer(c.pad, "pointermove", 2, 150, 220);
  c.advance(500);
  c.pointer(c.pad, "pointerup", 1, 100, 220);
  c.pointer(c.pad, "pointerup", 2, 150, 220);
  assert.ok(c.messages.length && c.messages.every((m) => m.t === "wheel"));
});

test("a second finger during drag releases once and suppresses the rest of that gesture", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.advance(450);
  c.pointer(c.pad, "pointerdown", 2, 150, 100);
  c.pointer(c.pad, "pointermove", 2, 150, 250);
  c.pointer(c.pad, "pointerup");
  c.pointer(c.pad, "pointerup", 2, 150, 250);
  assert.deepEqual(c.messages.map((m) => m.down), [true, false]);
});

for (const reason of ["pointercancel", "lostpointercapture", "blur", "pagehide", "resize", "orientationchange", "visibilitychange", "offline", "disconnect"]) {
  test(`${reason} clears both pending long press and active drag`, () => {
    for (const wait of [200, 450]) {
      const c = controller();
      c.pointer(c.pad, "pointerdown");
      c.advance(wait);
      c.messages.length = 0;
      if (reason === "pointercancel" || reason === "lostpointercapture") c.pointer(c.pad, reason);
      else if (reason === "visibilitychange") { c.document.hidden = true; c.document.emit(reason); }
      else if (reason === "disconnect") c.sockets[0].close();
      else c.window.emit(reason);
      c.advance(500);
      c.pointer(c.pad, "pointerup");
      assert.ok(c.messages.every((m) => m.t === "release"));
      assert.equal(c.pad.classList.contains("is-dragging"), false);
      assert.equal(c.pad.classList.contains("is-touching"), false);
    }
  });
}

test("drag handle changes one mode per gesture; cancellation and vertical drags do not switch", () => {
  const c = controller();
  c.pointer(c.divider, "pointerdown");
  c.pointer(c.divider, "pointerup", 1, 40, 100);
  assert.equal(c.remote.dataset.mode, "mouse");
  assert.equal(c.document.querySelector(".keyboard").hidden, true);
  c.pointer(c.divider, "pointerdown");
  c.pointer(c.divider, "pointerup", 1, 170, 100);
  assert.equal(c.remote.dataset.mode, "split");
  c.pointer(c.divider, "pointerdown");
  c.pointer(c.divider, "pointercancel", 1, 170, 100);
  assert.equal(c.remote.dataset.mode, "split");
  c.pointer(c.divider, "pointerdown");
  c.pointer(c.divider, "pointerup", 1, 170, 200);
  assert.equal(c.remote.dataset.mode, "split");
  c.divider.emit("keydown", { key: "End" });
  assert.equal(c.remote.dataset.mode, "keyboard");
  assert.equal(c.document.querySelector(".mouse").hidden, true);
});

test("mode switching releases an active mouse drag", () => {
  const c = controller();
  c.pointer(c.pad, "pointerdown");
  c.advance(450);
  c.messages.length = 0;
  c.divider.emit("keydown", { key: "End" });
  c.pointer(c.pad, "pointerup");
  assert.deepEqual(c.messages, [{ t: "release" }]);
});

test("Shift is latched locally, applied once, and remains separate from Caps Lock", () => {
  const c = controller();
  c.tap(c.key("ShiftLeft"));
  assert.equal(c.messages.length, 0);
  assert.equal(c.key("KeyQ").textContent, "Q");
  c.tap(c.key("KeyQ"));
  assert.deepEqual(c.keys(), ["down:ShiftLeft", "down:KeyQ", "up:KeyQ", "up:ShiftLeft"]);
  assert.equal(c.key("KeyQ").textContent, "q");
  c.tap(c.action("caps"));
  assert.equal(c.key("KeyQ").textContent, "Q");
  c.tap(c.key("ShiftLeft"));
  assert.equal(c.key("KeyQ").textContent, "q");
  c.tap(c.key("KeyQ"));
  assert.equal(c.key("KeyQ").textContent, "Q");
  assert.equal(c.key("CapsLock").getAttribute("aria-pressed"), "true");
});

test("language sends a single independent Shift tap even with Shift selected", () => {
  const c = controller();
  assert.equal(c.action("language").textContent, "中/英");
  c.tap(c.action("caps"));
  c.tap(c.key("ShiftLeft"));
  c.tap(c.key("ControlLeft"));
  c.messages.length = 0;
  c.tap(c.action("language"));
  assert.deepEqual(c.keys(), ["down:ShiftLeft", "up:ShiftLeft"]);
  assert.equal(c.action("language").textContent, "中/英");
  assert.equal(c.key("Comma").textContent, "，");
  assert.equal(c.key("Period").textContent, "。");
  assert.equal(c.key("CapsLock").getAttribute("aria-pressed"), "true");
  assert.equal(c.key("ShiftLeft").getAttribute("aria-pressed"), "false");
  c.messages.length = 0;
  c.tap(c.action("language"));
  assert.equal(c.action("language").textContent, "中/英");
  assert.deepEqual(c.keys(), ["down:ShiftLeft", "up:ShiftLeft"]);
});

test("numeric layer keeps Caps, Shift and F keys and sends the exact symbol combinations", () => {
  const c = controller();
  c.tap(c.action("caps"));
  c.tap(c.action("numbers"));
  assert.equal(c.document.querySelector("#number-layer").hidden, false);
  assert.equal(c.document.querySelector("#letter-layer").hidden, true);
  assert.equal(c.action("numbers").textContent, "ABC");
  assert.equal(c.key("CapsLock").getAttribute("aria-pressed"), "true");
  const symbols = [
    ["Digit2", true], ["Digit3", true], ["Digit4", true], ["Digit5", true],
    ["Digit7", true], ["Digit8", true], ["Digit9", true], ["Digit0", true],
    ["Minus", false], ["Equal", false], ["Slash", false], ["Semicolon", true],
    ["Semicolon", false], ["Digit1", true], ["Slash", true], ["Equal", true], ["Minus", true],
  ];
  for (const [code, shift] of symbols) {
    c.messages.length = 0;
    c.tap(c.key(code, shift));
    const expected = [`down:${code}`, `up:${code}`];
    if (shift) { expected.unshift("down:ShiftLeft"); expected.push("up:ShiftLeft"); }
    assert.deepEqual(c.keys(), expected);
  }
  for (let number = 1; number <= 12; number++) {
    c.messages.length = 0;
    c.tap(c.key(`F${number}`));
    assert.deepEqual(c.keys(), [`down:F${number}`, `up:F${number}`]);
  }
  c.tap(c.action("numbers"));
  assert.equal(c.key("KeyQ").textContent, "Q");
});

test("overlapping contacts on the same key do not release one another", () => {
  const c = controller();
  c.pointer(c.key("KeyQ"), "pointerdown", 1);
  c.pointer(c.key("KeyQ"), "pointerdown", 2);
  c.pointer(c.key("KeyQ"), "pointerup", 1);
  assert.deepEqual(c.keys(), ["down:KeyQ"]);
  c.pointer(c.key("KeyQ"), "pointerup", 2);
  assert.deepEqual(c.keys(), ["down:KeyQ", "up:KeyQ"]);
});

test("Ctrl applies to a mouse click and releases after it", () => {
  const c = controller();
  c.tap(c.key("ControlLeft"));
  c.tap(c.pad);
  assert.deepEqual(c.messages, [
    { t: "key", action: "down", code: "ControlLeft" },
    { t: "click", button: "left" },
    { t: "key", action: "up", code: "ControlLeft" },
  ]);
});

test("overlapping symbols that share a physical key each send their own stroke", () => {
  for (const firstShift of [true, false]) {
    const c = controller();
    c.tap(c.action("numbers"));
    c.messages.length = 0;
    c.pointer(c.key("Digit2", firstShift), "pointerdown", 1);
    c.pointer(c.key("Digit2", !firstShift), "pointerdown", 2);
    c.pointer(c.key("Digit2", firstShift), "pointerup", 1);
    assert.deepEqual(c.keys(), firstShift
      ? ["down:ShiftLeft", "down:Digit2", "up:Digit2", "up:ShiftLeft", "down:Digit2"]
      : ["down:Digit2", "up:Digit2", "down:ShiftLeft", "down:Digit2"]);
    c.pointer(c.key("Digit2", !firstShift), "pointerup", 2);
    assert.equal(c.keys().filter((key) => key === "up:Digit2").length, 2);
  }
});

test("keyboard and assistive click activation sends one full keystroke", () => {
  const c = controller();
  c.key("Enter").emit("click", { detail: 0 });
  assert.deepEqual(c.keys(), ["down:Enter", "up:Enter"]);
});

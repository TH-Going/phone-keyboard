(() => {
  "use strict";

  const pad = document.querySelector("#pad");
  const dot = document.querySelector("#touch-point");
  const remote = document.querySelector(".remote");
  const keyboard = document.querySelector(".keyboard");
  const mouse = document.querySelector(".mouse");
  const divider = document.querySelector(".mode-divider");
  const modeStatus = document.querySelector("#mode-status");
  const keyButtons = Array.from(document.querySelectorAll(".key"));
  const modes = ["mouse", "split", "keyboard"];
  const modeNames = ["鼠标模式", "键鼠同屏", "键盘模式"];
  let dividerDrag = null;

  const token = (() => {
    const parts = location.pathname.split("/").filter(Boolean);
    return parts[0] === "c" && parts[1] ? parts[1] : "";
  })();

  const haptic = (duration = 6) => {
    if (navigator.vibrate) navigator.vibrate(duration);
  };

  /* ------------------------------------------------------------------ */
  /* Mode switch                                                        */
  /* ------------------------------------------------------------------ */

  function setMode(mode) {
    if (!modes.includes(mode)) return;
    releaseAllInputs(true);
    remote.dataset.mode = mode;
    keyboard.hidden = mode === "mouse";
    mouse.hidden = mode === "keyboard";
    const index = modes.indexOf(mode);
    divider.setAttribute("aria-valuenow", String(index));
    divider.setAttribute("aria-valuetext", modeNames[index]);
    modeStatus.textContent = modeNames[index];
  }

  const nextMode = (mode, direction) => modes[Math.max(0, Math.min(2, modes.indexOf(mode) + direction))];
  const dragIsReady = (dx, dy) => Math.abs(dx) >= 42 && Math.abs(dx) > Math.abs(dy) * 1.2;

  function clearDividerDrag() {
    const previous = dividerDrag;
    dividerDrag = null;
    divider.classList.remove("is-dragging");
    divider.removeAttribute("data-ready");
    divider.style.removeProperty("--drag-x");
    if (previous && divider.hasPointerCapture(previous.pointerId)) {
      divider.releasePointerCapture(previous.pointerId);
    }
  }

  divider.addEventListener("pointerdown", (event) => {
    if (dividerDrag || event.button !== 0) return;
    event.preventDefault();
    releaseAllInputs(true);
    dividerDrag = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, mode: remote.dataset.mode };
    divider.setPointerCapture(event.pointerId);
    divider.classList.add("is-dragging");
  });
  divider.addEventListener("pointermove", (event) => {
    if (!dividerDrag || dividerDrag.pointerId !== event.pointerId) return;
    const dx = event.clientX - dividerDrag.x;
    const dy = event.clientY - dividerDrag.y;
    const canMove = nextMode(dividerDrag.mode, Math.sign(dx)) !== dividerDrag.mode;
    const offset = dx * (canMove ? .45 : .12);
    divider.style.setProperty("--drag-x", `${Math.max(-24, Math.min(24, offset))}px`);
    divider.dataset.ready = String(canMove && dragIsReady(dx, dy));
  });
  function finishDividerDrag(event) {
    if (!dividerDrag || dividerDrag.pointerId !== event.pointerId) return;
    const previous = dividerDrag;
    clearDividerDrag();
    const dx = event.clientX - previous.x;
    const dy = event.clientY - previous.y;
    if (event.type === "pointerup" && dragIsReady(dx, dy)) {
      setMode(nextMode(previous.mode, Math.sign(dx)));
      haptic(4);
    }
  }
  divider.addEventListener("pointerup", finishDividerDrag);
  divider.addEventListener("pointercancel", finishDividerDrag);
  divider.addEventListener("lostpointercapture", finishDividerDrag);
  divider.addEventListener("contextmenu", (event) => event.preventDefault());
  divider.addEventListener("keydown", (event) => {
    const direction = { ArrowLeft: -1, ArrowRight: 1 }[event.key];
    if (direction || event.key === "Home" || event.key === "End") {
      event.preventDefault();
      setMode(event.key === "Home" ? "mouse" : event.key === "End" ? "keyboard" : nextMode(remote.dataset.mode, direction));
    }
  });

  /* ------------------------------------------------------------------ */
  /* WebSocket                                                          */
  /* ------------------------------------------------------------------ */

  let socket = null;
  let reconnectTimer = 0;
  let reconnectDelay = 600;
  let unloading = false;

  function send(message) {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    try {
      socket.send(JSON.stringify(message));
      return true;
    } catch {
      return false;
    }
  }

  function connect() {
    if (!token || unloading) return;
    if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) return;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    try {
      socket = new WebSocket(`${scheme}//${location.host}/ws/${encodeURIComponent(token)}`);
    } catch {
      scheduleReconnect();
      return;
    }
    socket.addEventListener("open", () => {
      reconnectDelay = 600;
      releaseAllInputs(true);
    });
    socket.addEventListener("close", () => {
      socket = null;
      resetLocalState();
      if (!unloading) scheduleReconnect();
    });
    socket.addEventListener("error", () => {
      if (socket) socket.close();
    });
  }

  function scheduleReconnect() {
    window.clearTimeout(reconnectTimer);
    reconnectTimer = window.setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(Math.round(reconnectDelay * 1.7), 5000);
  }

  function releaseAllInputs(notifyServer = false) {
    if (notifyServer) send({ t: "release" });
    resetLocalState();
  }

  window.addEventListener("online", () => {
    reconnectDelay = 300;
    connect();
  });
  window.addEventListener("offline", () => {
    releaseAllInputs(true);
    if (socket) socket.close();
  });
  window.addEventListener("beforeunload", () => {
    unloading = true;
    releaseAllInputs(true);
    if (socket) socket.close();
  });
  window.addEventListener("pagehide", () => releaseAllInputs(true));
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) releaseAllInputs(true);
  });
  window.addEventListener("blur", () => releaseAllInputs(true));

  /* ------------------------------------------------------------------ */
  /* Keyboard                                                           */
  /* ------------------------------------------------------------------ */

  const heldKeys = new Map();
  const modifiers = new Set();
  const activeModifiers = new Set();
  const keyPointers = new Map();
  let capsLock = false;
  let language = "en";
  let numbers = false;

  const MODIFIER_CODE = {
    shift: "ShiftLeft",
    control: "ControlLeft",
    alt: "AltLeft",
    meta: "MetaLeft",
  };

  function modifierGroup(code) {
    return Object.keys(MODIFIER_CODE).find((group) => MODIFIER_CODE[group] === code) || "";
  }

  function renderKeyboard() {
    const uppercase = capsLock !== modifiers.has("shift");
    document.querySelector("#letter-layer").hidden = numbers;
    document.querySelector("#number-layer").hidden = !numbers;
    keyButtons.forEach((button) => {
      const code = button.dataset.code;
      const group = modifierGroup(code);
      if (group) button.setAttribute("aria-pressed", String(modifiers.has(group)));
      if (button.dataset.letter) button.textContent = uppercase ? button.dataset.letter.toUpperCase() : button.dataset.letter;
      if (code === "CapsLock") button.setAttribute("aria-pressed", String(capsLock));
      if (code === "Comma") button.textContent = language === "zh" ? "，" : ",";
      if (code === "Period") button.textContent = language === "zh" ? "。" : ".";
      if (button.dataset.action === "numbers") {
        button.textContent = numbers ? (uppercase ? "ABC" : "abc") : "123";
        button.setAttribute("aria-label", numbers ? "切换到字母键盘" : "切换到数字和符号");
      }
    });
  }

  // Latch the intent locally. Sending Shift alone here would also toggle the
  // PC input method; send modifiers only together with the next input action.
  function syncModifiers(required = modifiers) {
    for (const group of activeModifiers) {
      if (!required.has(group)) {
        if (!send({ t: "key", action: "up", code: MODIFIER_CODE[group] })) return false;
        activeModifiers.delete(group);
      }
    }
    for (const group of required) {
      if (!activeModifiers.has(group)) {
        if (!send({ t: "key", action: "down", code: MODIFIER_CODE[group] })) return false;
        activeModifiers.add(group);
      }
    }
    return true;
  }

  function releaseModifiers() {
    syncModifiers(new Set());
    activeModifiers.clear();
    modifiers.clear();
    renderKeyboard();
  }

  function toggleModifier(group) {
    if (modifiers.has(group)) {
      modifiers.delete(group);
      if (activeModifiers.has(group)) {
        send({ t: "key", action: "up", code: MODIFIER_CODE[group] });
        activeModifiers.delete(group);
      }
    } else {
      modifiers.add(group);
    }
    renderKeyboard();
    haptic(7);
  }

  function tapKey(code) {
    if (!send({ t: "key", action: "down", code })) return false;
    if (send({ t: "key", action: "up", code })) return true;
    releaseAllInputs(true);
    return false;
  }

  function performKeyAction(action) {
    if (action === "numbers") {
      const selected = new Set(modifiers);
      releaseAllInputs(true);
      selected.forEach((group) => modifiers.add(group));
      numbers = !numbers;
    } else if (action === "language") {
      releaseAllInputs(true);
      if (tapKey("ShiftLeft")) language = language === "en" ? "zh" : "en";
    } else if (action === "caps") {
      if (heldKeys.size || activeModifiers.size) releaseAllInputs(true);
      if (tapKey("CapsLock")) capsLock = !capsLock;
    }
    renderKeyboard();
  }

  function resetLocalState() {
    const keyCaptures = Array.from(keyPointers.entries());
    const padCaptures = Array.from(pointers.keys());
    keyPointers.clear();
    heldKeys.clear();
    modifiers.clear();
    activeModifiers.clear();
    pointers.clear();
    gesture = null;
    clearLongPress();
    clearDividerDrag();
    keyButtons.forEach((button) => button.classList.remove("is-pressed"));
    pad.classList.remove("is-touching", "is-dragging");
    moveX = moveY = wheelX = wheelY = 0;
    if (flushTimer) window.clearTimeout(flushTimer);
    flushTimer = 0;
    if (dotFrame) cancelAnimationFrame(dotFrame);
    dotFrame = 0;
    lastFlushAt = 0;
    padRect = null;
    keyCaptures.forEach(([id, entry]) => {
      if (entry.element.hasPointerCapture(id)) entry.element.releasePointerCapture(id);
    });
    padCaptures.forEach((id) => {
      if (pad.hasPointerCapture(id)) pad.releasePointerCapture(id);
    });
    renderKeyboard();
  }

  function startKey(button, event) {
    if (event.pointerType === "mouse" && event.button !== 0) return;
    if (keyPointers.has(event.pointerId)) return;
    event.preventDefault();
    if (event.pointerType !== "keyboard") button.setPointerCapture(event.pointerId);
    const code = button.dataset.code;
    const entry = { element: button, code, down: false };
    keyPointers.set(event.pointerId, entry);
    button.classList.add("is-pressed");
    haptic(5);
    if (modifierGroup(code) || button.dataset.action) return;

    const required = new Set(modifiers);
    // Symbol labels name an exact character, including on the numeric layer.
    if (button.dataset.shift === "true") required.add("shift");
    if (button.dataset.shift === "false") required.delete("shift");
    const count = heldKeys.get(code) || 0;
    const differentSymbol = count > 0 && button.dataset.shift !== undefined &&
      Array.from(keyPointers.values()).some((other) => other.down && other.code === code &&
        other.element.dataset.shift !== button.dataset.shift);
    // Two visible symbols can share a physical key (for example 2 and @).
    // Finish the previous stroke before changing Shift and sending the next.
    if (differentSymbol && !send({ t: "key", action: "up", code })) {
      releaseAllInputs(true);
      return;
    }
    if (!syncModifiers(required)) {
      releaseAllInputs(true);
      return;
    }
    if ((!count || differentSymbol) && !send({ t: "key", action: "down", code })) {
      releaseAllInputs(true);
      return;
    }
    heldKeys.set(code, count + 1);
    entry.down = true;
  }

  function finishKey(event) {
    const entry = keyPointers.get(event.pointerId);
    if (!entry) return;
    keyPointers.delete(event.pointerId);
    const button = entry.element;
    button.classList.toggle("is-pressed", Array.from(keyPointers.values()).some((other) => other.element === button));
    if (entry.down) {
      const count = heldKeys.get(entry.code) || 0;
      if (count > 1) {
        heldKeys.set(entry.code, count - 1);
      } else {
        heldKeys.delete(entry.code);
        send({ t: "key", action: "up", code: entry.code });
      }
      if (!heldKeys.size && !gesture?.dragging) releaseModifiers();
    } else if (event.type === "pointerup") {
      if (button.dataset.action) performKeyAction(button.dataset.action);
      else toggleModifier(modifierGroup(entry.code));
    }
  }

  keyButtons.forEach((button) => {
    button.addEventListener("pointerdown", (event) => startKey(button, event));
    button.addEventListener("pointerup", finishKey);
    button.addEventListener("pointercancel", finishKey);
    button.addEventListener("lostpointercapture", finishKey);
    button.addEventListener("contextmenu", (event) => event.preventDefault());
    button.addEventListener("click", (event) => {
      if (event.detail !== 0) return;
      startKey(button, { pointerId: -1, pointerType: "keyboard", preventDefault() {} });
      finishKey({ pointerId: -1, type: "pointerup" });
    });
  });

  /* ------------------------------------------------------------------ */
  /* Touchpad gestures                                                  */
  /* ------------------------------------------------------------------ */

  const MOVE_SCALE = 1.95;
  const WHEEL_SCALE = 2.4;
  const MOVE_START = 1.5;
  const TAP_SLOP = 6;
  const LONG_PRESS_MS = 450;
  const FLUSH_INTERVAL_MS = 8;
  const pointers = new Map();
  let gesture = null;
  let longPressTimer = 0;
  let moveX = 0;
  let moveY = 0;
  let wheelX = 0;
  let wheelY = 0;
  let flushTimer = 0;
  let lastFlushAt = 0;
  let padRect = null;
  let dotX = 0;
  let dotY = 0;
  let dotFrame = 0;

  function scheduleFlush() {
    if (flushTimer) return;
    const wait = FLUSH_INTERVAL_MS - (performance.now() - lastFlushAt);
    if (wait <= 0) flush();
    else flushTimer = window.setTimeout(flush, wait);
  }

  function flush() {
    if (flushTimer) window.clearTimeout(flushTimer);
    flushTimer = 0;
    lastFlushAt = performance.now();
    if (moveX || moveY) {
      const dx = Math.trunc(moveX);
      const dy = Math.trunc(moveY);
      moveX -= dx;
      moveY -= dy;
      if (dx || dy) send({ t: "move", dx, dy });
    }
    if (wheelX || wheelY) {
      const dx = Math.trunc(wheelX / 120) * 120;
      const dy = Math.trunc(wheelY / 120) * 120;
      if (dx || dy) {
        if (syncModifiers()) send({ t: "wheel", dx, dy });
        wheelX -= dx;
        wheelY -= dy;
      }
    }
  }

  function point(clientX, clientY) {
    const rect = padRect || (padRect = pad.getBoundingClientRect());
    return { x: clientX - rect.left, y: clientY - rect.top };
  }

  function moveDot(x, y) {
    dotX = x;
    dotY = y;
    if (dotFrame) return;
    dotFrame = requestAnimationFrame(() => {
      dotFrame = 0;
      dot.style.transform = `translate3d(${dotX - 12}px, ${dotY - 12}px, 0)`;
    });
  }

  function midpoint() {
    let x = 0;
    let y = 0;
    pointers.forEach((p) => { x += p.x; y += p.y; });
    return { x: x / pointers.size, y: y / pointers.size };
  }

  function clearLongPress() {
    if (longPressTimer) window.clearTimeout(longPressTimer);
    longPressTimer = 0;
  }

  function startLongPress() {
    const pending = gesture;
    clearLongPress();
    longPressTimer = window.setTimeout(() => {
      longPressTimer = 0;
      if (gesture !== pending || pointers.size !== 1 || gesture.maxPointers !== 1 || gesture.moved) return;
      flush();
      if (!syncModifiers() || !send({ t: "button", button: "left", down: true })) {
        releaseAllInputs(true);
        return;
      }
      gesture.dragging = true;
      gesture.moved = true;
      gesture.moveStarted = false;
      // Ignore the small finger jitter before activation: the PC cursor must
      // stay on the window border where the user placed it.
      pointers.forEach((p) => { p.startX = p.x; p.startY = p.y; });
      pad.classList.add("is-dragging");
      haptic(15);
    }, LONG_PRESS_MS);
  }

  function endMouseDrag() {
    if (!gesture?.dragging) return false;
    flush();
    send({ t: "button", button: "left", down: false });
    gesture.dragging = false;
    pad.classList.remove("is-dragging");
    if (!heldKeys.size) releaseModifiers();
    return true;
  }

  pad.addEventListener("pointerdown", (event) => {
    if (event.pointerType === "mouse" && event.button !== 0) return;
    if (pointers.has(event.pointerId)) return;
    event.preventDefault();
    pad.setPointerCapture(event.pointerId);
    if (pointers.size === 0) {
      flush();
      moveX = moveY = wheelX = wheelY = 0;
      padRect = pad.getBoundingClientRect();
    }
    const local = point(event.clientX, event.clientY);
    pointers.set(event.pointerId, { x: local.x, y: local.y, startX: local.x, startY: local.y });
    if (pointers.size === 1) {
      gesture = {
        startTime: performance.now(), moved: false, moveStarted: false,
        dragging: false, suppressed: false, maxPointers: 1,
        twoStartTime: 0, scrollMoved: false, lastMid: null,
      };
      pad.classList.add("is-touching");
      moveDot(local.x, local.y);
      startLongPress();
    } else if (gesture) {
      clearLongPress();
      if (endMouseDrag()) gesture.suppressed = true;
      gesture.maxPointers = Math.max(gesture.maxPointers, pointers.size);
      if (pointers.size === 2) {
        gesture.twoStartTime = performance.now();
        gesture.lastMid = gesture.twoStartPoint = midpoint();
        moveDot(gesture.lastMid.x, gesture.lastMid.y);
      } else {
        gesture.suppressed = true;
      }
    }
  });

  function movePointer(event) {
    const tracked = pointers.get(event.pointerId);
    if (!tracked || !gesture) return;
    const local = point(event.clientX, event.clientY);
    const dx = local.x - tracked.x;
    const dy = local.y - tracked.y;
    tracked.x = local.x;
    tracked.y = local.y;
    if (gesture.suppressed) return;

    if (pointers.size === 1 && gesture.maxPointers === 1) {
      const totalX = local.x - tracked.startX;
      const totalY = local.y - tracked.startY;
      const distance = Math.hypot(totalX, totalY);
      if (!gesture.dragging && distance > TAP_SLOP) {
        clearLongPress();
        gesture.moved = true;
      }
      if (!gesture.moveStarted) {
        if (distance > (gesture.dragging ? MOVE_START : TAP_SLOP)) {
          gesture.moveStarted = true;
          // Crossing the long-press dead zone must not jump the cursor by
          // the entire 6px, especially when aiming at a window border.
          const scale = MOVE_SCALE * (gesture.dragging ? 1 : (distance - TAP_SLOP) / distance);
          moveX += totalX * scale;
          moveY += totalY * scale;
          scheduleFlush();
        }
      } else {
        moveX += dx * MOVE_SCALE;
        moveY += dy * MOVE_SCALE;
        scheduleFlush();
      }
      moveDot(local.x, local.y);
    } else if (pointers.size === 2) {
      const mid = midpoint();
      if (gesture.lastMid) {
        wheelX += (mid.x - gesture.lastMid.x) * WHEEL_SCALE;
        wheelY += (mid.y - gesture.lastMid.y) * WHEEL_SCALE;
        if (Math.hypot(mid.x - gesture.twoStartPoint.x, mid.y - gesture.twoStartPoint.y) > TAP_SLOP) {
          gesture.scrollMoved = true;
        }
        scheduleFlush();
      }
      gesture.lastMid = mid;
      moveDot(mid.x, mid.y);
    }
  }
  pad.addEventListener("pointermove", movePointer);

  function finishPointer(event, allowTap = false) {
    if (!pointers.has(event.pointerId) || !gesture) return;
    if (!allowTap) {
      releaseAllInputs(true);
      return;
    }
    movePointer(event);
    clearLongPress();
    pointers.delete(event.pointerId);
    const wasDragging = endMouseDrag();
    if (pointers.size > 0) return;

    const elapsed = performance.now() - gesture.startTime;
    const wasTap = !gesture.suppressed && gesture.maxPointers === 1 && !gesture.moved && elapsed < LONG_PRESS_MS;
    const wasTwoFingerTap = !gesture.suppressed && gesture.maxPointers === 2 && !gesture.scrollMoved &&
      !gesture.moved && performance.now() - gesture.twoStartTime < 360;
    flush();
    if (wasTap || wasTwoFingerTap) {
      if (syncModifiers()) send({ t: "click", button: wasTap ? "left" : "right" });
      makeRipple();
      haptic(8);
    }
    if (!heldKeys.size && (wasDragging || wasTap || wasTwoFingerTap || activeModifiers.size)) releaseModifiers();
    pad.classList.remove("is-touching", "is-dragging");
    gesture = null;
    moveX = moveY = wheelX = wheelY = 0;
  }

  pad.addEventListener("pointerup", (event) => finishPointer(event, true));
  pad.addEventListener("pointercancel", (event) => finishPointer(event, false));
  pad.addEventListener("lostpointercapture", (event) => finishPointer(event, false));
  pad.addEventListener("contextmenu", (event) => event.preventDefault());
  window.addEventListener("resize", () => releaseAllInputs(true), { passive: true });
  window.addEventListener("orientationchange", () => releaseAllInputs(true), { passive: true });

  function makeRipple() {
    const ripple = document.createElement("span");
    ripple.className = "tap-ripple";
    ripple.style.left = `${dotX}px`;
    ripple.style.top = `${dotY}px`;
    pad.appendChild(ripple);
    window.setTimeout(() => ripple.remove(), 400);
  }

  /* ------------------------------------------------------------------ */
  /* Start                                                              */
  /* ------------------------------------------------------------------ */

  setMode(remote.dataset.mode || "split");
  connect();
})();

var BUTTONS = ["a", "b", "x", "y", "lb", "rb", "ls", "rs", "menu", "view", "du", "dd", "dl", "dr"];

function emptySnapshot() {
  return {
    service: "wolverine-dashboard",
    phase: "stopped",
    capturing: false,
    usb: "not_reading",
    usb_detail: "",
    audio_note: "",
    handshake: "idle",
    firmware: null,
    report_count: 0,
    last_input_age_ms: null,
    last_guide_age_ms: null,
    input: null,
    guide: null,
    extrema: {
      lx: { min: null, max: null },
      ly: { min: null, max: null },
      rx: { min: null, max: null },
      ry: { min: null, max: null },
      lt: { min: null, max: null },
      rt: { min: null, max: null }
    },
    logs: [],
    exit_code: null,
    error: null,
    replug_count: 0,
    link: "ok",
    stale_after_ms: 400
  };
}

function stickPercent(value) {
  if (value < 0) return (value / 32768) * 100;
  return (value / 32767) * 100;
}

function triggerPercent(value) {
  return (value / 1023) * 100;
}

function fmtPct(value, kind) {
  var n = kind === "trig" ? triggerPercent(value) : stickPercent(value);
  if (n === 0) return "0.0%";
  var body = n.toFixed(1) + "%";
  return n > 0 ? "+" + body : body;
}

function fmtAge(ms) {
  if (ms == null) return "—";
  if (ms < 1000) return ms + " ms";
  return (ms / 1000).toFixed(1) + " s";
}

function isFresh(age, snap) {
  return snap.link !== "down"
    && !!snap.capturing
    && snap.phase !== "stopped"
    && snap.phase !== "error"
    && snap.phase !== "reconnecting"
    && typeof age === "number"
    && age <= (snap.stale_after_ms || 400);
}

function guideDown(value) {
  return typeof value === "number" && value !== 0;
}

function buttonDown(value) {
  return value === 1 || value === true;
}

function presentation(snapshot) {
  var snap = snapshot || emptySnapshot();
  var inputFresh = !!(isFresh(snap.last_input_age_ms, snap) && snap.input && snap.phase === "live");
  var guideFresh = isFresh(snap.last_guide_age_ms, snap);
  var sticks = "blank";
  if (snap.input && inputFresh) sticks = "live";
  else if (snap.input) sticks = "held";
  var lit = {};
  var i;
  for (i = 0; i < BUTTONS.length; i++) {
    lit[BUTTONS[i]] = !!(inputFresh && buttonDown(snap.input[BUTTONS[i]]));
  }
  var linkDown = snap.link === "down";
  var mode = "stopped";
  var chip = "Stopped";
  if (linkDown) {
    mode = "down";
    chip = "Link lost";
  } else if (snap.phase === "error") {
    mode = "error";
    chip = "Error";
  } else if (snap.phase === "reconnecting") {
    mode = "reconnecting";
    chip = "Replug";
  } else if (snap.phase === "starting" || snap.phase === "waiting") {
    mode = "waiting";
    chip = "Waiting";
  } else if (snap.phase === "live" && inputFresh) {
    mode = "live";
    chip = "Live";
  } else if (snap.phase === "live" || sticks === "held") {
    mode = "held";
    chip = "Last received";
  }
  var handshake = "—";
  if (snap.handshake === "starting") handshake = "Starting";
  else if (snap.handshake === "failed") handshake = "Failed";
  else if (snap.handshake === "complete" && !snap.input && snap.capturing && snap.phase !== "error") handshake = "Complete, waiting for input";
  else if (snap.handshake === "complete") handshake = "Complete";
  var usb = "Not reading";
  var usbState = "";
  if (snap.usb === "open") usb = "Open";
  else if (snap.usb === "absent") { usb = "Not found"; usbState = "bad"; }
  else if (snap.usb === "claim_failed") { usb = "Claim failed"; usbState = "bad"; }
  else if (snap.usb === "lost") { usb = "Lost"; usbState = "wait"; }
  var banner = "";
  var bannerKind = "wait";
  if (linkDown) {
    banner = "Dashboard link lost. Numbers below are last received. This is not a USB disconnect.";
    bannerKind = "held";
  } else if (snap.phase === "error") {
    banner = snap.error || "Reader error.";
    bannerKind = "bad";
  } else if (snap.phase === "reconnecting") {
    banner = "Controller lost. Readings cleared. Waiting for replug.";
  } else if (snap.capturing && snap.replug_count > 0 && !snap.input) {
    banner = "Replug detected. Waiting for a new full input report.";
  } else if (snap.capturing && snap.phase === "waiting" && snap.handshake === "complete") {
    banner = "USB handshake complete. Move a stick or press a button to send input.";
  } else if (snap.capturing && (snap.phase === "waiting" || snap.phase === "starting")) {
    banner = "Reading USB. Waiting for a full input report.";
  } else if (sticks === "held" && snap.capturing) {
    banner = "No new input reports. Showing the last received measurements.";
    bannerKind = "held";
  } else if (!snap.capturing && snap.phase === "stopped" && snap.exit_code != null && snap.exit_code !== 0) {
    banner = "Reader exited " + snap.exit_code + ".";
    bannerKind = "bad";
  }
  return {
    mode: mode,
    chip: chip,
    banner: banner,
    bannerKind: bannerKind,
    handshake: handshake,
    usb: usb,
    usbState: usbState,
    usbDetail: snap.usb_detail || "",
    sticks: sticks,
    guideLit: !!(guideFresh && guideDown(snap.guide)),
    lit: lit,
    reportCount: snap.report_count || 0
  };
}

function axisText(snap, name, kind) {
  if (!snap.input) return { raw: "—", pct: "—", min: "—", max: "—" };
  var slot = (snap.extrema && snap.extrema[name]) || {};
  return {
    raw: String(snap.input[name]),
    pct: fmtPct(snap.input[name], kind),
    min: slot.min == null ? "—" : String(slot.min),
    max: slot.max == null ? "—" : String(slot.max)
  };
}

function panelHTML() {
  var keys = [
    ["a", "A"], ["b", "B"], ["x", "X"], ["y", "Y"],
    ["lb", "LB"], ["rb", "RB"], ["lt", "LT"], ["rt", "RT"],
    ["view", "View"], ["menu", "Menu"], ["ls", "LS"], ["rs", "RS"],
    ["du", "Up"], ["dd", "Down"], ["dl", "Left"], ["dr", "Right"], ["guide", "Guide"]
  ];
  return [
    '<article class="panel mode-stopped">',
    '<header class="top"><div><p class="kicker">Wolverine / Input monitor</p>',
    '<h1>Razer Wolverine V3 Pro</h1><p class="subtitle">Wired USB · 1532:0a3f</p></div>',
    '<div class="actions"><p class="chip" data-text="chip" aria-live="polite">Stopped</p>',
    '<button class="start" type="button">Start reading</button><button class="stop" type="button">Stop</button></div></header>',
    '<div class="workspace"><p class="banner" data-text="banner" aria-live="polite"></p>',
    '<section class="stats">', stat("USB connection", "usb"), stat("Handshake", "handshake"),
    stat("Firmware", "firmware"), stat("Input reports", "reports"), stat("Last report", "age"), '</section>',
    '<p class="audio" data-text="audio"></p>',
    '<div class="tester"><div class="measurements">',
    '<div class="section-heading"><h2>Buttons</h2><span>0 released / 1 pressed</span></div>',
    '<section class="button-grid">', keys.map(function (pair, i) {
      return '<div class="key" data-btn="' + pair[0] + '" aria-label="' + pair[1] + ', no data"><small>B' + i + '</small><b>' + pair[1] + '</b><span data-value>—</span></div>';
    }).join(''), '</section>',
    '<section class="sticks">', stickCard("l", "Left stick", "lx", "ly"), stickCard("r", "Right stick", "rx", "ry"), '</section>',
    '<section class="triggers">', trigCard("lt", "Left trigger / LT"), trigCard("rt", "Right trigger / RT"), '</section></div>',
    '<section class="controller-view"><div class="section-heading"><h2>Controller</h2><span class="legend"><i></i> Pressed</span></div>',
    controllerDrawing(), '<p class="drawing-status" data-text="drawingStatus">Waiting for controller data</p>',
    '<p class="diagram-note">Sticks move with your input. Trigger fill shows pressure.</p></section></div>',
    '<details class="logwrap"><summary>USB activity log</summary><ol class="log" data-log></ol></details>',
    '<footer class="fineprint"><span>Direct USB readings</span><span>Stick range −32768 to 32767 · Triggers 0 to 1023</span></footer>',
    '</div></article>'
  ].join('');
}

function controllerDrawing() {
  return `<svg class="controller" viewBox="0 0 560 410" role="img" aria-label="Controller diagram showing stick positions and pressed buttons">
    <path class="body-shadow" d="M165 119 H395 Q440 119 461 163 Q489 224 508 318 Q520 381 485 388 Q462 393 438 364 L393 314 Q381 301 357 301 H203 Q179 301 167 314 L122 364 Q98 393 75 388 Q40 381 52 318 Q71 224 99 163 Q120 119 165 119 Z"/>
    <path class="body-outline" d="M165 107 H395 Q440 107 461 151 Q489 212 508 306 Q520 369 485 376 Q462 381 438 352 L393 302 Q381 289 357 289 H203 Q179 289 167 302 L122 352 Q98 381 75 376 Q40 369 52 306 Q71 212 99 151 Q120 107 165 107 Z"/>
    <path class="body-seam" d="M73 226 L172 309 M487 226 L388 309 M151 113 Q125 129 124 147 M409 113 Q435 129 436 147"/>
    <rect class="trigger-outline" x="116" y="12" width="44" height="58" rx="14"/>
    <rect class="trigger-pressure" data-diagram-trigger="lt" x="120" y="66" width="36" height="0" rx="9"/>
    <text class="trigger-label" x="138" y="47">LT</text>
    <rect class="trigger-outline" x="400" y="12" width="44" height="58" rx="14"/>
    <rect class="trigger-pressure" data-diagram-trigger="rt" x="404" y="66" width="36" height="0" rx="9"/>
    <text class="trigger-label" x="422" y="47">RT</text>
    <g class="control" data-diagram-btn="lb"><rect x="105" y="82" width="78" height="17" rx="8"/><text x="144" y="95">LB</text></g>
    <g class="control" data-diagram-btn="rb"><rect x="377" y="82" width="78" height="17" rx="8"/><text x="416" y="95">RB</text></g>
    <circle class="stick-well" cx="156" cy="183" r="43"/>
    <g data-diagram-stick="l"><g class="control stick-cap" data-diagram-btn="ls"><circle cx="156" cy="183" r="31"/><text x="156" y="188">L</text></g></g>
    <circle class="stick-well" cx="351" cy="260" r="43"/>
    <g data-diagram-stick="r"><g class="control stick-cap" data-diagram-btn="rs"><circle cx="351" cy="260" r="31"/><text x="351" y="265">R</text></g></g>
    <circle class="button-well" cx="405" cy="183" r="48"/>
    <g class="control" data-diagram-btn="y"><circle cx="405" cy="154" r="15"/><text x="405" y="159">Y</text></g>
    <g class="control" data-diagram-btn="x"><circle cx="376" cy="183" r="15"/><text x="376" y="188">X</text></g>
    <g class="control" data-diagram-btn="b"><circle cx="434" cy="183" r="15"/><text x="434" y="188">B</text></g>
    <g class="control" data-diagram-btn="a"><circle cx="405" cy="212" r="15"/><text x="405" y="217">A</text></g>
    <circle class="button-well" cx="210" cy="260" r="43"/>
    <g class="control dpad-key" data-diagram-btn="du"><path d="M199 233 Q210 229 221 233 V249 L210 258 L199 249 Z"/></g>
    <g class="control dpad-key" data-diagram-btn="dd"><path d="M199 287 Q210 291 221 287 V271 L210 262 L199 271 Z"/></g>
    <g class="control dpad-key" data-diagram-btn="dl"><path d="M183 249 Q179 260 183 271 H199 L208 260 L199 249 Z"/></g>
    <g class="control dpad-key" data-diagram-btn="dr"><path d="M237 249 Q241 260 237 271 H221 L212 260 L221 249 Z"/></g>
    <g class="control system-key" data-diagram-btn="view"><circle cx="238" cy="189" r="12"/><path d="M233 185 H241 V191 H233 Z M236 182 H244 V188"/></g>
    <g class="control system-key" data-diagram-btn="menu"><circle cx="321" cy="189" r="12"/><path d="M316 185 H326 M316 189 H326 M316 193 H326"/></g>
    <g class="control guide-key" data-diagram-btn="guide"><circle cx="280" cy="153" r="18"/><text x="280" y="158">G</text></g>
    <text class="device-mark" x="280" y="236">WOLVERINE</text>
  </svg>`;
}

function paintController(root, snap, view) {
  root.querySelectorAll('[data-diagram-btn]').forEach(function (el) {
    var name = el.dataset.diagramBtn;
    el.classList.toggle('on', name === 'guide' ? view.guideLit : !!view.lit[name]);
  });
  ['l', 'r'].forEach(function (side) {
    var node = root.querySelector('[data-diagram-stick="' + side + '"]');
    var x = snap.input ? stickPercent(snap.input[side + 'x']) * 0.14 : 0;
    var y = snap.input ? -stickPercent(snap.input[side + 'y']) * 0.14 : 0;
    node.setAttribute('transform', 'translate(' + x + ' ' + y + ')');
    node.classList.toggle('unread', !snap.input);
  });
  ['lt', 'rt'].forEach(function (name) {
    var value = snap.input ? Math.max(0, Math.min(1, snap.input[name] / 1023)) : 0;
    var bar = root.querySelector('[data-diagram-trigger="' + name + '"]');
    bar.setAttribute('y', 66 - 50 * value);
    bar.setAttribute('height', 50 * value);
    var reading = root.querySelector('[data-btn="' + name + '"]');
    reading.querySelector('[data-value]').textContent = snap.input ? value.toFixed(2) : '—';
    reading.classList.toggle('on', view.sticks === 'live' && value > 0);
  });
  textInto(root, 'drawingStatus', view.sticks === 'blank' ? 'Waiting for controller data' : (view.sticks === 'held' ? 'Showing the last received position' : 'Receiving controller input'));
}

function stat(label, name) {
  return '<div class="stat" data-stat="' + name + '"><span>' + label + '</span><strong data-text="' + name + '">—</strong><em data-text="' + name + 'Detail"></em></div>';
}

function stickCard(side, title, xName, yName) {
  return [
    '<article class="stick" data-stick-card="' + side + '">',
    '<div class="plot"><canvas data-stick="' + side + '" aria-hidden="true"></canvas>',
    '<p class="plot-caption" data-text="' + side + 'cap">No data</p></div>',
    "<div>",
    '<span class="section-label">' + title + "</span>",
    axisBlock(xName, "X"),
    axisBlock(yName, "Y"),
    '<p class="extent"><span>min <b data-text="' + xName + 'Min">—</b> / <b data-text="' + yName + 'Min">—</b></span>',
    '<span>max <b data-text="' + xName + 'Max">—</b> / <b data-text="' + yName + 'Max">—</b></span></p>',
    '<p class="extent">signed raw −32768…32767</p>',
    "</div></article>"
  ].join("");
}

function axisBlock(name, label) {
  return '<p class="axis"><i>' + label + '</i><b data-text="' + name + '">—</b><small data-text="' + name + 'Pct">—</small></p>';
}

function trigCard(name, label) {
  return [
    '<article class="trig" data-trig="' + name + '">',
    '<div class="trig-head"><span class="trig-name">' + label + '</span><span data-text="' + name + 'Pct">—</span></div>',
    '<div class="track"><div class="fill" data-fill="' + name + '"></div></div>',
    '<div class="trig-read"><b data-text="' + name + '">—</b><span data-text="' + name + 'Unit"></span></div>',
    '<p class="extent"><span>min <b data-text="' + name + 'Min">—</b></span><span>max <b data-text="' + name + 'Max">—</b></span></p>',
    "</article>"
  ].join("");
}

function mount(root, options) {
  var opts = options || {};
  root.innerHTML = panelHTML();
  if (!opts.controls) {
    var actions = root.querySelector(".actions");
    if (actions) actions.classList.add("is-hidden");
  }
  render(emptySnapshot(), root);
}

function textInto(root, name, value) {
  var el = root.querySelector('[data-text="' + name + '"]');
  if (el) el.textContent = value;
}

function render(snapshot, root) {
  var node = root || document.getElementById("app");
  if (!node || !node.querySelector("[data-stick='l']")) return null;
  var snap = snapshot || emptySnapshot();
  var view = presentation(snap);
  var panel = node.querySelector(".panel");
  panel.className = "panel mode-" + view.mode;
  textInto(node, "chip", view.chip);
  var banner = node.querySelector("[data-text='banner']");
  banner.textContent = view.banner;
  banner.className = "banner" + (view.banner ? " on " + view.bannerKind : "");
  textInto(node, "usb", view.usb);
  textInto(node, "usbDetail", view.usbDetail);
  var usbStat = node.querySelector("[data-stat='usb']");
  if (usbStat) usbStat.className = "stat " + view.usbState;
  textInto(node, "handshake", view.handshake);
  var hs = node.querySelector("[data-stat='handshake']");
  if (hs) hs.className = "stat" + (snap.handshake === "failed" ? " bad" : (view.mode === "waiting" ? " wait" : ""));
  textInto(node, "firmware", snap.firmware || "—");
  textInto(node, "reports", String(view.reportCount));
  textInto(node, "age", fmtAge(snap.last_input_age_ms));
  textInto(node, "audio", snap.audio_note || "");
  paintStick(node, "l", snap, view, "lx", "ly");
  paintStick(node, "r", snap, view, "rx", "ry");
  paintTrig(node, snap, view, "lt");
  paintTrig(node, snap, view, "rt");
  var n;
  for (n = 0; n < BUTTONS.length; n++) paintKey(node, snap, view, BUTTONS[n]);
  paintGuide(node, snap, view);
  paintController(node, snap, view);
  paintLog(node, snap.logs || []);
  var start = node.querySelector(".start");
  var stop = node.querySelector(".stop");
  if (start && stop && !start.closest(".is-hidden")) {
    if (snap.link === "down") {
      start.disabled = false;
      stop.disabled = false;
    } else {
      start.disabled = !!snap.capturing;
      stop.disabled = !snap.capturing;
    }
  }
  return view;
}

function paintStick(root, side, snap, view, xName, yName) {
  var card = root.querySelector("[data-stick-card='" + side + "']");
  card.className = "stick" + (view.sticks === "held" ? " held" : "");
  var x = axisText(snap, xName, "stick");
  var y = axisText(snap, yName, "stick");
  textInto(root, xName, x.raw);
  textInto(root, xName + "Pct", x.pct);
  textInto(root, yName, y.raw);
  textInto(root, yName + "Pct", y.pct);
  textInto(root, xName + "Min", x.min);
  textInto(root, xName + "Max", x.max);
  textInto(root, yName + "Min", y.min);
  textInto(root, yName + "Max", y.max);
  textInto(root, side + "cap", view.sticks === "blank" ? "No data" : (view.sticks === "held" ? "Last received" : "Live"));
  drawStick(root.querySelector("[data-stick='" + side + "']"), snap.input && snap.input[xName], snap.input && snap.input[yName], view.sticks, snap.phase);
}

function paintTrig(root, snap, view, name) {
  var card = root.querySelector("[data-trig='" + name + "']");
  var reading = axisText(snap, name, "trig");
  card.className = "trig" + (view.sticks === "held" ? " held" : "");
  textInto(root, name, view.sticks === "blank" ? "—" : reading.raw);
  textInto(root, name + "Pct", view.sticks === "blank" ? "—" : reading.pct);
  textInto(root, name + "Unit", view.sticks === "blank" ? "" : "/1023");
  textInto(root, name + "Min", reading.min);
  textInto(root, name + "Max", reading.max);
  var fill = root.querySelector("[data-fill='" + name + "']");
  var width = 0;
  if (view.sticks !== "blank" && snap.input) width = Math.max(0, Math.min(100, triggerPercent(snap.input[name])));
  fill.style.width = width + "%";
}

function paintKey(root, snap, view, name) {
  var el = root.querySelector("[data-btn='" + name + "']");
  if (!el) return;
  var on = !!view.lit[name];
  el.className = "key" + (on ? " on" : "");
  el.querySelector("[data-value]").textContent = snap.input ? (buttonDown(snap.input[name]) ? "1.00" : "0.00") : "—";
  var label = el.querySelector("b").textContent;
  if (!snap.input || view.sticks === "blank") el.setAttribute("aria-label", label + ", no data");
  else if (!on && view.sticks === "held") el.setAttribute("aria-label", label + ", last received");
  else el.setAttribute("aria-label", label + (on ? ", pressed" : ", released"));
}

function paintGuide(root, snap, view) {
  var el = root.querySelector("[data-btn='guide']");
  el.className = "key" + (view.guideLit ? " on" : "");
  el.querySelector("[data-value]").textContent = snap.guide == null ? "—" : (guideDown(snap.guide) ? "1.00" : "0.00");
  if (snap.guide == null) el.setAttribute("aria-label", "Guide, no data");
  else if (view.guideLit) el.setAttribute("aria-label", "Guide, pressed");
  else if (view.mode === "held" || snap.link === "down") el.setAttribute("aria-label", "Guide, last received");
  else el.setAttribute("aria-label", "Guide, released");
}

function paintLog(root, logs) {
  var list = root.querySelector("[data-log]");
  var sig = logs.map(function (line) { return line.t + line.level + line.text; }).join("\n");
  if (list.dataset.sig === sig) return;
  list.dataset.sig = sig;
  var nearBottom = list.scrollHeight - list.scrollTop - list.clientHeight < 28;
  list.replaceChildren();
  logs.forEach(function (line) {
    var item = document.createElement("li");
    item.className = line.level || "info";
    item.textContent = line.t + "  " + line.text;
    list.appendChild(item);
  });
  if (nearBottom || list.scrollTop === 0) list.scrollTop = list.scrollHeight;
}

function drawStick(canvas, x, y, mode, phase) {
  if (!canvas) return;
  var dpr = window.devicePixelRatio || 1;
  var w = canvas.clientWidth || 280;
  var h = canvas.clientHeight || 280;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  var g = canvas.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  var cx = w / 2;
  var cy = h / 2;
  var r = Math.min(w, h) / 2 - 10;
  var ring = "#cbd5db";
  if (mode === "live") ring = "#16866c";
  else if (mode === "held") ring = "#8798a2";
  else if (phase === "waiting" || phase === "starting" || phase === "reconnecting") ring = "#c49a54";
  g.save();
  g.beginPath();
  g.arc(cx, cy, r, 0, Math.PI * 2);
  g.clip();
  g.strokeStyle = "#e6ecef";
  g.lineWidth = 1;
  [0.25, 0.5, 0.75].forEach(function (f) {
    g.beginPath();
    g.arc(cx, cy, r * f, 0, Math.PI * 2);
    g.stroke();
  });
  g.beginPath();
  g.moveTo(cx - r, cy);
  g.lineTo(cx + r, cy);
  g.moveTo(cx, cy - r);
  g.lineTo(cx, cy + r);
  g.stroke();
  g.restore();
  g.strokeStyle = ring;
  g.lineWidth = 2;
  g.beginPath();
  g.arc(cx, cy, r, 0, Math.PI * 2);
  g.stroke();
  g.font = "11px Menlo, monospace";
  g.fillStyle = "#7a8c97";
  g.textAlign = "center";
  g.fillText("+Y", cx, cy - r - 1);
  g.fillText("+X", cx + r - 10, cy - 4);
  if (mode === "blank" || x == null || y == null) return;
  var nx = x < 0 ? x / 32768 : x / 32767;
  var ny = y < 0 ? y / 32768 : y / 32767;
  g.fillStyle = mode === "live" ? "#16866c" : "#8798a2";
  g.beginPath();
  g.arc(cx + nx * r, cy - ny * r, 7, 0, Math.PI * 2);
  g.fill();
}

function postJSON(path) {
  return fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Wolverine-Request": "1" },
    body: "{}"
  }).then(function (res) {
    if (!res.ok) throw new Error(String(res.status));
    return res.json();
  });
}

function wire(root) {
  root.querySelector(".start").addEventListener("click", function () {
    postJSON("/api/start").then(function (snap) { render(snap, root); }).catch(function () {
      render(Object.assign(emptySnapshot(), { link: "down" }), root);
    });
  });
  root.querySelector(".stop").addEventListener("click", function () {
    postJSON("/api/stop").then(function (snap) { render(snap, root); }).catch(function () {
      render(Object.assign(emptySnapshot(), { link: "down" }), root);
    });
  });
}

function poll(root) {
  var last = null;
  function tick() {
    var started = performance.now();
    fetch("/api/state", { cache: "no-store" }).then(function (res) {
      if (!res.ok) throw new Error(String(res.status));
      return res.json();
    }).then(function (snap) {
      last = snap;
      render(snap, root);
    }).catch(function () {
      var failed = last ? Object.assign({}, last, { link: "down" }) : Object.assign(emptySnapshot(), { link: "down" });
      render(failed, root);
    }).then(function () {
      var wait = Math.max(0, 50 - (performance.now() - started));
      setTimeout(tick, wait);
    });
  }
  tick();
}

function sampleInput(over) {
  var input = {
    lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0,
    a: 0, b: 0, x: 0, y: 0, lb: 0, rb: 0, ls: 0, rs: 0,
    menu: 0, view: 0, du: 0, dd: 0, dl: 0, dr: 0, guide: 0
  };
  var key;
  for (key in over) input[key] = over[key];
  return input;
}

var FIXTURES = [
  {
    label: "No data",
    note: "Capture is stopped. Plots stay blank. Counts stay at zero. Nothing is drawn at the center.",
    snapshot: emptySnapshot()
  },
  {
    label: "Guide only",
    note: "A guide packet arrived and the handshake is complete. Guide can light. Sticks, triggers, and the full-input count stay empty.",
    snapshot: Object.assign(emptySnapshot(), {
      phase: "waiting",
      capturing: true,
      usb: "open",
      usb_detail: "1532:0a3f",
      audio_note: "Audio interface ignored",
      handshake: "complete",
      firmware: "1.1.1026.0",
      guide: 1,
      last_guide_age_ms: 16,
      logs: [{ t: "12:00:01", level: "warn", text: "could not claim audio interface #1 (ignored)" }]
    })
  },
  {
    label: "Live input",
    note: "LY and RT are real zeros from a full input report, so they show 0. LX is full negative scale. RX is full positive scale.",
    snapshot: Object.assign(emptySnapshot(), {
      phase: "live",
      capturing: true,
      usb: "open",
      usb_detail: "1532:0a3f",
      handshake: "complete",
      firmware: "1.1.1026.0",
      report_count: 12,
      last_input_age_ms: 15,
      last_guide_age_ms: 15,
      guide: 0,
      input: sampleInput({ lx: -32768, ly: 0, rx: 32767, ry: -1, lt: 1023, rt: 0, a: 1, du: 1, view: 1, rb: 1, ls: 1 }),
      extrema: {
        lx: { min: -32768, max: 100 },
        ly: { min: -200, max: 0 },
        rx: { min: 0, max: 32767 },
        ry: { min: -1, max: 40 },
        lt: { min: 0, max: 1023 },
        rt: { min: 0, max: 0 }
      }
    })
  },
  {
    label: "Last received",
    note: "The last full report is old. Numbers stay, labeled last received. Buttons and Guide do not stay lit. The link is not treated as unplugged.",
    snapshot: Object.assign(emptySnapshot(), {
      phase: "live",
      capturing: true,
      usb: "open",
      usb_detail: "1532:0a3f",
      handshake: "complete",
      firmware: "1.1.1026.0",
      report_count: 12,
      last_input_age_ms: 2500,
      last_guide_age_ms: 2500,
      guide: 1,
      input: sampleInput({ lx: -4000, ly: 8000, rx: 100, ry: -50, lt: 200, rt: 10, a: 1, guide: 1 }),
      extrema: {
        lx: { min: -4000, max: 10 },
        ly: { min: 0, max: 8000 },
        rx: { min: 0, max: 100 },
        ry: { min: -50, max: 0 },
        lt: { min: 0, max: 200 },
        rt: { min: 0, max: 10 }
      }
    })
  },
  {
    label: "Link lost",
    note: "The browser lost the dashboard, while the last snapshot was fresh. Highlights drop. The banner does not call that a USB disconnect.",
    snapshot: Object.assign(emptySnapshot(), {
      phase: "live",
      capturing: true,
      usb: "open",
      usb_detail: "1532:0a3f",
      handshake: "complete",
      firmware: "1.1.1026.0",
      report_count: 12,
      last_input_age_ms: 15,
      last_guide_age_ms: 15,
      guide: 1,
      link: "down",
      input: sampleInput({ lx: 1000, ly: -1000, a: 1, guide: 1 }),
      extrema: {
        lx: { min: 0, max: 1000 },
        ly: { min: -1000, max: 0 },
        rx: { min: 0, max: 0 },
        ry: { min: 0, max: 0 },
        lt: { min: 0, max: 0 },
        rt: { min: 0, max: 0 }
      }
    })
  },
  {
    label: "Replug",
    note: "The reader saw the device drop. Current input is cleared until a new full report. The old report count can remain.",
    snapshot: Object.assign(emptySnapshot(), {
      phase: "reconnecting",
      capturing: true,
      usb: "lost",
      usb_detail: "waiting for reconnect",
      handshake: "complete",
      firmware: "1.1.1026.0",
      report_count: 4,
      replug_count: 1
    })
  }
];

function renderFixtures(root) {
  FIXTURES.forEach(function (fx) {
    var section = document.createElement("section");
    section.className = "fixture";
    var title = document.createElement("h2");
    title.textContent = "Fixture: " + fx.label;
    var note = document.createElement("p");
    note.className = "fixture-note";
    note.textContent = fx.note;
    var point = document.createElement("div");
    section.append(title, note, point);
    root.append(section);
    mount(point, { controls: false });
    render(fx.snapshot, point);
  });
}

var exportRoot = typeof globalThis !== "undefined" ? globalThis : this;
exportRoot.presentation = presentation;
exportRoot.render = render;
exportRoot.mount = mount;
exportRoot.stickPercent = stickPercent;
exportRoot.triggerPercent = triggerPercent;
exportRoot.emptySnapshot = emptySnapshot;
exportRoot.FIXTURES = FIXTURES;
exportRoot.renderFixtures = renderFixtures;
exportRoot.wire = wire;
exportRoot.poll = poll;

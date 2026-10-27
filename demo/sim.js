/* The page around the simulator (knob.js / knob.wasm, built from sim/ with
 * the knob's own ui/ and core/). It plays the board's part: shows the frames,
 * passes touch and knob turns in, fetches and decodes covers, and stands in
 * for the microphone with a text box. */
"use strict";

const SIDE = 360;
const DEG_PER_DETENT = 15;    /* turning the ring by hand */
const WHEEL_PX_PER_DETENT = 40;

const screen = document.getElementById("screen");
const ctx = screen.getContext("2d");
const frame = ctx.createImageData(SIDE, SIDE);
const ring = document.getElementById("ring");
const knurl = document.getElementById("knurl");
const device = document.getElementById("device");
const statusLine = document.getElementById("status");
const voiceCard = document.getElementById("voice");
const voiceHint = document.getElementById("voice-hint");
const voiceForm = document.getElementById("voice-form");
const voiceInput = document.getElementById("voice-input");

let sim;  /* the C functions, once the module is ready */
let ringAngle = 0;  /* where the knob's detents have got to, in degrees */

var Module = {
  print: (t) => console.log(t),
  printErr: (t) => console.warn(t),
  onRuntimeInitialized: start,
  onHaptic: haptic,
  onListening: listening,
  loadCover: loadCover,
  loadPicture: loadPicture,
  refreshRecent: refreshRecent,
};

function start() {
  sim = {
    init: Module.cwrap("sim_init", null, []),
    frame: Module.cwrap("sim_frame", "number", []),
    pixels: Module.cwrap("sim_pixels", "number", []),
    touch: Module.cwrap("sim_touch", null, ["number", "number", "number", "number"]),
    knob: Module.cwrap("sim_knob", null, ["number"]),
    libBegin: Module.cwrap("sim_lib_begin", null, ["number"]),
    libAlbum: Module.cwrap("sim_lib_album", null, ["string", "string", "string", "string", "string", "number", "number"]),
    libTrack: Module.cwrap("sim_lib_track", null, ["string", "number", "string"]),
    libDone: Module.cwrap("sim_lib_done", null, ["number"]),
    libPlaylists: Module.cwrap("sim_lib_playlists", null, ["number"]),
    libPlaylist: Module.cwrap("sim_lib_playlist", null, ["string", "string", "number"]),
    libRecent: Module.cwrap("sim_lib_recent", null, ["number", "string", "string"]),
    libRecentDone: Module.cwrap("sim_lib_recent_done", null, []),
    libRecentReset: Module.cwrap("sim_lib_recent_reset", null, []),
    libPlaylistTrack: Module.cwrap("sim_lib_playlist_track", null, ["string", "number", "string", "string", "number"]),
    pictureReady: Module.cwrap("sim_picture_ready", null, ["number", "number", "number"]),
    coverReady: Module.cwrap("sim_cover_ready", null, ["number", "number"]),
    voiceText: Module.cwrap("sim_voice_text", null, ["string"]),
    voiceEnter: Module.cwrap("sim_voice_enter", null, []),
  };
  sim.init();
  requestAnimationFrame(loop);
  loadLibrary();
}

/* ---- Display ---- */
function loop() {
  if (sim.frame()) {
    const p = sim.pixels();
    frame.data.set(Module.HEAPU8.subarray(p, p + SIDE * SIDE * 4));
    ctx.putImageData(frame, 0, 0);
  }
  requestAnimationFrame(loop);
}

/* ---- Library: like the knob, the UI is up before the albums arrive ---- */
async function loadLibrary() {
  statusLine.textContent = "Loading the library…";
  let lib;
  try {
    lib = await (await fetch("data/library.json", { cache: "no-cache" })).json();  /* always the latest copy */
  } catch (e) {
    statusLine.textContent = "No library copied yet: run tools/sim.sh data, then rebuild.";
    return;
  }
  sim.libBegin(lib.albums.length);
  for (const a of lib.albums) {
    sim.libAlbum(a.name, a.sort_name || "", a.artist || "", a.artist_sort || "", a.id, a.cover ? 1 : 0, a.tracks.length);
    for (const t of a.tracks) sim.libTrack(t.name, t.seconds, t.artist || "");
  }
  const playlists = lib.playlists || [];
  sim.libPlaylists(playlists.length);
  for (const p of playlists) {
    sim.libPlaylist(p.name, p.id, p.tracks.length);
    for (const t of p.tracks) sim.libPlaylistTrack(t.name, t.seconds, t.artist || "", t.id, t.picture ? 1 : 0);
  }
  sim.libDone(lib.radio ? 1 : 0);
  /* recently played, after the albums are sorted: oldest first, each to the front */
  for (const r of (lib.recent || []).slice().reverse()) sim.libRecent(r.type === "playlist" ? 1 : 0, r.id, r.track || "");
  sim.libRecentDone();
  const tracks = lib.albums.reduce((n, a) => n + a.tracks.length, 0);
  if (lib.about) { publicDemo = true; statusLine.textContent = lib.about; return; }  /* the public demo's own line */
  statusLine.textContent = `${lib.albums.length} albums, ${tracks} tracks, ${playlists.length} playlists, copied from Music Assistant` +
    (lib.radio ? ", with radio stations." : ". No radio stations, so Radio isn't in the menu.");
}

/* Recent opened: the latest history, which the demo's server on the Mac fetches
 * from Music Assistant (the token stays there). Without that server (a plain
 * file server), the copied history stays. */
let refreshing = false, publicDemo = false;  /* the public demo has no server: its Recent stays as copied */
async function refreshRecent() {
  if (refreshing || publicDemo) return;
  refreshing = true;
  try {
    const r = await fetch("recent", { cache: "no-store" });
    if (r.ok) {
      const list = await r.json();
      sim.libRecentReset();
      for (const e of list.slice().reverse()) sim.libRecent(e.type === "playlist" ? 1 : 0, e.id, e.track || "");
      sim.libRecentDone();
    }
  } catch (e) {
    /* keep what's there */
  }
  refreshing = false;
}

/* One cover at a time, as the knob's loader: fetch, decode, scale to 360. */
const coverCanvas = document.createElement("canvas");
coverCanvas.width = coverCanvas.height = SIDE;
const coverCtx = coverCanvas.getContext("2d", { willReadFrequently: true });

async function loadCover(album, id) {
  let ptr = 0;
  try {
    const blob = await (await fetch(`data/covers/${encodeURIComponent(id)}.jpg`)).blob();
    const img = await createImageBitmap(blob);
    coverCtx.drawImage(img, 0, 0, SIDE, SIDE);
    img.close();
    const px = coverCtx.getImageData(0, 0, SIDE, SIDE).data;
    ptr = Module._malloc(px.length);
    Module.HEAPU8.set(px, ptr);
  } catch (e) {
    ptr = 0;
  }
  sim.coverReady(album, ptr);
  if (ptr) Module._free(ptr);
}

/* A playlist track's picture: YouTube thumbnails are wide, and some have black
 * bars above and below. Find the picture between any bars, then take its
 * centre square (as the knob will have to). */
function contentRows(img) {
  const w = 64, h = Math.max(1, Math.round((64 * img.height) / img.width));
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const x = c.getContext("2d", { willReadFrequently: true });
  x.drawImage(img, 0, 0, w, h);
  const px = x.getImageData(0, 0, w, h).data;
  const dark = (y) => { let s = 0; for (let i = 0; i < w; i++) { const o = (y * w + i) * 4; s += px[o] + px[o + 1] + px[o + 2]; } return s / (w * 3) < 18; };
  let top = 0, bottom = h - 1;
  while (top < h / 3 && dark(top)) top++;
  while (bottom > (2 * h) / 3 && dark(bottom)) bottom--;
  return [(top * img.height) / h, ((bottom + 1) * img.height) / h];
}

async function loadPicture(playlist, track, id) {
  let ptr = 0;
  try {
    const blob = await (await fetch(`data/tracks/${encodeURIComponent(id)}.jpg`)).blob();
    const img = await createImageBitmap(blob);
    const [y0, y1] = contentRows(img);
    const side = Math.min(img.width, y1 - y0);
    coverCtx.drawImage(img, (img.width - side) / 2, y0 + (y1 - y0 - side) / 2, side, side, 0, 0, SIDE, SIDE);
    img.close();
    const px = coverCtx.getImageData(0, 0, SIDE, SIDE).data;
    ptr = Module._malloc(px.length);
    Module.HEAPU8.set(px, ptr);
  } catch (e) {
    ptr = 0;
  }
  sim.pictureReady(playlist, track, ptr);
  if (ptr) Module._free(ptr);
}

/* ---- Touch: the screen is the touch panel ---- */
let touching = false;

/* keep getting the pointer's moves when it leaves the element (not possible
 * for scripted events, which have no real pointer behind them) */
function capture(el, e) {
  try { el.setPointerCapture(e.pointerId); } catch (err) { /* scripted */ }
}

function toScreen(e) {
  const r = screen.getBoundingClientRect();
  const x = Math.round(((e.clientX - r.left) / r.width) * SIDE);
  const y = Math.round(((e.clientY - r.top) / r.height) * SIDE);
  return [Math.max(0, Math.min(SIDE - 1, x)), Math.max(0, Math.min(SIDE - 1, y))];
}

screen.addEventListener("pointerdown", (e) => {
  if (!sim || !e.isPrimary || e.button > 0) return;
  e.preventDefault();
  capture(screen, e);
  touching = true;
  const [x, y] = toScreen(e);
  sim.touch(0, x, y, e.timeStamp);
});

screen.addEventListener("pointermove", (e) => {
  if (!touching || !e.isPrimary) return;
  /* every sample the browser saw, each with its own time: flick speed is
   * measured from them, as the firmware does from the touch panel's */
  const samples = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
  for (const s of samples.length ? samples : [e]) {
    const [x, y] = toScreen(s);
    sim.touch(1, x, y, s.timeStamp);
  }
});

function release(e) {
  if (!touching || !e.isPrimary) return;
  touching = false;
  const [x, y] = toScreen(e);
  sim.touch(2, x, y, e.timeStamp);
}
screen.addEventListener("pointerup", release);
screen.addEventListener("pointercancel", release);

/* ---- The knob ---- */
/* The ring is drawn where the hand has it (during a drag, between detents too)
 * and settles onto the detent when let go; only the GPU moves it. */
function showRing(angle, settle) {
  knurl.classList.toggle("settle", settle);
  knurl.style.transform = `rotate(${angle}deg)`;
}

function turn(steps, settle = true) {
  if (!sim || !steps) return;
  sim.knob(steps);
  ringAngle += steps * DEG_PER_DETENT;
  if (settle) showRing(ringAngle, true);
}

/* dragging the outer ring round */
let ringDrag = null;
function angleOf(e) {
  const r = device.getBoundingClientRect();
  return (Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2)) * 180) / Math.PI;
}
ring.addEventListener("pointerdown", (e) => {
  if (!e.isPrimary) return;
  e.preventDefault();
  capture(ring, e);
  ring.classList.add("turning");
  ringDrag = { last: angleOf(e), acc: 0 };
});
ring.addEventListener("pointermove", (e) => {
  if (!ringDrag) return;
  const a = angleOf(e);
  let d = a - ringDrag.last;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  ringDrag.last = a;
  ringDrag.acc += d;
  const steps = Math.trunc(ringDrag.acc / DEG_PER_DETENT);
  if (steps) {
    ringDrag.acc -= steps * DEG_PER_DETENT;
    turn(steps, false);  /* clockwise is positive, as on the knob */
  }
  showRing(ringAngle + ringDrag.acc, false);
});
const endRing = () => {
  if (!ringDrag) return;
  ringDrag = null;
  ring.classList.remove("turning");
  showRing(ringAngle, true);
};
ring.addEventListener("pointerup", endRing);
ring.addEventListener("pointercancel", endRing);

/* scrolling over the knob */
let wheelAcc = 0;
device.addEventListener("wheel", (e) => {
  e.preventDefault();
  const px = e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
  wheelAcc += px;
  const steps = Math.trunc(wheelAcc / WHEEL_PX_PER_DETENT);
  if (steps) {
    wheelAcc -= steps * WHEEL_PX_PER_DETENT;
    turn(steps);
  }
}, { passive: false });

document.addEventListener("keydown", (e) => {
  if (e.target === voiceInput) return;
  if (e.key === "ArrowRight" || e.key === "ArrowDown") { turn(1); e.preventDefault(); }
  else if (e.key === "ArrowLeft" || e.key === "ArrowUp") { turn(-1); e.preventDefault(); }
});

/* ---- Haptics: phones vibrate for each tick ---- */
function haptic() {
  /* phones only, and only once the page has been touched (browsers insist) */
  if (navigator.vibrate && navigator.maxTouchPoints > 0 && navigator.userActivation?.hasBeenActive) navigator.vibrate(8);
}

/* ---- Voice: the text box is the microphone ---- */
function listening(on) {
  voiceInput.disabled = !on;
  voiceCard.classList.toggle("listening", !!on);
  voiceHint.classList.toggle("live", !!on);
  if (on) {
    voiceInput.value = "";
    voiceHint.innerHTML = "<b>The knob is listening.</b> Type what you'd say, then Enter.";
    voiceInput.focus();
  } else {
    voiceHint.innerHTML = "Open <b>Search</b> on the knob. While it listens, type what you'd say here and press Enter.";
    voiceInput.blur();
  }
}
voiceInput.addEventListener("input", () => sim && sim.voiceText(voiceInput.value));
voiceForm.addEventListener("submit", (e) => {
  e.preventDefault();
  if (sim) sim.voiceEnter();
});

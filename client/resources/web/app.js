/* FreeShop web companion SPA. Vanilla JS, no build step.
   Talks to the REST/SSE API served by src/app/web_server.cpp. */
"use strict";

const $ = (id) => document.getElementById(id);

/* ---------- PIN ---------- */
// A QR scan lands with ?pin=…; remember it and strip it from the URL.
{
  const params = new URLSearchParams(location.search);
  const pin = params.get("pin");
  if (pin) {
    localStorage.setItem("freeshopPin", pin);
    history.replaceState(null, "", location.pathname + location.hash);
  }
}
const getPin = () => localStorage.getItem("freeshopPin") || "";

async function api(path, options = {}) {
  options.headers = Object.assign({}, options.headers);
  if (getPin()) options.headers["X-FreeShop-Pin"] = getPin();
  const resp = await fetch(path, options);
  if (resp.status === 401) {
    const pin = prompt("PIN (configúralo en la consola, en Ajustes → FTP):");
    if (pin) {
      localStorage.setItem("freeshopPin", pin);
      return api(path, options);
    }
  }
  return resp;
}

async function postJson(path, body) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

/* ---------- helpers ---------- */
function fmtBytes(n) {
  if (!n && n !== 0) return "?";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  let v = Number(n);
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return (v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)) + " " + units[i];
}
function fmtSpeed(bps) { return bps > 0 ? fmtBytes(bps) + "/s" : "—"; }
function fmtEta(seconds) {
  if (!seconds) return "";
  const s = Number(seconds);
  if (s < 60) return s + "s";
  const minutes = Math.floor(s / 60);
  if (minutes < 60) return minutes + "m " + (s % 60) + "s";
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return hours + "h " + (minutes % 60) + "m";
  return Math.floor(hours / 24) + "d " + (hours % 24) + "h";
}
function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// Server-side status/stage values are a fixed English enum (also used as CSS
// class hooks via .toLowerCase()) - only the on-screen label is translated.
const STATUS_LABEL = {
  Downloading: "Descargando", Installing: "Instalando", Committing: "Confirmando",
  Checking: "Verificando", Verifying: "Verificando", Resolving: "Resolviendo",
  Importing: "Importando", Completed: "Completado", Installed: "Instalado",
  Done: "Listo", Error: "Error", Paused: "Pausado", Queued: "En cola",
  Cancelled: "Cancelado", Fetching: "Obteniendo",
};
function statusLabel(s) { return STATUS_LABEL[s] || s; }

const STAGE_LABEL = {
  findingPeers: "Buscando pares", connecting: "Conectando",
  fetchingMetadata: "Obteniendo metadatos", validating: "Validando",
};

let toastTimer = null;
function toast(message, isError = false) {
  const el = $("toast");
  el.textContent = message;
  el.classList.toggle("err", isError);
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

function closeModal() { $("modal").close(); }
function openModal(html) {
  const modal = $("modal");
  modal.innerHTML = html;
  modal.showModal();
  modal.addEventListener("click", (e) => { if (e.target === modal) closeModal(); },
                         { once: true });
}

/* ---------- tabs ---------- */
const tabs = ["downloads", "catalog", "add", "files"];
function currentTab() {
  const h = location.hash.replace("#", "");
  return tabs.includes(h) ? h : "downloads";
}
function moveTabIndicator() {
  const active = document.querySelector(".tab.active");
  const indicator = $("tab-indicator");
  if (!active || !indicator) return;
  indicator.style.left = active.offsetLeft + "px";
  indicator.style.width = active.offsetWidth + "px";
}
function showTab() {
  const active = currentTab();
  for (const t of tabs) {
    $("view-" + t).hidden = t !== active;
    document.querySelector(`.tab[data-tab="${t}"]`)
      .classList.toggle("active", t === active);
  }
  moveTabIndicator();
  if (active === "catalog") loadCatalog();
  if (active === "files" && !filesPath) loadFiles("sdmc:/");
}
window.addEventListener("hashchange", showTab);
window.addEventListener("resize", moveTabIndicator);

/* ---------- live state (SSE) ---------- */
let state = { tasks: [], jobs: [], storage: null };
let lastStatuses = new Map();
let events = null;

function setConnLabel(on) {
  const el = document.querySelector("#conn .conn-label");
  if (el) el.textContent = on ? "conectado" : "conectando…";
}

function connectEvents() {
  if (events) events.close();
  events = new EventSource("/api/events");
  events.addEventListener("state", (e) => {
    $("conn").classList.add("on");
    setConnLabel(true);
    state = JSON.parse(e.data);
    notifyTransitions();
    renderDownloads();
  });
  events.onerror = () => { $("conn").classList.remove("on"); setConnLabel(false); };
}
connectEvents();

// Free one of the two SSE slots while the tab naps in the background.
let hiddenSince = null;
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    hiddenSince = Date.now();
    setTimeout(() => {
      if (document.hidden && hiddenSince && Date.now() - hiddenSince > 290000) {
        events?.close();
        events = null;
      }
    }, 300000);
  } else {
    hiddenSince = null;
    if (!events) connectEvents();
  }
});

/* ---------- alerts ----------
   The page is plain http:// on the LAN — an insecure origin, where the
   Notification API acts as permanently denied (Chrome M60+). So the primary
   channel is in-tab: sound + vibration + title flash + favicon badge, all of
   which still work on http. System notifications remain as a progressive
   enhancement for secure contexts (localhost / the PC test driver). */

// Notification API only counts where it can actually be granted.
function notifySupported() {
  return "Notification" in window && Notification.permission !== "denied";
}
function alertsOn() { return localStorage.getItem("freeshopAlerts") !== "off"; }

function refreshNotifyRow() {
  $("alerts-btn").textContent = alertsOn() ? "🔔 Alertas activadas" : "🔕 Alertas desactivadas";
  $("notify-btn").hidden =
    !notifySupported() || Notification.permission !== "default";
}
$("alerts-btn").addEventListener("click", () => {
  localStorage.setItem("freeshopAlerts", alertsOn() ? "off" : "on");
  if (alertsOn()) { unlockAudio(); beep(); }
  refreshNotifyRow();
});
$("notify-btn").addEventListener("click", async () => {
  await Notification.requestPermission();
  refreshNotifyRow();
});

// Autoplay policy: an AudioContext starts suspended until a user gesture.
// Any first tap on the page unlocks it, so a later "installed" beep works
// even if the user never touched the alerts button.
let audioCtx = null;
function unlockAudio() {
  if (!audioCtx) {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    audioCtx = new Ctx();
  }
  if (audioCtx.state === "suspended") audioCtx.resume();
}
document.addEventListener("pointerdown", unlockAudio, { once: true });

function beep(isError = false) {
  if (!audioCtx || audioCtx.state !== "running") return;
  // two short tones: up-chirp for done, low double for error
  const seq = isError ? [[220, 0, 0.12], [180, 0.16, 0.14]]
                      : [[660, 0, 0.09], [880, 0.11, 0.12]];
  for (const [freq, at, dur] of seq) {
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = "sine";
    osc.frequency.value = freq;
    const t0 = audioCtx.currentTime + at;
    gain.gain.setValueAtTime(0.0001, t0);
    gain.gain.exponentialRampToValueAtTime(0.12, t0 + 0.015);
    gain.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start(t0);
    osc.stop(t0 + dur + 0.02);
  }
}

/* Title flash + favicon badge until the user looks at the tab again. */
let flashTimer = null;
let badgedIcon = null;
function setFavicon(href) {
  let link = document.querySelector('link[rel="icon"]');
  if (link) link.href = href;
}
function buildBadgedIcon(done) {
  const img = new Image();
  img.src = "/icon-192.png";
  img.onload = () => {
    const c = document.createElement("canvas");
    c.width = c.height = 64;
    const g = c.getContext("2d");
    g.drawImage(img, 0, 0, 64, 64);
    g.beginPath();
    g.arc(48, 16, 14, 0, Math.PI * 2);
    g.fillStyle = "#ff5d5d";
    g.fill();
    badgedIcon = c.toDataURL("image/png");
    done(badgedIcon);
  };
}
function startFlash(message) {
  stopFlash();
  const base = "FreeShop";
  let tick = false;
  document.title = message;
  flashTimer = setInterval(() => {
    tick = !tick;
    document.title = tick ? base : message;
  }, 1200);
  if (badgedIcon) setFavicon(badgedIcon);
  else buildBadgedIcon(setFavicon);
}
function stopFlash() {
  if (flashTimer) clearInterval(flashTimer);
  flashTimer = null;
  document.title = "FreeShop";
  setFavicon("/icon-192.png");
}
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) stopFlash();
});
window.addEventListener("focus", stopFlash);

function fireAlert(title, body, isError) {
  if (notifySupported() && Notification.permission === "granted") {
    new Notification(title, { body, icon: "/icon-192.png" });
    return;
  }
  if (!alertsOn()) return;
  beep(isError);
  if (navigator.vibrate) navigator.vibrate(isError ? [180, 90, 180] : [120]);
  // Flash regardless of visibility: on a phone the tab is usually behind
  // the browser chrome or another app; focus/visibility clears it.
  startFlash((isError ? "✕ " : "✓ ") + body);
}

function notifyTransitions() {
  const interesting = { Installed: "instalado", Completed: "descargado", Error: "con error" };
  for (const t of state.tasks) {
    const prev = lastStatuses.get(t.id);
    if (prev && prev !== t.status && interesting[t.status]) {
      const what = t.status === "Error" ? (t.error || "falló")
                                        : interesting[t.status];
      fireAlert("FreeShop: " + t.name, t.name + " — " + what,
                t.status === "Error");
    }
    lastStatuses.set(t.id, t.status);
  }
}

/* ---------- downloads view ---------- */
const activeStatuses = ["Downloading", "Installing", "Committing", "Checking", "Verifying"];

function taskCard(t) {
  // Wanted (selection-aware) download range: the server sends it excluding
  // skipped files, which are pre-marked done by the engine.
  const wantedTotal = t.wantedTotalBytes || 0;
  const wantedDone = wantedTotal
    ? Math.min(t.wantedCompletedBytes || 0, wantedTotal) : 0;
  const downloadPct = wantedTotal
    ? Math.min(100, 100 * wantedDone / wantedTotal)
    : (t.totalBytes ? Math.min(100, 100 * t.completedBytes / t.totalBytes) : 0);
  const installPct = t.installTotalBytes
    ? Math.min(100, 100 * t.installedBytes / t.installTotalBytes) : null;
  const fetchPct = t.fetchProgress != null
    ? Math.min(100, 100 * t.fetchProgress) : null;
  // The bar tracks what the status is actually doing, like the Switch UI:
  // the fetch fraction, the current package's install fraction, or the
  // wanted download range.
  const pct = t.status === "Fetching" && fetchPct !== null ? fetchPct
    : (t.status === "Installing" || t.status === "Committing") && installPct !== null
      ? installPct : downloadPct;
  const active = activeStatuses.includes(t.status);
  const barClass = t.status === "Error" ? "err"
    : (t.status === "Installed" || t.status === "Completed") ? "ok" : "";

  const meta = [];
  if (t.status === "Downloading") meta.push(fmtSpeed(t.speedBps));
  if (t.status === "Installing") meta.push(fmtSpeed(t.installSpeedBps));
  if (t.status === "Fetching" && fetchPct !== null)
    meta.push(`obteniendo ${fetchPct.toFixed(0)}%`);
  // The byte line pairs with the bar: install bytes while installing,
  // wanted download bytes otherwise.
  if (t.status === "Installing" || t.status === "Committing") {
    if (t.installTotalBytes)
      meta.push(`${fmtBytes(t.installedBytes)} / ${fmtBytes(t.installTotalBytes)}`);
  } else if (wantedTotal) {
    meta.push(`${fmtBytes(wantedDone)} / ${fmtBytes(wantedTotal)}`);
  } else if (t.totalBytes) {
    meta.push(`${fmtBytes(t.completedBytes)} / ${fmtBytes(t.totalBytes)}`);
  }
  const eta = fmtEta(t.etaSeconds);
  if (eta) meta.push("faltan " + eta);
  if (active) meta.push(`${t.peers} pares`);
  if (t.mode === "install" && t.packageCount)
    meta.push(`paquete ${t.packagesInstalled}/${t.packageCount}`);

  const btn = (label, cmd, cls = "") =>
    `<button class="btn ${cls}" data-task="${t.id}" data-cmd="${cmd}">${label}</button>`;
  const actions = [];
  if (["Downloading", "Checking", "Queued"].includes(t.status)) actions.push(btn("Pausar", "pause"));
  if (t.status === "Paused") actions.push(btn("Reanudar", "resume", "btn-primary"));
  if (t.status === "Error") actions.push(btn("Reintentar", "retry", "btn-primary"));
  if (t.status === "Queued") actions.push(btn("Subir en la cola", "move-front"));
  if (["Paused", "Completed", "Error"].includes(t.status)) actions.push(btn("Verificar", "verify"));
  actions.push(btn("Eliminar", "remove", "btn-danger"));

  return `<div class="card task" data-id="${t.id}">
    <div class="task-head">
      <div class="task-name">${esc(t.name || t.id)}</div>
      <span class="badge ${t.status.toLowerCase()}">${esc(statusLabel(t.status))}</span>
    </div>
    <div class="bar"><div class="bar-fill ${barClass}" style="width:${pct}%"></div></div>
    <div class="task-meta">${meta.map(esc).join("<span>·</span>")}</div>
    ${t.error ? `<div class="task-error">${esc(t.error)}</div>` : ""}
    <div class="task-actions">${actions.join("")}</div>
  </div>`;
}

function jobCard(j) {
  const stageText = STAGE_LABEL[j.stage] || j.stage;
  const detail = j.state === "resolving"
    ? stageText + (j.peerCount ? ` (par ${j.peerIndex}/${j.peerCount})` : "")
    : j.state === "error" ? (j.error || "falló") : j.state;
  const cancellable = ["queued", "resolving"].includes(j.state);
  return `<div class="card task">
    <div class="task-head">
      <div class="task-name">${esc(j.title)}</div>
      <span class="badge ${j.state}">${j.state === "resolving" ? "Resolviendo" : esc(j.state)}</span>
    </div>
    <div class="task-meta">${esc(detail)}</div>
    ${cancellable
      ? `<div class="task-actions"><button class="btn" data-job="${j.jobId}">Cancelar</button></div>`
      : ""}
  </div>`;
}

function renderDownloads() {
  // Terminal "done" jobs whose task is already in the list are noise.
  const taskIds = new Set(state.tasks.map((t) => t.id));
  const jobs = state.jobs.filter((j) => !(j.state === "done" && taskIds.has(j.taskId)));
  $("jobs").innerHTML = jobs.map(jobCard).join("");
  $("tasks").innerHTML = state.tasks.map(taskCard).join("");
  $("tasks-empty").hidden = state.tasks.length > 0 || jobs.length > 0;

  if (state.storage && state.storage.available) {
    $("storage").hidden = false;
    const used = state.storage.totalBytes - state.storage.freeBytes;
    $("storage-fill").style.width =
      (100 * used / state.storage.totalBytes).toFixed(1) + "%";
    $("storage-text").textContent =
      `${fmtBytes(state.storage.freeBytes)} libres de ${fmtBytes(state.storage.totalBytes)}`;
  }
  refreshNotifyRow();
}

document.addEventListener("click", async (e) => {
  const cmdBtn = e.target.closest("[data-cmd]");
  if (cmdBtn) {
    const id = cmdBtn.dataset.task;
    const cmd = cmdBtn.dataset.cmd;
    if (cmd === "remove") return confirmRemove(id);
    const resp = await api(`/api/tasks/${id}/${cmd}`, { method: "POST" });
    if (!resp.ok && resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "no se pudo completar la acción", true);
    }
    return;
  }
  const jobBtn = e.target.closest("[data-job]");
  if (jobBtn) await api(`/api/jobs/${jobBtn.dataset.job}/cancel`, { method: "POST" });
});

function confirmRemove(id) {
  const t = state.tasks.find((x) => x.id === id);
  openModal(`<div class="modal-body">
      <h3>¿Eliminar descarga?</h3>
      <div class="modal-sub">${esc(t ? t.name : id)}</div>
      <label class="modal-check">
        <input type="checkbox" id="rm-data"> También eliminar los datos descargados
      </label>
    </div>
    <div class="modal-actions">
      <button class="btn" id="rm-cancel">Cancelar</button>
      <button class="btn btn-danger" id="rm-ok">Eliminar</button>
    </div>`);
  $("rm-cancel").onclick = closeModal;
  $("rm-ok").onclick = async () => {
    const resp = await postJson(`/api/tasks/${id}/remove`,
                                { deleteData: $("rm-data").checked });
    closeModal();
    if (!resp.ok) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "no se pudo eliminar", true);
    }
  };
}

/* ---------- catalog ---------- */
let catalog = null;
let catalogFiltered = [];
let catalogShown = 0;
const CATALOG_CHUNK = 60;

async function loadCatalog() {
  if (catalog) return;
  const status = $("catalog-status");
  status.hidden = false;
  status.textContent = "Cargando catálogo…";
  try {
    const resp = await api("/api/catalog");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    catalog = await resp.json();
    status.hidden = true;
    applyCatalogFilter();
  } catch (err) {
    catalog = null;
    status.textContent = "No se pudo cargar el catálogo: " + err.message;
  }
}

function applyCatalogFilter() {
  const q = $("search").value.trim().toLowerCase();
  catalogFiltered = !q
    ? catalog
    : catalog.filter((e) => e.title.toLowerCase().includes(q));
  catalogShown = 0;
  $("catalog-grid").innerHTML = "";
  appendCatalogChunk();
}

function appendCatalogChunk() {
  if (!catalogFiltered) return;
  const slice = catalogFiltered.slice(catalogShown, catalogShown + CATALOG_CHUNK);
  catalogShown += slice.length;
  const html = slice.map((e, i) => {
    const idx = catalogShown - slice.length + i;
    const sub = [e.year, e.size ? fmtBytes(e.size) : ""].filter(Boolean).join(" · ");
    return `<div class="game" data-idx="${idx}">
      ${e.posterUrl
        ? `<img class="game-cover" loading="lazy" src="${esc(e.posterUrl)}" alt="" onerror="this.style.visibility='hidden'">`
        : `<div class="game-cover"></div>`}
      <div class="game-body">
        <div class="game-title">${esc(e.title)}</div>
        <div class="game-sub">${esc(sub)}</div>
      </div>
    </div>`;
  }).join("");
  $("catalog-grid").insertAdjacentHTML("beforeend", html);
}

new IntersectionObserver((entries) => {
  if (entries[0].isIntersecting) appendCatalogChunk();
}).observe($("catalog-sentinel"));

let searchTimer = null;
$("search").addEventListener("input", () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => { if (catalog) applyCatalogFilter(); }, 150);
});

$("catalog-grid").addEventListener("click", (e) => {
  const card = e.target.closest(".game");
  if (!card) return;
  const entry = catalogFiltered[Number(card.dataset.idx)];
  if (entry) openGameModal(entry);
});

function openGameModal(entry) {
  const sub = [entry.year, entry.genre, entry.publisher,
               entry.size ? fmtBytes(entry.size) : ""].filter(Boolean).join(" · ");
  openModal(`<div class="modal-body">
      ${entry.posterUrl ? `<img class="modal-cover" src="${esc(entry.posterUrl)}" alt="">` : ""}
      <h3>${esc(entry.title)}</h3>
      <div class="modal-sub">${esc(sub)}</div>
      ${entry.description ? `<div class="modal-desc">${esc(entry.description)}</div>` : ""}
    </div>
    <div class="modal-actions">
      <button class="btn" id="game-cancel">Cerrar</button>
      <button class="btn" id="game-download">Descargar</button>
      <button class="btn btn-primary" id="game-install">Instalar</button>
    </div>`);
  $("game-cancel").onclick = closeModal;
  const start = (mode) => async () => {
    closeModal();
    const resp = await postJson("/api/add/catalog",
                                { infoHash: entry.infoHash, mode });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      toast("Agregado a la cola de la Switch");
      location.hash = "#downloads";
    } else {
      toast(body.error || "no se pudo agregar", true);
    }
  };
  $("game-install").onclick = start("install");
  $("game-download").onclick = start("download");
}

/* ---------- add tab ---------- */
function checkedMode(groupId) {
  return document.querySelector(`#${groupId} input:checked`).value;
}

$("magnet-btn").addEventListener("click", async () => {
  const magnet = $("magnet-input").value.trim();
  if (!magnet) return toast("Pega un enlace magnet primero", true);
  $("magnet-btn").disabled = true;
  try {
    const resp = await postJson("/api/add/magnet",
                                { magnet, mode: checkedMode("magnet-mode") });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("magnet-input").value = "";
      toast("Resolviendo en la Switch…");
      location.hash = "#downloads";
    } else {
      toast(body.error || "no se pudo agregar", true);
    }
  } finally {
    $("magnet-btn").disabled = false;
  }
});

$("torrent-file").addEventListener("change", () => {
  const file = $("torrent-file").files[0];
  $("torrent-file-label").textContent = file ? file.name : "Toca para elegir un archivo…";
});

$("torrent-btn").addEventListener("click", async () => {
  const file = $("torrent-file").files[0];
  if (!file) return toast("Elige un archivo .torrent primero", true);
  $("torrent-btn").disabled = true;
  try {
    const resp = await api(
      "/api/add/torrent?mode=" + checkedMode("torrent-mode"),
      { method: "POST",
        headers: { "Content-Type": "application/x-bittorrent" },
        body: file });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("torrent-file").value = "";
      $("torrent-file-label").textContent = "Toca para elegir un archivo…";
      toast("Agregado a la cola de la Switch");
      location.hash = "#downloads";
    } else {
      toast(body.error || "no se pudo subir", true);
    }
  } finally {
    $("torrent-btn").disabled = false;
  }
});

/* ---------- files ---------- */
let filesPath = "";
const ICON_FOLDER = '<svg viewBox="0 0 24 24" class="files-icon"><path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4.5l2 2h7.5a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5H5a1.5 1.5 0 0 1-1.5-1.5v-10Z" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>';
const ICON_FILE = '<svg viewBox="0 0 24 24" class="files-icon"><path d="M7 3.5h7l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1v-16a1 1 0 0 1 1-1Z" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/><path d="M14 3.5V8h4.5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>';

async function loadFiles(path) {
  filesPath = path;
  $("files-breadcrumb").textContent = path;
  $("files-list").innerHTML = "";
  $("files-empty").hidden = true;
  try {
    const resp = await api("/api/explorer/list?path=" + encodeURIComponent(path));
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) {
      toast(body.error || "no se pudo abrir esa carpeta", true);
      return;
    }
    renderFiles(body);
  } catch {
    toast("error de red", true);
  }
}

function renderFiles(body) {
  const list = $("files-list");
  list.innerHTML = "";
  const entries = body.entries || [];
  if (body.parent !== undefined) {
    const up = document.createElement("div");
    up.className = "files-row";
    up.dataset.dir = "1";
    up.innerHTML = `${ICON_FOLDER}<span class="files-name">..</span>`;
    up.addEventListener("click", () => loadFiles(body.parent));
    list.appendChild(up);
  }
  $("files-empty").hidden = entries.length !== 0;
  for (const entry of entries) {
    const row = document.createElement("div");
    row.className = "files-row" + (entry.directory ? "" : " is-file");
    if (entry.directory) row.dataset.dir = "1";
    row.innerHTML = (entry.directory ? ICON_FOLDER : ICON_FILE) +
      `<span class="files-name">${esc(entry.name)}</span>` +
      (entry.directory ? "" : `<span class="files-size">${fmtBytes(entry.size)}</span>`);
    if (entry.directory) {
      row.addEventListener("click", () => loadFiles(entry.path));
    } else {
      row.querySelector(".files-name").addEventListener("click", () => downloadFile(entry));
    }
    list.appendChild(row);
  }
}

async function downloadFile(entry) {
  toast("Descargando " + entry.name + "…");
  try {
    const resp = await api("/api/explorer/download?path=" + encodeURIComponent(entry.path));
    if (!resp.ok) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "no se pudo descargar", true);
      return;
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = entry.name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
  } catch {
    toast("error de red", true);
  }
}

$("files-upload-btn").addEventListener("click", () => $("files-upload-input").click());
$("files-upload-input").addEventListener("change", async () => {
  const file = $("files-upload-input").files[0];
  $("files-upload-input").value = "";
  if (!file || !filesPath) return;
  const progress = $("files-upload-progress");
  progress.hidden = false;
  progress.textContent = "Subiendo " + file.name + "…";
  $("files-upload-btn").disabled = true;
  try {
    // The browser streams `file` as the request body natively - it is never
    // held whole in JS memory, matching what the server does on its side.
    const resp = await api(
      "/api/explorer/upload?path=" + encodeURIComponent(filesPath) +
        "&name=" + encodeURIComponent(file.name),
      { method: "POST", body: file });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      toast("Se subió " + file.name);
      loadFiles(filesPath);
    } else {
      toast(body.error || "no se pudo subir", true);
    }
  } catch {
    toast("error de red", true);
  } finally {
    progress.hidden = true;
    $("files-upload-btn").disabled = false;
  }
});

/* ---------- boot ---------- */
showTab();
refreshNotifyRow();
api("/api/tasks").then((r) => r.json()).then((s) => {
  state = s;
  renderDownloads();
}).catch(() => {});

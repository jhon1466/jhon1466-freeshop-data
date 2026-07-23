// Mirrors shared/catalog.schema.json. Keep these in sync by hand: this page
// has no build step, so it can't import ajv + the JSON schema directly. The
// server re-validates with the real ajv schema on save, so this is just
// instant feedback, not the source of truth.
const ID_PATTERN = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const SHA256_PATTERN = /^[a-f0-9]{64}$/;
const DESCRIPTION_MAX_LENGTH = 200;
const REQUIRED_FIELDS = [
  "id",
  "title",
  "author",
  "category",
  "description",
  "version",
  "iconUrl",
  "downloadUrl",
  "fileSize",
  "filename",
];
// sha256 is optional (see shared/catalog.schema.json) - if present it must
// still match SHA256_PATTERN (checked below), but it's no longer required.

function validateEntry(entry, existingApps, editingId) {
  const errors = [];

  for (const field of REQUIRED_FIELDS) {
    const value = entry[field];
    // description has no minLength in the schema - an empty string is valid,
    // just must be present. Every other required string field does have a
    // minLength (or an equivalently strict pattern), so "" is genuinely
    // missing for those.
    const missing =
      field === "description"
        ? value === undefined || value === null
        : value === undefined || value === null || value === "";
    if (missing) {
      errors.push(`${field} es obligatorio`);
    }
  }

  if (entry.id && !ID_PATTERN.test(entry.id)) {
    errors.push("id debe ser un slug (minúsculas, números, guiones)");
  }
  if (entry.id && existingApps.some((a) => a.id === entry.id && a.id !== editingId)) {
    errors.push(`ya existe una app con id "${entry.id}"`);
  }
  if (entry.parentId) {
    if (entry.parentId === entry.id) {
      errors.push("parentId no puede ser el mismo id de la app");
    } else {
      const parent = existingApps.find((a) => a.id === entry.parentId);
      if (!parent) {
        errors.push(`parentId "${entry.parentId}" no existe en el catálogo`);
      } else if (parent.parentId) {
        errors.push(`"${entry.parentId}" ya es DLC/update de otro juego - solo un nivel de anidado`);
      }
    }
  }
  if (entry.description && entry.description.length > DESCRIPTION_MAX_LENGTH) {
    errors.push(`description debe tener ${DESCRIPTION_MAX_LENGTH} caracteres o menos`);
  }
  if (entry.sha256 && !SHA256_PATTERN.test(entry.sha256)) {
    errors.push("sha256 debe ser 64 caracteres hexadecimales en minúscula");
  }
  if (entry.fileSize !== undefined && entry.fileSize !== "" && (!Number.isInteger(entry.fileSize) || entry.fileSize < 0)) {
    errors.push("fileSize debe ser un entero >= 0");
  }
  const fileType = entry.fileType || "nro";
  if (entry.filename) {
    const expectedExt = fileType === "nsp" ? ".nsp" : fileType === "xci" ? ".xci" : ".nro";
    if (!entry.filename.toLowerCase().endsWith(expectedExt)) {
      errors.push(`filename debe terminar en ${expectedExt} para fileType "${fileType}"`);
    }
    if (entry.filename.includes("/") || entry.filename.includes("\\")) {
      errors.push("filename no debe contener rutas");
    }
  }

  return errors;
}

function validateCatalogDoc(doc) {
  const errors = [];
  if (doc.schemaVersion !== 1) errors.push("schemaVersion debe ser 1");
  if (!Array.isArray(doc.apps)) errors.push("apps debe ser un arreglo");
  else {
    doc.apps.forEach((app, i) => {
      const entryErrors = validateEntry(app, doc.apps, app.id);
      entryErrors.forEach((e) => errors.push(`apps[${i}]: ${e}`));
    });
  }
  return errors;
}

const SESSION_KEY = "freeshop-admin-password";

const state = {
  catalog: null, // { schemaVersion, generatedAt, apps: [] }
  password: sessionStorage.getItem(SESSION_KEY) || "",
};

const $ = (sel) => document.querySelector(sel);

const els = {
  session: $("#session"),
  logoutBtn: $("#logout-btn"),
  loginView: $("#login-view"),
  loginForm: $("#login-form"),
  loginError: $("#login-error"),
  appView: $("#app-view"),
  appsTbody: $("#apps-tbody"),
  emptyState: $("#empty-state"),
  addAppBtn: $("#add-app-btn"),
  importBtn: $("#import-btn"),
  exportBtn: $("#export-btn"),
  saveStatus: $("#save-status"),
  editDialog: $("#edit-dialog"),
  editForm: $("#edit-form"),
  editDialogTitle: $("#edit-dialog-title"),
  editError: $("#edit-error"),
  editCancelBtn: $("#edit-cancel-btn"),
  editSaveBtn: $("#edit-save-btn"),
  importDialog: $("#import-dialog"),
  importTextarea: $("#import-textarea"),
  importError: $("#import-error"),
  importCancelBtn: $("#import-cancel-btn"),
  importConfirmBtn: $("#import-confirm-btn"),
  iconFileInput: $("#icon-file-input"),
  iconUploadBtn: $("#icon-upload-btn"),
  iconUploadStatus: $("#icon-upload-status"),
  checksumBtn: $("#checksum-btn"),
  checksumStatus: $("#checksum-status"),
  parentIdSelect: $("#parent-id-select"),
  categorySuggestions: $("#category-suggestions"),
};

// Shown even before any app uses them, so a fresh catalog still offers
// sensible starting points - purely suggestions (the field stays free
// text), not an enum.
const SUGGESTED_CATEGORIES = ["Ports Nativos", "Traducciones", "Emuladores", "Mods", "Homebrew", "Juegos"];

function authHeaders() {
  return { Authorization: `Bearer ${state.password}` };
}

async function verifyPassword(password) {
  const res = await fetch("/api/admin/verify", {
    headers: { Authorization: `Bearer ${password}` },
  });
  return res.ok;
}

function showLoggedIn() {
  els.session.hidden = false;
  els.loginView.hidden = true;
  els.appView.hidden = false;
}

function showLoggedOut() {
  els.session.hidden = true;
  els.loginView.hidden = false;
  els.appView.hidden = true;
  state.catalog = null;
}

async function loadCatalog() {
  const res = await fetch("/api/apps");
  if (res.ok) {
    state.catalog = await res.json();
  } else {
    // No catalog committed yet - start from an empty document.
    state.catalog = { schemaVersion: 1, generatedAt: new Date().toISOString(), apps: [] };
  }
  renderTable();
}

async function saveCatalog(nextApps) {
  const next = {
    schemaVersion: 1,
    generatedAt: new Date().toISOString(),
    apps: nextApps,
  };
  const errors = validateCatalogDoc(next);
  if (errors.length > 0) {
    throw new Error(errors.join("; "));
  }

  const res = await fetch("/api/admin/catalog", {
    method: "POST",
    headers: { ...authHeaders(), "Content-Type": "application/json" },
    body: JSON.stringify(next),
  });
  if (res.status === 401) {
    showLoggedOut();
    throw new Error("sesión expirada, vuelve a iniciar sesión");
  }
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.message || `HTTP ${res.status}`);
  }

  state.catalog = next;
  renderTable();
  els.saveStatus.textContent = "Guardado " + new Date().toLocaleTimeString();
  setTimeout(() => (els.saveStatus.textContent = ""), 4000);
}

function renderTable() {
  const apps = state.catalog.apps;
  els.appsTbody.innerHTML = "";
  els.emptyState.hidden = apps.length > 0;
  for (const a of apps) {
    const tr = document.createElement("tr");
    let idCell = escapeHtml(a.id);
    if (a.parentId) {
      const parent = apps.find((p) => p.id === a.parentId);
      const label = a.contentType === "update" ? "Update" : a.contentType === "dlc" ? "DLC" : "DLC/Update";
      idCell += `<br><small>&#8618; ${label} de ${escapeHtml(parent ? parent.title : a.parentId)}</small>`;
    }
    tr.innerHTML = `
      <td>${idCell}</td>
      <td>${escapeHtml(a.title)}</td>
      <td>${escapeHtml(a.version)}</td>
      <td>${escapeHtml(a.category)}</td>
      <td class="row-actions">
        <button type="button" data-action="edit" data-id="${escapeHtml(a.id)}">Editar</button>
        <button type="button" data-action="delete" data-id="${escapeHtml(a.id)}">Borrar</button>
      </td>
    `;
    els.appsTbody.appendChild(tr);
  }
}

function fileToBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result.split(",")[1]);
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(file);
  });
}

function populateCategorySuggestions() {
  const apps = state.catalog ? state.catalog.apps : [];
  const existing = [...new Set(apps.map((a) => a.category).filter(Boolean))];
  const all = [...new Set([...existing, ...SUGGESTED_CATEGORIES])];
  els.categorySuggestions.innerHTML = all.map((c) => `<option value="${escapeHtml(c)}"></option>`).join("");
}

function populateParentIdSelect(editingId) {
  const select = els.parentIdSelect;
  select.innerHTML = '<option value="">(ninguno - juego base)</option>';
  const apps = state.catalog ? state.catalog.apps : [];
  // Only other base games can be a parent - a DLC/update can't itself have
  // DLC/updates (see validateEntry's "solo un nivel de anidado" check).
  for (const a of apps) {
    if (a.id === editingId || a.parentId) continue;
    const opt = document.createElement("option");
    opt.value = a.id;
    opt.textContent = `${a.title} (${a.id})`;
    select.appendChild(opt);
  }
}

function openEditDialog(entry) {
  els.editError.hidden = true;
  els.editForm.reset();
  els.iconFileInput.value = "";
  els.iconUploadStatus.textContent = "";
  els.checksumStatus.textContent = "";
  els.editForm.dataset.editingId = entry ? entry.id : "";
  els.editDialogTitle.textContent = entry ? `Editar "${entry.id}"` : "Añadir app";
  populateParentIdSelect(entry ? entry.id : null);
  populateCategorySuggestions();
  if (entry) {
    for (const [key, value] of Object.entries(entry)) {
      const field = els.editForm.elements.namedItem(key);
      if (field) field.value = value ?? "";
    }
    els.editForm.elements.namedItem("id").disabled = true;
  } else {
    els.editForm.elements.namedItem("id").disabled = false;
  }
  els.editDialog.showModal();
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  }[c]));
}

async function main() {
  if (state.password && (await verifyPassword(state.password))) {
    showLoggedIn();
    await loadCatalog();
  } else {
    state.password = "";
    sessionStorage.removeItem(SESSION_KEY);
    showLoggedOut();
  }

  els.loginForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    els.loginError.hidden = true;
    const password = $("#login-password").value;
    const ok = await verifyPassword(password);
    if (!ok) {
      els.loginError.textContent = "Contraseña incorrecta";
      els.loginError.hidden = false;
      return;
    }
    state.password = password;
    sessionStorage.setItem(SESSION_KEY, password);
    els.loginForm.reset();
    showLoggedIn();
    await loadCatalog();
  });

  els.logoutBtn.addEventListener("click", () => {
    state.password = "";
    sessionStorage.removeItem(SESSION_KEY);
    showLoggedOut();
  });

  els.appsTbody.addEventListener("click", async (e) => {
    const btn = e.target.closest("button[data-action]");
    if (!btn) return;
    const id = btn.dataset.id;
    if (btn.dataset.action === "edit") {
      openEditDialog(state.catalog.apps.find((a) => a.id === id));
    } else if (btn.dataset.action === "delete") {
      const children = state.catalog.apps.filter((a) => a.parentId === id);
      const childNote =
        children.length > 0
          ? `\n\nTambién se borrarán ${children.length} DLC/actualización(es) vinculada(s): ${children
              .map((c) => c.title)
              .join(", ")}.`
          : "";
      if (!confirm(`¿Borrar "${id}" del catálogo?${childNote}`)) return;
      try {
        const childIds = new Set(children.map((c) => c.id));
        await saveCatalog(state.catalog.apps.filter((a) => a.id !== id && !childIds.has(a.id)));
      } catch (err) {
        alert("No se pudo borrar: " + err.message);
      }
    }
  });

  els.addAppBtn.addEventListener("click", () => openEditDialog(null));
  els.editCancelBtn.addEventListener("click", () => els.editDialog.close());

  function setStatus(el, text, kind) {
    el.textContent = text;
    el.classList.remove("status-error", "status-ok");
    if (kind) el.classList.add(kind === "error" ? "status-error" : "status-ok");
  }

  els.iconUploadBtn.addEventListener("click", async () => {
    setStatus(els.iconUploadStatus, "", null);
    const editingId = els.editForm.dataset.editingId;
    const id = editingId || els.editForm.elements.namedItem("id").value.trim();
    if (!id || !ID_PATTERN.test(id)) {
      setStatus(els.iconUploadStatus, "completa un id válido antes de subir el icono", "error");
      return;
    }
    const file = els.iconFileInput.files[0];
    if (!file) {
      setStatus(els.iconUploadStatus, "selecciona un archivo primero", "error");
      return;
    }
    setStatus(els.iconUploadStatus, "subiendo...", null);
    try {
      const contentBase64 = await fileToBase64(file);
      const res = await fetch("/api/admin/icon", {
        method: "POST",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({ id, filename: file.name, contentBase64 }),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(body.message || `HTTP ${res.status}`);
      }
      els.editForm.elements.namedItem("iconUrl").value = body.iconUrl;
      setStatus(els.iconUploadStatus, "subido ✓", "ok");
    } catch (err) {
      setStatus(els.iconUploadStatus, "error: " + err.message, "error");
    }
  });

  els.checksumBtn.addEventListener("click", async () => {
    const url = els.editForm.elements.namedItem("downloadUrl").value.trim();
    if (!url) {
      setStatus(els.checksumStatus, "completa downloadUrl primero", "error");
      return;
    }
    if (!/^https?:\/\//i.test(url)) {
      setStatus(
        els.checksumStatus,
        "downloadUrl debe ser un link completo empezando con http:// o https:// (parece que falta el principio)",
        "error"
      );
      return;
    }
    setStatus(els.checksumStatus, "calculando...", null);
    try {
      const res = await fetch("/api/admin/filesize", {
        method: "POST",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({ url }),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(body.message || `HTTP ${res.status}`);
      }
      els.editForm.elements.namedItem("fileSize").value = body.fileSize;
      setStatus(els.checksumStatus, `listo ✓ (${body.fileSize} bytes)`, "ok");
    } catch (err) {
      setStatus(els.checksumStatus, "error: " + err.message, "error");
    }
  });

  els.editSaveBtn.addEventListener("click", async () => {
    const formData = new FormData(els.editForm);
    const editingId = els.editForm.dataset.editingId;
    const entry = {
      id: editingId || formData.get("id").trim(),
      title: formData.get("title").trim(),
      author: formData.get("author").trim(),
      category: formData.get("category").trim(),
      description: formData.get("description").trim(),
      version: formData.get("version").trim(),
      iconUrl: formData.get("iconUrl").trim(),
      downloadUrl: formData.get("downloadUrl").trim(),
      fileSize: parseInt(formData.get("fileSize"), 10),
      filename: formData.get("filename").trim(),
      fileType: formData.get("fileType") || "nro",
    };
    const sha256 = formData.get("sha256").trim().toLowerCase();
    if (sha256) entry.sha256 = sha256;
    const longDescription = formData.get("longDescription").trim();
    if (longDescription) entry.longDescription = longDescription;
    const homepageUrl = formData.get("homepageUrl").trim();
    if (homepageUrl) entry.homepageUrl = homepageUrl;
    const license = formData.get("license").trim();
    if (license) entry.license = license;
    const parentId = formData.get("parentId");
    if (parentId) entry.parentId = parentId;
    const contentType = formData.get("contentType");
    if (contentType) entry.contentType = contentType;

    const otherApps = state.catalog.apps.filter((a) => a.id !== editingId);
    const errors = validateEntry(entry, otherApps, editingId || null);
    if (errors.length > 0) {
      els.editError.textContent = errors.join("; ");
      els.editError.hidden = false;
      return;
    }

    const nextApps = editingId
      ? state.catalog.apps.map((a) => (a.id === editingId ? entry : a))
      : [...state.catalog.apps, entry];

    try {
      await saveCatalog(nextApps);
      els.editDialog.close();
    } catch (err) {
      els.editError.textContent = "No se pudo guardar: " + err.message;
      els.editError.hidden = false;
    }
  });

  els.importBtn.addEventListener("click", () => {
    els.importError.hidden = true;
    els.importTextarea.value = "";
    els.importDialog.showModal();
  });
  els.importCancelBtn.addEventListener("click", () => els.importDialog.close());
  els.importConfirmBtn.addEventListener("click", async () => {
    els.importError.hidden = true;
    let parsed;
    try {
      parsed = JSON.parse(els.importTextarea.value);
    } catch (err) {
      els.importError.textContent = "JSON inválido: " + err.message;
      els.importError.hidden = false;
      return;
    }
    if (!Array.isArray(parsed.apps)) {
      els.importError.textContent = 'El JSON debe tener un arreglo "apps"';
      els.importError.hidden = false;
      return;
    }
    try {
      await saveCatalog(parsed.apps);
      els.importDialog.close();
    } catch (err) {
      els.importError.textContent = "No se pudo importar: " + err.message;
      els.importError.hidden = false;
    }
  });

  els.exportBtn.addEventListener("click", () => {
    const blob = new Blob([JSON.stringify(state.catalog, null, 2)], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "catalog.json";
    a.click();
    URL.revokeObjectURL(url);
  });
}

main();

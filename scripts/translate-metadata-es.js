// Translates every game_metadata_index.json entry's `description` to Latin
// American Spanish using a local Ollama model - no API key, no per-character
// cost, runs entirely on this machine's GPU. Only `description` is
// translated: every entry in the current index already has one, so `intro`
// (the fallback catalog_presentation.cpp uses when description is missing)
// never actually gets read - translating it too would double the runtime
// for no rendering benefit.
//
// Writes translations to a small side file (infoHash -> descriptionEs)
// rather than rewriting the whole 8MB index each checkpoint, and skips any
// infoHash already present there - safe to stop (Ctrl+C) and re-run later,
// it picks up where it left off.
//
// Usage:
//   node scripts/translate-metadata-es.js
//
// Requires Ollama running locally (default http://localhost:11434) with the
// model below already pulled.
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const SOURCE = path.join(root, "client", "resources", "catalog", "game_metadata_index.json");
const OUTPUT = path.join(root, "client", "resources", "catalog", "game_metadata_index.es.json");
const OLLAMA_URL = "http://localhost:11434/api/generate";
const MODEL = "huihui_ai/qwen3.5-abliterated:9b";
const CHECKPOINT_EVERY = 20;
const MAX_RETRIES = 3;
// Ollama serves one loaded model to multiple concurrent requests (queued
// internally against the GPU), so a handful of workers in flight keeps the
// GPU fed between each request's own overhead instead of idling on I/O -
// worth trying higher if `nvidia-smi` still shows headroom while this runs.
const CONCURRENCY = 4;

function buildPrompt(text) {
  return "Traduce al espanol latino la siguiente descripcion de un videojuego de Nintendo Switch. " +
    "Reglas: no traduzcas nombres propios, titulos de juegos, ni nombres de personajes o marcas " +
    "(dejalos exactamente como estan en ingles); traduce el resto de forma natural y fluida; " +
    "responde UNICAMENTE con la traduccion final, sin comentarios, notas ni texto adicional.\n\n" +
    "Texto:\n" + text;
}

async function translateOnce(text) {
  const res = await fetch(OLLAMA_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: MODEL,
      prompt: buildPrompt(text),
      stream: false,
      think: false,
    }),
  });
  if (!res.ok) throw new Error(`Ollama HTTP ${res.status}`);
  const json = await res.json();
  const out = (json.response || "").trim();
  if (!out) throw new Error("respuesta vacia");
  return out;
}

async function translate(text) {
  let lastError;
  for (let attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    try {
      return await translateOnce(text);
    } catch (err) {
      lastError = err;
      if (attempt < MAX_RETRIES) await new Promise(r => setTimeout(r, 2000 * attempt));
    }
  }
  throw lastError;
}

function loadTranslations() {
  if (!fs.existsSync(OUTPUT)) return {};
  try {
    return JSON.parse(fs.readFileSync(OUTPUT, "utf8"));
  } catch {
    return {};
  }
}

function saveTranslations(translations) {
  fs.writeFileSync(OUTPUT, JSON.stringify(translations));
}

async function main() {
  const data = JSON.parse(fs.readFileSync(SOURCE, "utf8"));
  const withDescription = data.filter(e => e.description);
  const translations = loadTranslations();
  const pending = withDescription.filter(e => !translations[e.infoHash]);

  console.log(`Total con description: ${withDescription.length}, ya traducidas: ${withDescription.length - pending.length}, pendientes: ${pending.length}`);

  const startedAt = Date.now();
  let doneThisRun = 0;
  let failedThisRun = 0;
  let nextIndex = 0;

  // Simple pool: CONCURRENCY workers each pull the next pending entry off a
  // shared cursor and loop until the queue is empty. No locking needed for
  // `translations`/counters - JS is single-threaded, only the network wait
  // inside translate() actually overlaps between workers.
  async function worker() {
    while (nextIndex < pending.length) {
      const entry = pending[nextIndex++];
      try {
        translations[entry.infoHash] = await translate(entry.description);
        doneThisRun++;
      } catch (err) {
        failedThisRun++;
        console.error(`FALLO ${entry.infoHash} (${entry.name}): ${err.message}`);
      }

      if ((doneThisRun + failedThisRun) % CHECKPOINT_EVERY === 0) {
        saveTranslations(translations);
        const elapsedS = (Date.now() - startedAt) / 1000;
        const rate = doneThisRun / elapsedS;
        const remaining = pending.length - doneThisRun - failedThisRun;
        const etaMin = rate > 0 ? (remaining / rate) / 60 : 0;
        console.log(`[${doneThisRun + failedThisRun}/${pending.length}] guardado. ${rate.toFixed(2)}/s, ETA ${etaMin.toFixed(1)} min, fallos=${failedThisRun}`);
      }
    }
  }

  await Promise.all(Array.from({ length: Math.min(CONCURRENCY, pending.length) }, worker));

  saveTranslations(translations);
  console.log(`Listo. Traducidas esta corrida: ${doneThisRun}, fallos: ${failedThisRun}, total en archivo: ${Object.keys(translations).length}`);
}

main().catch(err => {
  console.error(String(err.stack || err));
  process.exit(1);
});

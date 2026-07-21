import fs from "node:fs";
import path from "node:path";
import Ajv, { ErrorObject } from "ajv";
import addFormats from "ajv-formats";

const schemaPath = path.resolve(__dirname, "../../../shared/catalog.schema.json");
const schema = JSON.parse(fs.readFileSync(schemaPath, "utf-8"));

const ajv = new Ajv({ allErrors: true });
addFormats(ajv);
const validateFn = ajv.compile(schema);

export interface ValidationResult {
  valid: boolean;
  errors: ErrorObject[] | null | undefined;
}

export function validateCatalogDocument(doc: unknown): ValidationResult {
  const valid = validateFn(doc) as boolean;
  return { valid, errors: validateFn.errors };
}

function formatErrors(errors: ErrorObject[] | null | undefined): string {
  if (!errors || errors.length === 0) return "(no error details)";
  return errors
    .map((e) => `  - ${e.instancePath || "/"} ${e.message}`)
    .join("\n");
}

// Runs when invoked directly (`npm run validate-catalog`), validating the
// on-disk catalog.json the same way the server does at startup.
if (require.main === module) {
  const catalogPath = path.resolve(__dirname, "../../data/catalog.json");
  const raw = fs.readFileSync(catalogPath, "utf-8");
  const doc = JSON.parse(raw);
  const result = validateCatalogDocument(doc);
  if (!result.valid) {
    console.error(`Catalog validation FAILED for ${catalogPath}:\n${formatErrors(result.errors)}`);
    process.exit(1);
  }
  console.log(`Catalog validation OK: ${catalogPath} (${doc.apps?.length ?? 0} app(s))`);
}

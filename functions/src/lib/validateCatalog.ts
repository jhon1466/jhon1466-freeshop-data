import fs from "node:fs";
import path from "node:path";
import Ajv, { ErrorObject } from "ajv";
import addFormats from "ajv-formats";

// Mirrors server/src/lib/validateCatalog.ts - kept in sync by hand since
// Firebase only deploys the functions/ directory itself (see functions/README.md).
const schemaPath = path.resolve(__dirname, "../../shared/catalog.schema.json");
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

export const GITHUB_OWNER = process.env.GITHUB_OWNER || "jhon1466";
export const GITHUB_REPO = process.env.GITHUB_REPO || "jhon1466-freeshop-data";
export const GITHUB_BRANCH = process.env.GITHUB_BRANCH || "main";
export const GITHUB_PATH = process.env.GITHUB_PATH || "data/catalog.json";

export function rawUrl(path: string): string {
  return `https://raw.githubusercontent.com/${GITHUB_OWNER}/${GITHUB_REPO}/${GITHUB_BRANCH}/${path}`;
}

export function contentsApiUrl(path: string): string {
  return `https://api.github.com/repos/${GITHUB_OWNER}/${GITHUB_REPO}/contents/${path}`;
}

// Public repo: reads go straight to the raw CDN, no token/API calls needed.
export const CATALOG_RAW_URL = rawUrl(GITHUB_PATH);
export const CATALOG_CONTENTS_API_URL = contentsApiUrl(GITHUB_PATH);

// Only needed for admin writes (commits). Never sent to the browser.
export const GITHUB_TOKEN = process.env.GITHUB_TOKEN || "";

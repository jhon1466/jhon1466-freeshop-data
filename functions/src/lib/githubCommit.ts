import { CatalogDocument } from "../types/catalog";
import { GITHUB_BRANCH, GITHUB_PATH, GITHUB_TOKEN, contentsApiUrl } from "./githubConfig";

export class GithubCommitError extends Error {}

function githubHeaders(): Record<string, string> {
  return {
    Authorization: `Bearer ${GITHUB_TOKEN}`,
    Accept: "application/vnd.github+json",
    "Content-Type": "application/json",
  };
}

async function getCurrentSha(path: string): Promise<string | undefined> {
  const res = await fetch(`${contentsApiUrl(path)}?ref=${GITHUB_BRANCH}`, {
    headers: githubHeaders(),
  });
  if (res.status === 404) return undefined;
  if (!res.ok) {
    throw new GithubCommitError(`Failed to read current sha for ${path}: HTTP ${res.status}`);
  }
  const body = (await res.json()) as { sha: string };
  return body.sha;
}

// Commits a single file via the GitHub Contents API. Each save is a real
// commit, giving a free audit trail/history in the data repo.
export async function commitFile(path: string, content: Buffer, message: string): Promise<void> {
  if (!GITHUB_TOKEN) {
    throw new GithubCommitError("GITHUB_TOKEN is not configured on the server.");
  }

  const sha = await getCurrentSha(path);

  const res = await fetch(contentsApiUrl(path), {
    method: "PUT",
    headers: githubHeaders(),
    body: JSON.stringify({
      message,
      content: content.toString("base64"),
      sha,
      branch: GITHUB_BRANCH,
    }),
  });

  if (!res.ok) {
    const details = await res.text();
    throw new GithubCommitError(`GitHub commit failed: HTTP ${res.status} ${details}`);
  }
}

export async function commitCatalog(doc: CatalogDocument): Promise<void> {
  await commitFile(
    GITHUB_PATH,
    Buffer.from(JSON.stringify(doc, null, 2) + "\n", "utf-8"),
    "Update catalog via admin panel"
  );
}

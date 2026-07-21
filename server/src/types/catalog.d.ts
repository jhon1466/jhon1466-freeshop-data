export interface AppEntrySource {
  origin: "curated" | "aggregated";
  sourceIndex?: string;
  sourceId?: string;
}

export interface AppEntry {
  id: string;
  title: string;
  author: string;
  category: string;
  description: string;
  longDescription?: string;
  version: string;
  iconUrl: string;
  downloadUrl: string;
  fileSize: number;
  sha256: string;
  nroFilename: string;
  homepageUrl?: string;
  license?: string;
  source?: AppEntrySource;
  updatedAt?: string;
}

export interface CatalogDocument {
  schemaVersion: 1;
  generatedAt: string;
  apps: AppEntry[];
}

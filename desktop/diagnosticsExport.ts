import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type { SupportBundle } from "../src/domain/production.ts";

export type SupportBundleExportResult = {
  path: string;
  bytes: number;
};

export async function writeSupportBundleExport(
  directory: string,
  bundle: SupportBundle
): Promise<SupportBundleExportResult> {
  await mkdir(directory, { recursive: true });
  const path = join(directory, `${bundle.id}.json`);
  const payload = `${JSON.stringify(bundle, null, 2)}\n`;
  await writeFile(path, payload, "utf8");
  return { path, bytes: new TextEncoder().encode(payload).length };
}
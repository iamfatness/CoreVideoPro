/** @typedef {import("./types.mjs").StoredLicense} StoredLicense */

const memory = new Map();

/**
 * @param {import("./types.mjs").StoredLicense} license
 * @param {{ LICENSE_KV?: { put(key: string, value: string): Promise<void> } }} env
 */
export async function saveLicense(license, env) {
  const serialized = JSON.stringify(license);
  memory.set(license.licenseKey, license);
  if (env.LICENSE_KV) {
    await env.LICENSE_KV.put(`license:${license.licenseKey}`, serialized);
  }
}

/**
 * @param {string} licenseKey
 * @param {{ LICENSE_KV?: { get(key: string): Promise<string | null> } }} env
 * @returns {Promise<StoredLicense | undefined>}
 */
export async function loadLicense(licenseKey, env) {
  if (memory.has(licenseKey)) {
    return memory.get(licenseKey);
  }
  if (env.LICENSE_KV) {
    const raw = await env.LICENSE_KV.get(`license:${licenseKey}`);
    if (raw) {
      const parsed = JSON.parse(raw);
      memory.set(licenseKey, parsed);
      return parsed;
    }
  }
  return undefined;
}

/** Test hook — clears the in-memory store. */
export function resetLicenseStore() {
  memory.clear();
}
import { ensureE2eMainBundle, ensurePreloadBundle } from "../../desktop/scripts/desktopRuntime.mjs";

export default async function globalSetup() {
  ensurePreloadBundle();
  ensureE2eMainBundle();
}
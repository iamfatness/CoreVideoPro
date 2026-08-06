/**
 * Operator role overrides.
 * Mukana declares each panelist's role, but the operator must be able to move
 * the host or reader chair mid-show. This table takes precedence over Mukana
 * in the panelist join, and it guarantees the exclusive roles have exactly one
 * holder across both sources. Rows are keyed by person key rather than PIN, so
 * a show with no Mukana registry — and therefore no PINs — can still have its
 * host or reader chair moved. The registry itself stays PIN-keyed, because it
 * is a registry, not an override table: when a registry-declared holder is
 * demoted, its row is written under `pin:<PIN>`, exactly the key
 * `resolvePersonKey` produces for a participant carrying that PIN.
 */

import type { MukanaDb, Role } from "./contracts.js";
import type { PersonKey } from "./personKey.js";

export type OverrideRecord = {
  personKey: PersonKey;
  displayName: string;
  location: string;
  role: Role;
};

export class OverrideDb {
  private entriesByPersonKey = new Map<PersonKey, OverrideRecord>();

  set(record: OverrideRecord): void {
    this.entriesByPersonKey.set(record.personKey, { ...record });
  }

  delete(personKey: PersonKey): void {
    this.entriesByPersonKey.delete(personKey);
  }

  roleFor(personKey: PersonKey): Role | undefined {
    return this.entriesByPersonKey.get(personKey)?.role;
  }

  entries(): Record<PersonKey, OverrideRecord> {
    return Object.fromEntries(
      [...this.entriesByPersonKey.entries()].map(([personKey, record]) => [
        personKey,
        { ...record }
      ])
    );
  }

  clear(): void {
    this.entriesByPersonKey.clear();
  }

  restore(entries: Record<PersonKey, OverrideRecord>): void {
    this.entriesByPersonKey = new Map(
      Object.entries(entries).map(([personKey, record]) => [personKey, { ...record }])
    );
  }

  /**
   * Give `personKey` an exclusive role, guaranteeing it is the only holder.
   * Prior holders declared by the registry are demoted with an explicit
   * override row keyed `pin:<PIN>`; prior holders that existed only as
   * overrides have their row removed.
   */
  assignExclusiveRole(personKey: PersonKey, role: "host" | "reader", registry: MukanaDb): void {
    const priorRecord = this.entriesByPersonKey.get(personKey);

    for (const record of Object.values(registry)) {
      if (record.role !== role) continue;
      const registryPersonKey: PersonKey = `pin:${record.pin}`;
      if (this.entriesByPersonKey.has(registryPersonKey)) continue;
      this.entriesByPersonKey.set(registryPersonKey, {
        personKey: registryPersonKey,
        displayName: record.displayName,
        location: record.location,
        role: "panelist"
      });
    }

    for (const [existingPersonKey, record] of [...this.entriesByPersonKey.entries()]) {
      if (record.role !== role) continue;
      const registryPin = existingPersonKey.startsWith("pin:")
        ? existingPersonKey.slice("pin:".length)
        : undefined;
      if (registryPin !== undefined && registry[registryPin]?.role === role) {
        this.entriesByPersonKey.set(existingPersonKey, { ...record, role: "panelist" });
      } else {
        this.entriesByPersonKey.delete(existingPersonKey);
      }
    }

    const registryPin = personKey.startsWith("pin:") ? personKey.slice("pin:".length) : undefined;
    const registryRecord = registryPin === undefined ? undefined : registry[registryPin];
    this.entriesByPersonKey.set(personKey, {
      personKey,
      displayName: registryRecord?.displayName ?? priorRecord?.displayName ?? "",
      location: registryRecord?.location ?? priorRecord?.location ?? "",
      role
    });
  }
}

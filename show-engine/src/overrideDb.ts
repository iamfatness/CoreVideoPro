/**
 * Operator role overrides.
 * Mukana declares each panelist's role, but the operator must be able to move
 * the host or reader chair mid-show. This table takes precedence over Mukana
 * in the panelist join, and it guarantees the exclusive roles have exactly one
 * holder across both sources. Rows are keyed by person key rather than PIN, so
 * a show with no Mukana registry — and therefore no PINs — can still have its
 * host or reader chair moved. The registry itself stays PIN-keyed, because it
 * is a registry, not an override table: when a registry-declared holder is
 * demoted, its row is written under `personKeyForPin(pin)`, exactly the key
 * `resolvePersonKey` produces for a participant carrying that PIN. That
 * shared helper is the only place the PIN-tier prefix is spelled.
 */

import type { MukanaDb, MukanaRecord } from "./contracts.js";
import type { Role } from "./contracts.js";
import { personKeyForPin, type PersonKey } from "./personKey.js";

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
   *
   * The registry is PIN-keyed and this table is person-keyed, so the two are
   * bridged by re-keying the registry through `personKeyForPin` once, up
   * front. That direction is the safe one: a `PersonKey` is opaque and is
   * never taken apart to recover a PIN, so a non-registry key can never be
   * mistaken for a registry one.
   */
  assignExclusiveRole(personKey: PersonKey, role: "host" | "reader", registry: MukanaDb): void {
    const priorRecord = this.entriesByPersonKey.get(personKey);

    const registryByPersonKey = new Map<PersonKey, MukanaRecord>();
    for (const record of Object.values(registry)) {
      registryByPersonKey.set(personKeyForPin(record.pin), record);
    }

    for (const [registryPersonKey, record] of registryByPersonKey) {
      if (record.role !== role) continue;
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
      if (registryByPersonKey.get(existingPersonKey)?.role === role) {
        this.entriesByPersonKey.set(existingPersonKey, { ...record, role: "panelist" });
      } else {
        this.entriesByPersonKey.delete(existingPersonKey);
      }
    }

    const registryRecord = registryByPersonKey.get(personKey);
    this.entriesByPersonKey.set(personKey, {
      personKey,
      displayName: registryRecord?.displayName ?? priorRecord?.displayName ?? "",
      location: registryRecord?.location ?? priorRecord?.location ?? "",
      role
    });
  }
}

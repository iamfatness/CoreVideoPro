/**
 * Operator role overrides.
 * Mukana declares each panelist's role, but the operator must be able to move
 * the host or reader chair mid-show. This table takes precedence over Mukana
 * in the panelist join, and it guarantees the exclusive roles have exactly one
 * holder across both sources.
 */

import type { MukanaDb, Role } from "./contracts.js";

export type OverrideRecord = {
  pin: string;
  displayName: string;
  location: string;
  role: Role;
};

export class OverrideDb {
  private entriesByPin = new Map<string, OverrideRecord>();

  set(record: OverrideRecord): void {
    this.entriesByPin.set(record.pin, { ...record });
  }

  delete(pin: string): void {
    this.entriesByPin.delete(pin);
  }

  roleFor(pin: string): Role | undefined {
    return this.entriesByPin.get(pin)?.role;
  }

  entries(): Record<string, OverrideRecord> {
    return Object.fromEntries(
      [...this.entriesByPin.entries()].map(([pin, record]) => [pin, { ...record }])
    );
  }

  clear(): void {
    this.entriesByPin.clear();
  }

  restore(entries: Record<string, OverrideRecord>): void {
    this.entriesByPin = new Map(
      Object.entries(entries).map(([pin, record]) => [pin, { ...record }])
    );
  }

  /**
   * Give `pin` an exclusive role, guaranteeing it is the only holder.
   * Prior holders declared by Mukana are demoted with an explicit override row;
   * prior holders that existed only as overrides have their row removed.
   */
  assignExclusiveRole(pin: string, role: "host" | "reader", mukana: MukanaDb): void {
    for (const record of Object.values(mukana)) {
      if (record.role !== role) continue;
      if (this.entriesByPin.has(record.pin)) continue;
      this.entriesByPin.set(record.pin, {
        pin: record.pin,
        displayName: record.displayName,
        location: record.location,
        role: "panelist"
      });
    }

    for (const [existingPin, record] of [...this.entriesByPin.entries()]) {
      if (record.role !== role) continue;
      if (mukana[existingPin]?.role === role) {
        this.entriesByPin.set(existingPin, { ...record, role: "panelist" });
      } else {
        this.entriesByPin.delete(existingPin);
      }
    }

    const mukanaRecord = mukana[pin];
    this.entriesByPin.set(pin, {
      pin,
      displayName: mukanaRecord?.displayName ?? "",
      location: mukanaRecord?.location ?? "",
      role
    });
  }
}

/**
 * The shared active-speaker gate. A single predicate that every consumer of
 * an active-speaker event asks first, so an accessibility exemption (e.g. an
 * ASL interpreter who signs continuously while a panelist talks) is honored
 * everywhere an active-speaker event fans out — not just inside `ProgramBus`.
 */

import type { Role } from "./contracts.js";

/**
 * Whether an active-speaker event for this role should be allowed to affect
 * on-screen state. `role` is `null` when the speaker is not seated and
 * therefore has no editorial role; that returns `true` deliberately — an
 * unseated speaker is an ordinary participant until the roster says
 * otherwise, and returning `false` would silently freeze active-speaker
 * follow whenever someone unseated talks.
 */
export function shouldFollowSpeaker(role: Role | null, skipRoles: readonly Role[]): boolean {
  if (role === null) {
    return true;
  }
  return !skipRoles.includes(role);
}

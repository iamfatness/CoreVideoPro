/**
 * The package's sole wall-clock seam.
 *
 * Every other module in this package is already deterministic: given the
 * same inputs, it produces the same outputs, which is what makes the
 * existing test suite (and the orchestrator built on top of it) reliable
 * and replayable. `Date.now()` is the one input that breaks that — a
 * direct call bakes real wall-clock time into otherwise-pure logic and
 * makes it untestable without waiting on the clock or patching globals.
 *
 * `clock.ts` is the ONLY place in this package permitted to read the
 * system clock. Every module that needs "now" takes a `Clock` (defaulting
 * to `systemClock` at the host-adapter boundary) instead of calling
 * `Date.now()` itself, so tests can inject a fake clock and stay
 * deterministic.
 */

export type Clock = { now(): number };

export const systemClock: Clock = {
  now(): number {
    return Date.now();
  }
};

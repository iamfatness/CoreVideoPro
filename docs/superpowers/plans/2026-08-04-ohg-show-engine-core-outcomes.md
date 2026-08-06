# OHG Show Engine — Plan 1 Outcomes

Companion to `2026-08-04-ohg-show-engine-core.md`. Records what shipped, what was
deliberately left undone, and what Plans 2–6 should account for. Written at the close of
Plan 1 execution (2026-08-04).

**Shipped:** `show-engine/` workspace package — 11 modules, 129 tests, typecheck and build
clean, registered in the root `workspaces` array and in `test:gate`.

## Deliberate divergences from the plan document

The plan's sample code contained five defects found during review. The shipped code is
correct where it diverges; **the plan document was intentionally not rewritten**, so read
the code, not the plan's code blocks, as the source of truth.

1. `ZoomIngest.commit()` deep-copies published participants; `roster`/`joined` compare
   before marking the cache dirty. (Plan aliased, and always dirtied — which would have
   made a poll-rate host force a full downstream recompute every tick.)
2. `MukanaRegistry.merge()`/`current()` deep-copy records, not just the container.
3. `OverrideDb.assignExclusiveRole` resolves identity registry → existing override row →
   `""`. The plan always used the registry, blanking operator-typed identity for guests
   who are not registered.
4. `persistence.ts` — `parentDirectory` handles no-separator and root-level paths (the
   plan's version could make `mkdir` collide with the rename target); `load()` validates
   document shape rather than admitting `null` or a non-array `seats`.
5. `LiveSlots.fromJSON` throws the exported `LiveSlotsRestoreError` on an incoherent
   roster; `rebuild()` returns the panelists it could not seat instead of dropping them
   silently.

Divergences 1–3 were ruled on by the owner; 4–5 came out of the final whole-branch review.

## Known boundaries (deliberate, not oversights)

- **`OverrideDb` is authoritative for editorial roles.** `LiveSlots`' exclusive-role
  enforcement is view-level repair only, and its tie-break differs from `add`/`replace`:
  `refresh` is deterministic lowest-slot-wins, `add`/`replace` is newest-wins. A
  characterization test pins this. Every role mutation must go through
  `assignExclusiveRole` or it will visibly revert on the next refresh.
- **`ZoomIngest` never evicts.** `left` marks a participant offline and keeps the record so
  a reconnecting panelist can hold their seat; only a full `roster` event prunes.
- **`MukanaClient` owns no timer.** One fetch per call; it reports `nextDelayMs()` and the
  caller owns the loop.
- **`load()` vs `fromJSON` split.** `load()` answers "is this a `ShowState`-shaped
  document" (returns `null`); `fromJSON` answers "is this a coherent roster" (throws
  `LiveSlotsRestoreError`). A restore failure is catchable and downgradable to a clean
  start plus a health warning — it must never take the engine down before air.

## Carried into later plans

- **`ZoomIngest.snapshot()` deep-copies on every call**, redundant now that `commit()`
  copies. Do **not** fix by removing the copy — that would make it the one accessor in the
  package that aliases. Add a monotonic `revision` counter bumped in `commit()` and let the
  orchestrator memoize on the integer.
- **`MukanaHealth` is single-endpoint.** `nextDelayMs()` hardcodes `panelistsIntervalMs`,
  but `handsIntervalMs`/`questionIntervalMs` already exist in config and will need
  consumers. Make health per-endpoint before those land, not after.
- **Do not carry the `rebuild(everyone)` idiom into the orchestrator.** The integration
  tests seat the whole panelist DB because the fixtures are tiny; a real meeting has far
  more participants than `capacity`, so that call would fill every slot with whoever sorts
  first by `participantId`. Seating is a deliberate operator act (`ohg.panelist.add`).
- Smaller deferrals: `readPin` accepts any integer/string where only 4 digits can ever
  join; the dormant gate keys on `"status" in root` without a type check; newline stripping
  concatenates words (`"Ann\nLee"` → `"AnnLee"`); utility-tail placement keys on any parsed
  PIN ≥ 9000 including walk-ins; `MukanaClient` conflates transport and parse failure in
  one `invalid` outcome; `StateFs.mkdir` must be recursive and idempotent but nothing says
  so; `OverrideDb.set()` bypasses the exclusivity guarantee its header advertises, and
  `restore()` trusts persisted `role` values without `coerceRole`.
- Consider `noUncheckedIndexedAccess` in `show-engine/tsconfig.json`. Several guards are
  already written as if it were on, and it would have caught the phantom-`{}` panelist at
  compile time. It is a divergence from `native-core`, so it is a judgment call.

## Process note for Plans 2–6

Five review findings originated in Plan 1's own sample code bodies. The **Interfaces**
blocks and **Behavior** prose were what let twelve independent implementers produce a
coherent package — those earned their place. The full implementation bodies mostly created
conflicts to adjudicate. Later plans should keep the interfaces and the prose plus the test
list, and drop the sample implementations. Each plan's Definition of Done should also
include CI-gate registration, not just workspace registration.

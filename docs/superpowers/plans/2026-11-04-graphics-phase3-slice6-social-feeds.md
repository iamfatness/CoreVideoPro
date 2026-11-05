# Graphics Phase 3 Slice 6 — YouTube social feed implementation

Tracks [#1113](https://github.com/iamfatness/CoreVideoPro/issues/1113). Use the [Slice 6 spec](../specs/2026-11-04-graphics-phase3-slice6-social-feeds.md) as the contract. Run and record the focused failing case before each task, implement the smallest passing change, then review the diff and commit. Keep evidence under `D:/CoreVideo-QA/tmp`. Never use the installed app or owner show files.

## 1. Private review state

Add a focused MediaCore session queue with exact revisions, decisions, bounded item and tombstone counts and injected time. Test pending/approve/reject, a stale review click after an edit, approval of one of two items, duplicate delivery, delete-before-arrival, expiry, overflow and old-source callbacks after disconnect. Keep the type independent of provider I/O and the native core. Commit **Model exact-version social graphic review**.

## 2. Approved-only native playout

Build the dedicated shell playout service using the existing native template and instance contracts. Use a dedicated instance with a non-riding private cue, separated from the saved playlist and generic show-from-cue paths. Trace all native entry points before selecting the instance isolation guard. Test the exact approved payload and no native call on approval alone. Drive delete, edit and disconnect while readiness is held. Exercise ordinary Take, layout, hotkey and control action bypasses and assert no social Program mutation. Verify native epoch and prepared-revision fences, then verify hide of the active item. Commit **Gate social graphics at Program submission**.

## 3. Connect the YouTube adapter

Use the documented Google video lookup and paged chat with a fake transport, bounded responses, provider-paced polling and explicit cancellation. Map only text and invalidations. Test continuation, replay, delayed callbacks, authorization failure, quota/backoff, ended chats and malformed messages. Use the existing protected-secret store facility with a separate provider binding and exercise removal, credential scoping and redacted failures. Confirm no provider bytes enter diagnostics. Commit **Connect public YouTube live chat to social review**.

## 4. Operator review and qualification

Add a focused WinUI panel with connect, review, approve/reject, show/hide, status and local-data removal. Bind the click to the exact displayed item. Guard async handlers. Add compiled-layout and isolated runtime coverage, including Program pixels, an unapproved item and revocation while preparing. Verify the reference layout sizes and inspect screenshots. Document the workflow in the graphics reference and update the guide index. Run affected shell suites and hosted CI. Record a controlled public provider test separately. Open a PR for #1113 with fixture, layout, provider and live acceptance status. Commit **Expose and qualify social graphic review in Operate**.

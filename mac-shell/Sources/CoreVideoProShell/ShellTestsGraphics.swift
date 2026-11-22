// Graphics in the Mac shell (G1-G3) — shell tests.
// Plan: docs/superpowers/plans/2026-11-21-mac-graphics-shell-g1-g3.md.
// Registered in ShellTests.runAndExit via `graphicsCases`.

import Foundation
import Security

extension ShellTests {
    static var graphicsCases: [(String, () -> Void)] {
        [
            ("keychain/store-roundtrip-memory", testKeychainStoreMemoryRoundTrip),
            ("keychain/existing-items-keep-their-names", testKeychainUsersKeepTheirItemNames),
            ("keychain/system-roundtrip", testKeychainStoreSystemRoundTrip),
        ]
    }

    // ── Task 3: KeychainStore ───────────────────────────────────────────────

    static func testKeychainStoreMemoryRoundTrip() {
        let memory = MemoryKeychain()
        let store = KeychainStore(service: "test.service", account: "a", backend: memory)
        expect(store.read() == nil, "missing item reads nil")
        expect(store.write(Data("one".utf8)), "write succeeds")
        expectEqual(store.readString(), "one", "read back")
        expect(store.write(Data("two".utf8)), "overwrite succeeds")
        expectEqual(store.readString(), "two", "overwrite replaces, never duplicates")
        expectEqual(memory.keys.count, 1, "one item per (service, account)")
        let other = KeychainStore(service: "test.service", account: "b", backend: memory)
        expect(other.read() == nil, "another account is another item")
        expect(store.writeString(""), "empty string deletes")
        expect(store.read() == nil, "deleted")
        expect(store.delete(), "deleting a missing item succeeds")
    }

    /// The two existing secrets moved onto KeychainStore WITHOUT renaming their
    /// items, or a stream key / Zoom sign-in saved by an older build is lost.
    static func testKeychainUsersKeepTheirItemNames() {
        let previous = KeychainStore.defaultBackend
        let memory = MemoryKeychain()
        KeychainStore.defaultBackend = memory
        defer { KeychainStore.defaultBackend = previous }
        StreamKeychain.save("rtmp-secret")
        expectEqual(memory.read(service: "us.iamfatness.corevideopro.stream-key", account: "rtmp")
                        .flatMap { String(data: $0, encoding: .utf8) }, "rtmp-secret",
                    "the stream key keeps its Keychain item name")
        expectEqual(StreamKeychain.load(), "rtmp-secret", "and loads back through the store")
        StreamKeychain.save("")
        expect(memory.read(service: "us.iamfatness.corevideopro.stream-key", account: "rtmp") == nil,
               "an empty key deletes the item (StreamKeychain contract)")
        expectEqual(ZoomOAuth.keychain.service, "us.iamfatness.corevideopro.zoom-oauth", "Zoom tokens keep their service")
        expectEqual(ZoomOAuth.keychain.account, "tokens", "and their account")
        expect(!ZoomOAuth.keychain.service.contains("OBS"), "never the OBS plugin's item")
    }

    /// One real login-Keychain round trip under a throwaway service. Skipped
    /// (loudly) where the Keychain is not usable, e.g. a locked CI keychain.
    static func testKeychainStoreSystemRoundTrip() {
        let store = KeychainStore(service: "us.iamfatness.corevideopro.shell-test.\(UUID().uuidString)",
                                  account: "roundtrip", backend: SystemKeychain.shared)
        defer { store.delete() }
        guard store.write(Data("secret".utf8)) else {
            FileHandle.standardError.write(Data("  [skip] login Keychain not writable here\n".utf8))
            return
        }
        expectEqual(store.readString(), "secret", "system Keychain read back")
        expect(store.write(Data("updated".utf8)), "system overwrite")
        expectEqual(store.readString(), "updated", "update replaces the existing item")
        expect(store.delete(), "system delete")
        expect(store.read() == nil, "gone after delete")
    }
}

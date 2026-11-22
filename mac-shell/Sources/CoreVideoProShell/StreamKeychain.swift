// Stream-key storage. House rule: never persist a secret plaintext — the
// Windows shell DPAPI-encrypts the RTMP key (prefs S4); the mac analogue is
// a Keychain generic-password item. The prefs file only ever stores the URL.
// Stored through the shared KeychainStore; the item name is unchanged so a key
// saved by an older build is still found.

import Foundation

enum StreamKeychain {
    static var store: KeychainStore {
        KeychainStore(service: "us.iamfatness.corevideopro.stream-key", account: "rtmp")
    }

    static func load() -> String { store.readString() ?? "" }

    /// An empty key deletes the item.
    static func save(_ key: String) { store.writeString(key) }
}

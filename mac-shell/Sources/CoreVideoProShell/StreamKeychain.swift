// Stream-key storage. House rule: never persist a secret plaintext — the
// Windows shell DPAPI-encrypts the RTMP key (prefs S4); the mac analogue is
// a Keychain generic-password item. The prefs file only ever stores the URL.
// Stored through the shared KeychainStore; the item name is unchanged so a key
// saved by an older build is still found.

import Foundation

enum StreamKeychain {
    private static let service = "us.iamfatness.corevideopro.stream-key"
    private static let account = "rtmp"
    /// The SRT destination's passphrase (mac streaming Phase 2, D1): same
    /// service, its own account, never in ShellPrefs.
    static let srtPassphraseAccount = "srt-passphrase"

    static func store(account: String = StreamKeychain.account) -> KeychainStore {
        KeychainStore(service: service, account: account)
    }

    static func load(account: String = StreamKeychain.account) -> String {
        store(account: account).readString() ?? ""
    }

    /// An empty key deletes the item.
    static func save(_ key: String, account: String = StreamKeychain.account) {
        store(account: account).writeString(key)
    }
}

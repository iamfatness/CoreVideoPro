// Stream-key storage. House rule: never persist a secret plaintext — the
// Windows shell DPAPI-encrypts the RTMP key (prefs S4); the mac analogue is
// a Keychain generic-password item. The prefs file only ever stores the URL.

import Foundation
import Security

enum StreamKeychain {
    private static let service = "us.iamfatness.corevideopro.stream-key"
    private static let account = "rtmp"

    static func load() -> String {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let key = String(data: data, encoding: .utf8)
        else { return "" }
        return key
    }

    static func save(_ key: String) {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        if key.isEmpty {
            SecItemDelete(base as CFDictionary)
            return
        }
        let payload = Data(key.utf8)
        let update: [String: Any] = [kSecValueData as String: payload]
        let status = SecItemUpdate(base as CFDictionary, update as CFDictionary)
        if status == errSecItemNotFound {
            var add = base
            add[kSecValueData as String] = payload
            SecItemAdd(add as CFDictionary, nil)
        }
    }
}

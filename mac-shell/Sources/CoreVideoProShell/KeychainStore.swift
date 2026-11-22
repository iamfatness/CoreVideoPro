// KeychainStore — the one generic-password helper (master plan B1 / graphics G1,
// spec docs/superpowers/specs/2026-11-15-mac-graphics-shell-design.md 5.8).
//
// It replaces the two hand-written SecItem copies (StreamKeychain, ZoomOAuth) and
// is the store graphics feed credentials (G9) and social review (B2-B4) use: one
// item per (service, account), get / set / delete, no access group (the shell is
// not sandboxed). Callers keep their own item names, so a secret saved by an
// older build is still found.
//
// The backend is a seam: production is the login Keychain; the shell test
// harness swaps in an in-memory backend so no test writes a real secret.

import Foundation
import Security

protocol KeychainBackend: AnyObject {
    func read(service: String, account: String) -> Data?
    /// Creates or replaces the item. False when the Keychain refused.
    func write(_ data: Data, service: String, account: String) -> Bool
    /// True when the item is gone afterwards (deleting a missing item succeeds).
    func delete(service: String, account: String) -> Bool
}

/// The login Keychain (generic-password items).
final class SystemKeychain: KeychainBackend {
    static let shared = SystemKeychain()

    private func base(_ service: String, _ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    func read(service: String, account: String) -> Data? {
        var query = base(service, account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    func write(_ data: Data, service: String, account: String) -> Bool {
        let query = base(service, account)
        let status = SecItemUpdate(query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecSuccess { return true }
        guard status == errSecItemNotFound else { return false }
        var add = query
        add[kSecValueData as String] = data
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }

    func delete(service: String, account: String) -> Bool {
        let status = SecItemDelete(base(service, account) as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }
}

/// In-memory backend for the shell test harness.
final class MemoryKeychain: KeychainBackend {
    private let lock = NSLock()
    private var items: [String: Data] = [:]
    private func key(_ service: String, _ account: String) -> String { service + "\u{0}" + account }

    func read(service: String, account: String) -> Data? {
        lock.lock(); defer { lock.unlock() }
        return items[key(service, account)]
    }
    func write(_ data: Data, service: String, account: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        items[key(service, account)] = data
        return true
    }
    func delete(service: String, account: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        items[key(service, account)] = nil
        return true
    }
    /// Every stored (service, account) pair, for tests.
    var keys: [(service: String, account: String)] {
        lock.lock(); defer { lock.unlock() }
        return items.keys.map { pair in
            let parts = pair.split(separator: "\u{0}", maxSplits: 1, omittingEmptySubsequences: false)
            return (String(parts[0]), parts.count > 1 ? String(parts[1]) : "")
        }
    }
}

struct KeychainStore {
    /// The backend new stores use. Production: the login Keychain. The test
    /// harness replaces it before any case runs.
    static var defaultBackend: KeychainBackend = SystemKeychain.shared

    let service: String
    let account: String
    private let backendOverride: KeychainBackend?

    init(service: String, account: String, backend: KeychainBackend? = nil) {
        self.service = service
        self.account = account
        self.backendOverride = backend
    }

    private var backend: KeychainBackend { backendOverride ?? Self.defaultBackend }

    func read() -> Data? { backend.read(service: service, account: account) }
    @discardableResult func write(_ data: Data) -> Bool { backend.write(data, service: service, account: account) }
    @discardableResult func delete() -> Bool { backend.delete(service: service, account: account) }

    func readString() -> String? { read().flatMap { String(data: $0, encoding: .utf8) } }
    /// An empty string deletes the item (a cleared secret leaves nothing behind).
    @discardableResult func writeString(_ value: String) -> Bool {
        value.isEmpty ? delete() : write(Data(value.utf8))
    }
}

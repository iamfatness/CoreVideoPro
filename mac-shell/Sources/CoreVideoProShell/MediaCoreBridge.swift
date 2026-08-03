// MediaCoreBridge — spawns and supervises corevideo-native, speaking the
// line-delimited JSON-RPC protocol (docs/mac-port-phase4-swiftui-shell.md).
// Requests carry an id; responses echo it; interleaved event lines (no
// pending id match) are published to `events`. The bridge owns the process
// lifecycle: exit -> status surfaced + relaunch with backoff (loud, never
// silent).

import Foundation

typealias JSONObject = [String: Any]

enum BridgeStatus: Equatable {
    case launching
    case connected(renderer: String)
    case exited(code: Int32)
    case failed(String)
}

final class MediaCoreBridge {
    private let corePath: String
    private let environmentExtras: [String: String]
    private var process: Process?
    private var stdinHandle: FileHandle?
    private var stdoutBuffer = Data()
    private var nextId = 1
    private var pending: [String: (Result<JSONObject, Error>) -> Void] = [:]
    private let lock = NSLock()
    private var relaunchAttempts = 0
    private var stopped = false

    var onStatus: ((BridgeStatus) -> Void)?
    var onEvent: ((JSONObject) -> Void)?
    var onStderrLine: ((String) -> Void)?

    init(corePath: String, environmentExtras: [String: String]) {
        self.corePath = corePath
        self.environmentExtras = environmentExtras
    }

    func start() {
        stopped = false
        launch()
    }

    func stop() {
        stopped = true
        process?.terminate()
        process = nil
    }

    private func launch() {
        onStatus?(.launching)
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: corePath)
        var env = ProcessInfo.processInfo.environment
        for (key, value) in environmentExtras {
            env[key] = value
        }
        proc.environment = env
        let stdinPipe = Pipe()
        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        proc.standardInput = stdinPipe
        proc.standardOutput = stdoutPipe
        proc.standardError = stderrPipe
        stdinHandle = stdinPipe.fileHandleForWriting

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.consumeStdout(handle.availableData)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            guard let text = String(data: handle.availableData, encoding: .utf8), !text.isEmpty
            else { return }
            for line in text.split(separator: "\n") {
                self?.onStderrLine?(String(line))
            }
        }
        proc.terminationHandler = { [weak self] finished in
            guard let self else { return }
            self.failAllPending(BridgeError.processExited)
            self.onStatus?(.exited(code: finished.terminationStatus))
            guard !self.stopped else { return }
            self.relaunchAttempts += 1
            let delay = min(30.0, pow(2.0, Double(self.relaunchAttempts)))
            DispatchQueue.global().asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self, !self.stopped else { return }
                self.launch()
            }
        }
        do {
            try proc.run()
            process = proc
        } catch {
            onStatus?(.failed("core launch failed: \(error.localizedDescription)"))
        }
    }

    private func consumeStdout(_ data: Data) {
        guard !data.isEmpty else { return }
        stdoutBuffer.append(data)
        while let newline = stdoutBuffer.firstIndex(of: 0x0A) {
            let lineData = stdoutBuffer.subdata(in: stdoutBuffer.startIndex..<newline)
            stdoutBuffer.removeSubrange(stdoutBuffer.startIndex...newline)
            guard !lineData.isEmpty,
                  let object = (try? JSONSerialization.jsonObject(with: lineData)) as? JSONObject
            else { continue }
            dispatch(object)
        }
    }

    private func dispatch(_ object: JSONObject) {
        if let id = object["id"] as? String {
            if id == "handshake" {
                relaunchAttempts = 0
                let renderer =
                    ((object["profile"] as? JSONObject)?["renderer"] as? String) ?? "unknown"
                onStatus?(.connected(renderer: renderer))
                return
            }
            lock.lock()
            let completion = pending.removeValue(forKey: id)
            lock.unlock()
            if let completion {
                if (object["ok"] as? Bool) == false {
                    let message = (object["error"] as? JSONObject)?["message"] as? String
                    completion(.failure(BridgeError.remote(message ?? "request failed")))
                } else {
                    completion(.success(object))
                }
                return
            }
        }
        onEvent?(object)
    }

    private func failAllPending(_ error: Error) {
        lock.lock()
        let all = pending
        pending.removeAll()
        lock.unlock()
        for (_, completion) in all {
            completion(.failure(error))
        }
    }

    func request(_ body: JSONObject, timeout: TimeInterval = 6.0) async throws -> JSONObject {
        lock.lock()
        let id = "shell-\(nextId)"
        nextId += 1
        lock.unlock()
        var payload = body
        payload["id"] = id
        let data = try JSONSerialization.data(withJSONObject: payload)
        return try await withCheckedThrowingContinuation { continuation in
            let box = CompletionBox(continuation: continuation)
            lock.lock()
            pending[id] = { box.finish(with: $0) }
            lock.unlock()
            guard let handle = stdinHandle else {
                lock.lock()
                pending.removeValue(forKey: id)
                lock.unlock()
                box.finish(with: .failure(BridgeError.notRunning))
                return
            }
            handle.write(data)
            handle.write(Data([0x0A]))
            DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { [weak self] in
                guard let self else { return }
                self.lock.lock()
                let timedOut = self.pending.removeValue(forKey: id)
                self.lock.unlock()
                if timedOut != nil {
                    box.finish(with: .failure(BridgeError.timeout))
                }
            }
        }
    }
}

enum BridgeError: Error, LocalizedError {
    case notRunning
    case timeout
    case processExited
    case remote(String)

    var errorDescription: String? {
        switch self {
        case .notRunning: return "media core is not running"
        case .timeout: return "media core request timed out"
        case .processExited: return "media core exited"
        case .remote(let message): return message
        }
    }
}

// Guards the continuation against double-resume across the timeout race.
private final class CompletionBox {
    private var continuation: CheckedContinuation<JSONObject, Error>?
    private let lock = NSLock()

    init(continuation: CheckedContinuation<JSONObject, Error>) {
        self.continuation = continuation
    }

    func finish(with result: Result<JSONObject, Error>) {
        lock.lock()
        let taken = continuation
        continuation = nil
        lock.unlock()
        switch (taken, result) {
        case (.some(let cont), .success(let value)): cont.resume(returning: value)
        case (.some(let cont), .failure(let error)): cont.resume(throwing: error)
        case (.none, _): break
        }
    }
}

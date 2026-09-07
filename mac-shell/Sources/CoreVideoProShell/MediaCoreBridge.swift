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

// Every state transition occurs on MediaCoreBridge.stateQueue. The small policy
// is independently testable without launching a process or loading the UI.
struct BridgeGenerationPolicy {
    private(set) var generation: UInt64 = 0
    private(set) var stopped = true
    private(set) var running = false
    private(set) var ready = false

    mutating func begin() -> UInt64 {
        generation &+= 1
        stopped = false
        running = true
        ready = false
        return generation
    }

    mutating func invalidate(stopped: Bool) -> UInt64 {
        generation &+= 1
        self.stopped = stopped
        running = false
        ready = false
        return generation
    }

    func isCurrent(_ token: UInt64) -> Bool { token == generation && running && !stopped }
    func canWrite(_ token: UInt64) -> Bool { isCurrent(token) && ready }
    func canRelaunch(_ token: UInt64) -> Bool { token == generation && !running && !stopped }

    mutating func acceptHandshake(_ token: UInt64) -> Bool {
        guard isCurrent(token) else { return false }
        ready = true
        return true
    }
}

final class MediaCoreBridge {
    private let corePath: String
    private let environmentExtras: [String: String]
    // Process identity, framing, pending requests and recovery scheduling all
    // belong to one serial executor. Blocking pipe writes never run on it.
    private let stateQueue = DispatchQueue(label: "us.iamfatness.corevideopro.bridge-state")
    private let callbackQueue = DispatchQueue(label: "us.iamfatness.corevideopro.bridge-callback")
    private let writeQueue = DispatchQueue(label: "us.iamfatness.corevideopro.bridge-write")
    private var policy = BridgeGenerationPolicy()
    private var process: Process?
    private var stdinHandle: FileHandle?
    private var lineBuffer: [UInt8] = []
    private var nextId = 1
    private var pending: [String: CompletionBox] = [:]
    private var relaunchAttempts = 0

    var onStatus: ((BridgeStatus) -> Void)?
    var onEvent: ((JSONObject) -> Void)?
    var onStderrLine: ((String) -> Void)?

    init(corePath: String, environmentExtras: [String: String]) {
        self.corePath = corePath
        self.environmentExtras = environmentExtras
    }

    func start() {
        stateQueue.async { [weak self] in
            guard let self, !self.policy.running else { return }
            self.launch()
        }
    }

    func stop() {
        let detached = stateQueue.sync { () -> (Process?, FileHandle?, [CompletionBox]) in
            _ = policy.invalidate(stopped: true)
            let retired = detachChild()
            let waiters = Array(pending.values)
            pending.removeAll()
            return (retired.0, retired.1, waiters)
        }
        // No callbacks or process disposal while owning the state executor.
        Self.retire(detached.0, stdin: detached.1)
        callbackQueue.async {
            for waiter in detached.2 { waiter.finish(with: .failure(BridgeError.processExited)) }
        }
    }

    // stateQueue only.
    private func detachChild() -> (Process?, FileHandle?) {
        let detached = (process, stdinHandle)
        process = nil
        stdinHandle = nil
        lineBuffer.removeAll(keepingCapacity: true)
        return detached
    }

    private static func retire(_ proc: Process?, stdin: FileHandle?) {
        proc?.terminationHandler = nil
        (proc?.standardOutput as? Pipe)?.fileHandleForReading.readabilityHandler = nil
        (proc?.standardError as? Pipe)?.fileHandleForReading.readabilityHandler = nil
        // Terminate before closing a writer that might be blocked on a full pipe.
        if proc?.isRunning == true { proc?.terminate() }
        try? stdin?.close()
    }

    // Callback delivery remains outside the state executor, so callers may
    // reenter start/stop/request. A queued callback from an old child is dropped.
    private func deliver(_ token: UInt64, _ action: @escaping () -> Void) {
        callbackQueue.async { [weak self] in
            guard let self, self.stateQueue.sync(execute: { self.policy.generation == token }) else { return }
            action()
        }
    }

    // stateQueue only. Installing identity before run() lets a fast bootstrap
    // handshake queue safely while launch is completing.
    private func launch() {
        let token = policy.begin()
        lineBuffer.removeAll(keepingCapacity: true)
        deliver(token) { [weak self] in self?.onStatus?(.launching) }
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: corePath)
        var env = ProcessInfo.processInfo.environment
        for (key, value) in environmentExtras { env[key] = value }
        proc.environment = env
        let stdinPipe = Pipe(), stdoutPipe = Pipe(), stderrPipe = Pipe()
        proc.standardInput = stdinPipe
        proc.standardOutput = stdoutPipe
        proc.standardError = stderrPipe
        process = proc
        stdinHandle = stdinPipe.fileHandleForWriting

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            // Backpressure stays at the pipe reader instead of accumulating an
            // unbounded queue of full frame/snapshot chunks behind state work.
            self?.stateQueue.sync { self?.consumeStdout(data, token: token) }
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            guard let text = String(data: handle.availableData, encoding: .utf8), !text.isEmpty else { return }
            self?.stateQueue.sync {
                guard let self, self.policy.isCurrent(token) else { return }
                self.deliver(token) { [weak self] in
                    for line in text.split(separator: "\n") { self?.onStderrLine?(String(line)) }
                }
            }
        }
        proc.terminationHandler = { [weak self] finished in
            let code = finished.terminationStatus
            self?.stateQueue.async { [weak self] in self?.childExited(token: token, code: code) }
        }
        do { try proc.run() }
        catch {
            let failureToken = policy.invalidate(stopped: true)
            let retired = detachChild()
            callbackQueue.async { Self.retire(retired.0, stdin: retired.1) }
            deliver(failureToken) { [weak self] in self?.onStatus?(.failed("core launch failed: \(error.localizedDescription)")) }
        }
    }

    // stateQueue only. The recovery token invalidates an already-scheduled
    // relaunch when stop(), an explicit new start, or another exit intervenes.
    private func childExited(token: UInt64, code: Int32) {
        guard policy.isCurrent(token) else { return }
        let recoveryToken = policy.invalidate(stopped: false)
        let retired = detachChild()
        failAllPending(BridgeError.processExited)
        callbackQueue.async { Self.retire(retired.0, stdin: retired.1) }
        deliver(recoveryToken) { [weak self] in self?.onStatus?(.exited(code: code)) }
        relaunchAttempts += 1
        let delay = min(30.0, pow(2.0, Double(relaunchAttempts)))
        stateQueue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, self.policy.canRelaunch(recoveryToken) else { return }
            self.launch()
        }
    }

    private func consumeStdout(_ data: Data, token: UInt64) {
        guard policy.isCurrent(token), !data.isEmpty else { return }
        lineBuffer.append(contentsOf: data)
        for object in Self.drainCompleteLines(&lineBuffer) {
            guard policy.isCurrent(token) else { return }
            dispatch(object, token: token)
        }
    }

    /// Parse complete newline-terminated objects, preserving only this child's
    /// partial tail. Plain byte-array indices remain valid after removal.
    static func drainCompleteLines(_ buffer: inout [UInt8]) -> [JSONObject] {
        var objects: [JSONObject] = []
        var start = 0
        var index = 0
        while index < buffer.count {
            if buffer[index] == 0x0A {
                if index > start {
                    let lineData = Data(buffer[start..<index])
                    if let object = (try? JSONSerialization.jsonObject(with: lineData)) as? JSONObject {
                        objects.append(object)
                    }
                }
                start = index + 1
            }
            index += 1
        }
        buffer.removeFirst(start)
        return objects
    }

    // stateQueue only.
    private func dispatch(_ object: JSONObject, token: UInt64) {
        guard policy.isCurrent(token) else { return }
        if let id = object["id"] as? String {
            if id == "handshake" {
                let compatibleVersion = object["protocolVersion"].map {
                    ($0 as? JSONObject).map(validateProtocolVersion) ?? false
                } ?? true
                guard object["ok"] as? Bool == true, compatibleVersion else {
                    let failureToken = policy.invalidate(stopped: true)
                    let retired = detachChild()
                    let message = "Media core protocol is incompatible. Install matching shell and core versions."
                    failAllPending(BridgeError.remote(message))
                    callbackQueue.async { Self.retire(retired.0, stdin: retired.1) }
                    deliver(failureToken) { [weak self] in self?.onStatus?(.failed(message)) }
                    return
                }
                guard policy.acceptHandshake(token) else { return }
                relaunchAttempts = 0
                let renderer = ((object["profile"] as? JSONObject)?["renderer"] as? String) ?? "unknown"
                deliver(token) { [weak self] in self?.onStatus?(.connected(renderer: renderer)) }
                return
            }
            if let box = pending.removeValue(forKey: id) {
                let result: Result<JSONObject, Error>
                if object["ok"] as? Bool == false {
                    let message = (object["error"] as? JSONObject)?["message"] as? String
                    result = .failure(BridgeError.remote(message ?? "request failed"))
                } else { result = .success(object) }
                callbackQueue.async { [weak self] in
                    guard let self else { box.finish(with: .failure(BridgeError.processExited)); return }
                    let current = self.stateQueue.sync { self.policy.isCurrent(token) }
                    box.finish(with: current ? result : .failure(BridgeError.processExited))
                }
                return
            }
        }
        guard policy.ready else { return }
        deliver(token) { [weak self] in self?.onEvent?(object) }
    }

    // stateQueue only; continuations resume outside it.
    private func failAllPending(_ error: Error) {
        let waiters = Array(pending.values)
        pending.removeAll()
        callbackQueue.async {
            for box in waiters { box.finish(with: .failure(error)) }
        }
    }

    private func failRequest(_ id: String, token: UInt64, error: Error) {
        guard policy.generation == token, let box = pending.removeValue(forKey: id) else { return }
        callbackQueue.async { box.finish(with: .failure(error)) }
    }

    func request(_ body: JSONObject, timeout: TimeInterval = 6.0) async throws -> JSONObject {
        return try await withCheckedThrowingContinuation { continuation in
            let box = CompletionBox(continuation: continuation)
            stateQueue.async { [weak self] in
                guard let self else {
                    box.finish(with: .failure(BridgeError.notRunning))
                    return
                }
                guard self.policy.canWrite(self.policy.generation), let handle = self.stdinHandle else {
                    self.callbackQueue.async { box.finish(with: .failure(BridgeError.notRunning)) }
                    return
                }
                let token = self.policy.generation
                let id = "shell-\(self.nextId)"
                self.nextId += 1
                var payload = body
                payload["id"] = id
                let data: Data
                do { data = try JSONSerialization.data(withJSONObject: payload) }
                catch { self.callbackQueue.async { box.finish(with: .failure(error)) }; return }
                self.pending[id] = box
                self.writeQueue.async { [weak self] in
                    guard let self else { box.finish(with: .failure(BridgeError.notRunning)); return }
                    let writable = self.stateQueue.sync {
                        self.policy.canWrite(token) && self.stdinHandle === handle && self.pending[id] != nil
                    }
                    guard writable else { return } // stop/exit/timeout already owns completion
                    var line = data
                    line.append(0x0A)
                    do { try handle.write(contentsOf: line) }
                    catch {
                        self.stateQueue.async { [weak self] in self?.failRequest(id, token: token, error: error) }
                    }
                }
                self.stateQueue.asyncAfter(deadline: .now() + timeout) { [weak self] in
                    self?.failRequest(id, token: token, error: BridgeError.timeout)
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

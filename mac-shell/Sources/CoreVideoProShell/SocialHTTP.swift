// The HTTP seam the social transports share (YouTube Data API, Twitch Helix
// and OAuth). Windows builds each HttpClient with AllowAutoRedirect=false,
// UseCookies=false, UseProxy=false (owner decision N5: match it) and reads at
// most 1 MiB (GraphicsFeedTransport.ReadBody). The Mac session is ephemeral
// (no cookies, no cache), ignores the system proxy, refuses every redirect so
// a 3xx reaches the caller as the response, and caps the body. Errors carry
// fixed text only: a response body, a URL or a header never becomes an error
// message (a secret could ride in any of them).

import Foundation

struct SocialHTTPRequest {
    var method: String
    var url: URL
    var headers: [String: String] = [:]
    var body: Data?
}

struct SocialHTTPResponse {
    var status: Int
    var headers: [String: String] = [:]
    var body: Data
}

/// Fixed-text failures. `describing` never includes a response, URL or secret.
enum SocialFailure: Error, Equatable, CustomStringConvertible {
    case authorization, ended, retry, invalid

    var description: String {
        switch self {
        case .authorization: return "authorization"
        case .ended: return "ended"
        case .retry: return "retry"
        case .invalid: return "invalid"
        }
    }
}

protocol SocialHTTP: Sendable {
    /// Throws SocialFailure.retry on a transport failure or the deadline, and
    /// .invalid on an oversized body.
    func send(_ request: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse
}

final class URLSessionSocialHTTP: NSObject, SocialHTTP, URLSessionTaskDelegate, @unchecked Sendable {
    static let bodyLimit = 1_048_576

    private lazy var session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieAcceptPolicy = .never
        config.httpShouldSetCookies = false
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        // N5: no system proxy, as Windows' UseProxy=false.
        config.connectionProxyDictionary = [
            kCFNetworkProxiesHTTPEnable as String: false,
            "HTTPSEnable": false,
        ]
        return URLSession(configuration: config, delegate: self, delegateQueue: nil)
    }()

    /// The whole exchange, headers to last byte, inside `deadline` (Windows
    /// CancelAfter): URLRequest.timeoutInterval alone is an idle timeout.
    func send(_ request: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse {
        try await withThrowingTaskGroup(of: SocialHTTPResponse.self) { group in
            group.addTask { try await self.exchange(request, deadline: deadline) }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(max(0, deadline) * 1_000_000_000))
                throw SocialFailure.retry
            }
            defer { group.cancelAll() }
            guard let first = try await group.next() else { throw SocialFailure.retry }
            return first
        }
    }

    private func exchange(_ request: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse {
        var r = URLRequest(url: request.url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: deadline)
        r.httpMethod = request.method
        for (name, value) in request.headers { r.setValue(value, forHTTPHeaderField: name) }
        r.httpBody = request.body
        // Streamed and bounded: an oversized body stops at limit + 1 bytes
        // rather than being buffered whole (GraphicsFeedTransport.ReadBounded).
        var data = Data()
        let response: URLResponse
        do {
            let (bytes, head) = try await session.bytes(for: r)
            response = head
            if head.expectedContentLength > Int64(Self.bodyLimit) { throw SocialFailure.invalid }
            for try await byte in bytes {
                data.append(byte)
                if data.count > Self.bodyLimit { throw SocialFailure.invalid }
            }
        } catch let failure as SocialFailure {
            throw failure
        } catch {
            throw SocialFailure.retry
        }
        guard let http = response as? HTTPURLResponse else { throw SocialFailure.retry }
        var headers: [String: String] = [:]
        for (k, v) in http.allHeaderFields { if let k = k as? String, let v = v as? String { headers[k.lowercased()] = v } }
        return SocialHTTPResponse(status: http.statusCode, headers: headers, body: data)
    }

    // Refuse every redirect: the 3xx itself becomes the response.
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

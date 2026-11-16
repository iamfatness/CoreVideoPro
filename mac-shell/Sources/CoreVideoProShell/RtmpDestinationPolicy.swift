// RtmpDestinationPolicy.swift — a pure port of Windows' RTMP destination rules
// for `transport.stream.rtmp.configure`: StreamDestinationControl.Configure
// (W/Services/StreamDestinationControl.cs) and StudioStreamOutputValidation
// BuildRtmpUrl / ValidateRtmp. No refusal text echoes caller data (the args
// carry credentials); every text is fixed.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 11).

import Foundation

enum RtmpDestinationPolicy {
    private static let protocols: Set<String> = ["rtmp", "rtmps"]

    /// Windows BuildRtmpUrl: a bare host/path gets the (normalized) protocol prefix.
    static func buildUrl(protocol: String, serverUrl: String) -> String {
        let url = serverUrl.trimmingCharacters(in: .whitespacesAndNewlines)
        if url.isEmpty || url.contains("://") { return url }
        var scheme = `protocol`.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if scheme.isEmpty || !protocols.contains(scheme) { scheme = "rtmps" }
        return "\(scheme)://\(url)"
    }

    /// Windows ValidateRtmp, in Windows' order.
    static func validate(protocol: String, serverUrl: String, streamKey: String) -> String? {
        var scheme = `protocol`.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if scheme.isEmpty { scheme = "rtmps" }
        guard protocols.contains(scheme) else { return "Choose RTMP protocol rtmp or rtmps before streaming." }
        let url = serverUrl.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !url.isEmpty else { return "Configure RTMP server URL before streaming." }
        guard let parts = URLComponents(string: buildUrl(protocol: scheme, serverUrl: url)),
              let urlScheme = parts.scheme?.lowercased(), protocols.contains(urlScheme) else {
            return "RTMP server URL must be a valid rtmp:// or rtmps:// URL."
        }
        guard !(parts.host ?? "").trimmingCharacters(in: .whitespaces).isEmpty else {
            return "RTMP server URL must include a host."
        }
        guard urlScheme == scheme else { return "RTMP protocol selection must match the server URL scheme." }
        guard !parts.path.trimmingCharacters(in: CharacterSet(charactersIn: "/")).isEmpty else {
            return "RTMP server URL must include an application path."
        }
        let key = streamKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { return "Configure RTMP stream key before streaming." }
        guard !key.unicodeScalars.contains(where: { CharacterSet.whitespacesAndNewlines.contains($0) }) else {
            return "RTMP stream key cannot contain whitespace."
        }
        return nil
    }

    struct Decision: Equatable { var url: String; var key: String }

    /// Bound args: enabled Bool, protocol String, serverUrl String, streamKey String? (nil keeps the saved key).
    static func configure(args: [Any?], busy: Bool, currentKey: String) -> Result<Decision, ControlBindError> {
        func refuse(_ text: String) -> Result<Decision, ControlBindError> { .failure(ControlBindError(message: text, status: 422)) }
        func text(_ index: Int) -> String? { index < args.count ? args[index] as? String : nil }
        if busy { return refuse("Stop streaming and wait for the transition to finish before configuring destinations.") }
        let enabled = (args.first ?? nil) as? Bool ?? false
        // D9: the Mac has one always-armed RTMP destination and no enable flag.
        guard enabled else {
            return refuse("macOS streams to one RTMP destination; it cannot be disabled. Send enabled=true to configure it.")
        }
        let scheme = (text(1) ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard protocols.contains(scheme) else { return refuse("RTMP protocol must be rtmp or rtmps.") }
        let serverUrl = (text(2) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let url = buildUrl(protocol: scheme, serverUrl: serverUrl)
        if let parts = URLComponents(string: url), parts.scheme != nil,
           parts.user != nil || parts.password != nil || parts.query != nil || parts.fragment != nil {
            return refuse("Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.")
        }
        let key = text(3).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) } ?? currentKey
        if let error = validate(protocol: scheme, serverUrl: serverUrl, streamKey: key) { return refuse(error) }
        return .success(Decision(url: url, key: key))
    }
}

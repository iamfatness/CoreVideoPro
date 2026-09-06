import Foundation

enum RecordingLifecycleReadModel {
    static func status(_ recording: [String: Any]) -> String {
        guard recording.keys.contains("lifecycle") else {
            return recording["status"] as? String ?? "idle"
        }
        guard let lifecycle = recording["lifecycle"] as? [String: Any],
              validateOutputLifecycle(lifecycle),
              let state = lifecycle["state"] as? String else { return "unknown" }
        if state == "live" {
            let health = lifecycle["health"] as? String
            if health == "healthy" || health == "degraded" { return "recording" }
            return health == "failed" ? "failed" : "unknown"
        }
        if state == "completed", lifecycle["finalized"] as? Bool != true { return "unknown" }
        return state
    }
}

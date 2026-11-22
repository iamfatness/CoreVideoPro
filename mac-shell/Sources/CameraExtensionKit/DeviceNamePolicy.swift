import Foundation

public enum DeviceNamePolicy {
    public static let maxScalars = 64

    public static func sanitized(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        let kept = trimmed.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) }
        var scalars = String.UnicodeScalarView()
        scalars.append(contentsOf: kept.prefix(maxScalars))
        // Dropping controls or truncating can expose edge whitespace again.
        let result = String(scalars).trimmingCharacters(in: .whitespacesAndNewlines)
        return result.isEmpty ? CameraConstants.defaultDeviceName : result
    }
}

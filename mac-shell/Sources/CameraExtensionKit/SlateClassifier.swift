/// Recognises the standby slate from a few Y-plane samples, so the shell can
/// tell whether the camera is serving slate or program.
public enum SlateClassifier {
    public static func isSlate(yPlane: UnsafeBufferPointer<UInt8>, width: Int, height: Int,
                               bytesPerRow: Int) -> Bool {
        guard width >= 4, height >= 4, bytesPerRow >= width,
              yPlane.count >= (height - 1) * bytesPerRow + width else { return false }
        let cols = [width / 4, width / 2, 3 * width / 4]
        func rowMatches(_ row: Int, _ expected: Int) -> Bool {
            cols.allSatisfy { abs(Int(yPlane[row * bytesPerRow + $0]) - expected) <= 2 }
        }
        return rowMatches(height / 4, 24)
            && rowMatches(height / 2, 130)
            && rowMatches(3 * height / 4, 24)
    }
}

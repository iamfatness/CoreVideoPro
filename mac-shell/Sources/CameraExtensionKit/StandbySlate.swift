/// Standby slate, byte-identical to C++ `fillVirtualCameraStandbySlate`
/// (native/src/modules/VirtualCameraSlate.h): Y 24 field, centered Y 130 bar of
/// height max(h/12, 1), UV 128.
public enum StandbySlate {
    public static func nv12(width: Int, height: Int) -> [UInt8]? {
        guard width > 0, height > 0, width % 2 == 0, height % 2 == 0 else { return nil }
        let barHeight = max(height / 12, 1)
        let barTop = (height - barHeight) / 2
        let barBottom = barTop + barHeight
        var out = [UInt8](repeating: 24, count: width * height)
        for row in barTop..<barBottom {
            let start = row * width
            for col in 0..<width { out[start + col] = 130 }
        }
        out.append(contentsOf: [UInt8](repeating: 128, count: width * (height / 2)))
        return out
    }
}

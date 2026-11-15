public enum FrameChoice: Equatable {
    case program
    case slate
}

/// Decides between the program frame and the standby slate. The camera must
/// always serve something (law 2): fail toward the slate.
public struct FrameSelector {
    private var lastDelivery: UInt64?

    public init() {}

    public mutating func producerDelivered(atNanoseconds now: UInt64) {
        lastDelivery = now
    }

    /// An explicit stop is slate immediately, with no hold.
    public mutating func producerStopped() {
        lastDelivery = nil
    }

    public func choice(atNanoseconds now: UInt64) -> FrameChoice {
        guard let last = lastDelivery, now >= last else { return .slate }
        return now - last > CameraConstants.holdNanoseconds ? .slate : .program
    }
}

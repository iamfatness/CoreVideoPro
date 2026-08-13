// Design system per docs/design-handoff-macos.md — the platform-neutral spec
// of the Windows broadcast-console look. Non-negotiables honored here:
// dark-only opaque hexes, bundled Space Grotesk / IBM Plex Mono (SF Pro
// fallback is a BUG), no stock control styling, load-bearing status colors.

import AppKit
import CoreText
import SwiftUI

// Color tokens — exact hexes from docs/design-handoff-macos.md (dark only,
// opaque, status colors load-bearing and never adjusted).
enum Studio {
    static let background = Color(hex: 0x0A0B0C)
    static let panel = Color(hex: 0x101315)
    static let surface = Color(hex: 0x16191B)
    static let surfaceRaised = Color(hex: 0x1B1F22)
    static let field = Color(hex: 0x0E1112)
    static let border = Color.white.opacity(0.09)
    static let line2 = Color.white.opacity(0.05)
    static let textPrimary = Color(hex: 0xE9EDEF)
    static let secondary = Color(hex: 0x8B949B)
    static let textDim = Color(hex: 0x5C656B)
    static let accent = Color(hex: 0x22C86E)
    static let onAccent = Color(hex: 0x06170D)
    static let amber = Color(hex: 0xE8A41F)
    static let red = Color(hex: 0xE5433F)
    // The one documented inconsistency: sliders/toggles render the stock
    // accent on Windows too — match the reference, don't unify on green.
    static let blue = Color(red: 0.29, green: 0.62, blue: 0.85)
    // Back-compat aliases for pre-handoff call sites. card is OPAQUE
    // (the audit flagged the translucent variant as a spec violation).
    static let stroke = border
    static let card = surface
}

enum DesignFonts {
    // Registers the bundled TTFs for this process. The .app carries them in
    // Resources/Fonts (ATSApplicationFontsPath); dev runs from the repo tree
    // register straight from the checkout so bare-exe launches match too.
    static func register() {
        let candidates = [
            (Bundle.main.resourcePath ?? "") + "/Fonts",
            ShellPaths.repoRoot() + "/mac-shell/Resources/Fonts",
        ]
        for dir in candidates {
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir)
            else { continue }
            for name in names where name.hasSuffix(".ttf") {
                let url = URL(fileURLWithPath: dir + "/" + name)
                CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
            }
        }
    }
}

extension Font {
    // Space Grotesk carries its weights in one variable TTF; NSFontManager
    // resolves the weighted face from the family.
    static func grotesk(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .custom("Space Grotesk", size: size).weight(weight)
    }

    static func plexMono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        switch weight {
        case .medium: return .custom("IBM Plex Mono Medium", size: size)
        case .semibold, .bold: return .custom("IBM Plex Mono SemiBold", size: size)
        default: return .custom("IBM Plex Mono", size: size)
        }
    }
}

// Mono label: 10px, 0.16em tracking, UPPERCASE, muted (spec "Typography").
struct MonoLabel: View {
    let text: String
    var dim = false

    init(_ text: String, dim: Bool = false) {
        self.text = text
        self.dim = dim
    }

    var body: some View {
        Text(text.uppercased())
            .font(.plexMono(10))
            .tracking(10 * 0.16)
            .foregroundStyle(dim ? Studio.textDim : Studio.secondary)
    }
}

// Ghost button: transparent fill, 1px 9%-white border, 8px radius (spec
// "Component specs"). The default button everywhere.
struct GhostButtonStyle: ButtonStyle {
    var tint: Color = Studio.textPrimary

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.grotesk(13, .semibold))
            .foregroundStyle(tint)
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(configuration.isPressed ? Studio.surfaceRaised : .clear))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(Studio.border, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 8))
    }
}

// Accent button: #22C86E fill, #06170D text, no border.
struct AccentButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.grotesk(13, .semibold))
            .foregroundStyle(Studio.onAccent)
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(Studio.accent.opacity(configuration.isPressed ? 0.85 : 1)))
            .contentShape(RoundedRectangle(cornerRadius: 8))
    }
}

// Text field: #0E1112 fill, 1px border, 8px radius (replaces .roundedBorder).
struct StudioFieldStyle: TextFieldStyle {
    func _body(configuration: TextField<Self._Label>) -> some View {
        configuration
            .textFieldStyle(.plain)
            .font(.grotesk(12))
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(Studio.border, lineWidth: 1))
    }
}

// The Multiview brand mark — geometry transcribed from multiview-mark.svg
// (120x120 viewBox: frame 14,22 92x76 r15 stroke 6; cross at x=60/y=60
// stroke 5; live tile 65,27 36x28 r6). Tile color is the ONLY state variant.
struct MultiviewMark: View {
    var color: Color = Studio.accent
    var tile: Color = Studio.accent

    var body: some View {
        Canvas { context, size in
            let scale = min(size.width, size.height) / 120
            context.scaleBy(x: scale, y: scale)
            let frame = Path(roundedRect: CGRect(x: 14, y: 22, width: 92, height: 76),
                             cornerRadius: 15)
            context.stroke(frame, with: .color(color), lineWidth: 6)
            var cross = Path()
            cross.move(to: CGPoint(x: 60, y: 22))
            cross.addLine(to: CGPoint(x: 60, y: 98))
            cross.move(to: CGPoint(x: 14, y: 60))
            cross.addLine(to: CGPoint(x: 106, y: 60))
            context.stroke(cross, with: .color(color), lineWidth: 5)
            let live = Path(roundedRect: CGRect(x: 65, y: 27, width: 36, height: 28),
                            cornerRadius: 6)
            context.fill(live, with: .color(tile))
        }
    }
}

// App-icon treatment: the mark on a 36x36 squircle (r9, padding 6) over the
// near-black gradient #15191B -> #0B0D0E.
struct BrandBadge: View {
    var body: some View {
        RoundedRectangle(cornerRadius: 9)
            .fill(LinearGradient(
                colors: [Color(hex: 0x15191B), Color(hex: 0x0B0D0E)],
                startPoint: .top, endPoint: UnitPoint(x: 0.35, y: 1)))
            .frame(width: 36, height: 36)
            .overlay(MultiviewMark().padding(6))
            .overlay(RoundedRectangle(cornerRadius: 9)
                .stroke(Studio.border, lineWidth: 1))
    }
}

extension Color {
    init(hex: UInt32, alpha: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xff) / 255,
                  green: Double((hex >> 8) & 0xff) / 255,
                  blue: Double(hex & 0xff) / 255,
                  opacity: alpha)
    }
}

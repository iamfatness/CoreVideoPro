import AppKit
import CoreGraphics

// App icon = the Multiview brand mark on the near-black gradient squircle
// (design-handoff-macos.md "App-icon treatment"), rendered at every size the
// .icns needs. Geometry is the 120x120 viewBox from multiview-mark.svg.
func drawIcon(size: CGFloat) -> NSImage {
    let image = NSImage(size: NSSize(width: size, height: size))
    image.lockFocus()
    guard let context = NSGraphicsContext.current?.cgContext else {
        image.unlockFocus(); return image
    }
    let rect = CGRect(x: 0, y: 0, width: size, height: size)
    // Squircle: radius 9/36 of the icon, per the spec's 36x36 r9.
    let path = CGPath(roundedRect: rect.insetBy(dx: size * 0.06, dy: size * 0.06),
                      cornerWidth: size * 0.22, cornerHeight: size * 0.22,
                      transform: nil)
    context.saveGState()
    context.addPath(path)
    context.clip()
    let space = CGColorSpaceCreateDeviceRGB()
    let gradient = CGGradient(colorsSpace: space, colors: [
        CGColor(colorSpace: space, components: [0x15/255.0, 0x19/255.0, 0x1B/255.0, 1])!,
        CGColor(colorSpace: space, components: [0x0B/255.0, 0x0D/255.0, 0x0E/255.0, 1])!,
    ] as CFArray, locations: [0, 1])!
    context.drawLinearGradient(gradient, start: CGPoint(x: 0, y: size),
                               end: CGPoint(x: size * 0.35, y: 0), options: [])
    context.restoreGState()

    // Mark, scaled from the 120-unit viewBox with the spec's 6-unit padding.
    let scale = size / 120.0
    context.saveGState()
    context.translateBy(x: 0, y: size)
    context.scaleBy(x: scale, y: -scale)
    let green = CGColor(colorSpace: space, components: [0x22/255.0, 0xC8/255.0, 0x6E/255.0, 1])!
    context.setStrokeColor(green)
    context.setFillColor(green)
    let frame = CGPath(roundedRect: CGRect(x: 14, y: 22, width: 92, height: 76),
                       cornerWidth: 15, cornerHeight: 15, transform: nil)
    context.setLineWidth(6)
    context.addPath(frame)
    context.strokePath()
    context.setLineWidth(5)
    context.move(to: CGPoint(x: 60, y: 22)); context.addLine(to: CGPoint(x: 60, y: 98))
    context.move(to: CGPoint(x: 14, y: 60)); context.addLine(to: CGPoint(x: 106, y: 60))
    context.strokePath()
    context.addPath(CGPath(roundedRect: CGRect(x: 65, y: 27, width: 36, height: 28),
                           cornerWidth: 6, cornerHeight: 6, transform: nil))
    context.fillPath()
    context.restoreGState()
    image.unlockFocus()
    return image
}

let outDir = CommandLine.arguments[1]
for (name, size) in [("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32),
                     ("icon_32x32@2x", 64), ("icon_128x128", 128), ("icon_128x128@2x", 256),
                     ("icon_256x256", 256), ("icon_256x256@2x", 512),
                     ("icon_512x512", 512), ("icon_512x512@2x", 1024)] {
    let image = drawIcon(size: CGFloat(size))
    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { continue }
    try! png.write(to: URL(fileURLWithPath: "\(outDir)/\(name).png"))
}
print("icon set written")

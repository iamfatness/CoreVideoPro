// SourceSheet — Sources row thumbnails from the core's source sheet (sources Phase 2 plan,
// Task 10; spec docs/superpowers/specs/2026-11-15-mac-sources-rebuild-design.md §4.5).
//
// The core renders ONE 5x2 IOSurface while the Sources tab holds the preview-demand lease;
// cell N is Show Input slot N+1, always (native/src/core/SourceSheetPlan.h). A row shows its
// cell only when the core says that cell holds the row's own source (Windows PatchPreview,
// MultiviewInputRowsProjection.cs:264-300): a reassigned row blanks at once and waits for a
// snapshot naming the new identity, so it can never show the previous source's picture.

import AppKit
import QuartzCore
import SwiftUI

struct SheetCell: Equatable {
    let slot: Int          // 0-based, the core's numbering
    let sourceId: String   // "" = unassigned
    let state: String      // live | held | empty
}

/// The `sourceSheet` snapshot node. nil = no usable sheet (node absent on an older core,
/// demand not active, or no exported surface): the rows say "Preview unavailable".
struct SourceSheetState: Equatable {
    let surfaceId: UInt32
    let columns: Int
    let rows: Int
    let cells: [Int: SheetCell]   // keyed by 0-based slot

    static func parse(_ snapshot: JSONObject) -> SourceSheetState? {
        guard let node = snapshot["sourceSheet"] as? JSONObject,
              (node["active"] as? Bool) == true,
              let texture = node["texture"] as? JSONObject,
              let id = (texture["iosurfaceId"] as? NSNumber)?.uint32Value, id != 0,
              let columns = (node["columns"] as? NSNumber)?.intValue, columns > 0,
              let rows = (node["rows"] as? NSNumber)?.intValue, rows > 0 else { return nil }
        var cells: [Int: SheetCell] = [:]
        for cell in node["cells"] as? [JSONObject] ?? [] {
            guard let slot = (cell["slot"] as? NSNumber)?.intValue else { continue }
            cells[slot] = SheetCell(slot: slot, sourceId: cell["sourceId"] as? String ?? "",
                                    state: cell["state"] as? String ?? "empty")
        }
        return SourceSheetState(surfaceId: id, columns: columns, rows: rows, cells: cells)
    }
}

enum SourceThumbnailPolicy {
    enum Result: Equatable {
        /// Show the sheet: surface + the cell's column/row in the grid.
        case cell(surfaceId: UInt32, column: Int, row: Int, columns: Int, rows: Int)
        case placeholder(String)
    }

    static let notInMultiview = "Not in the multiview"
    static let videoOff = "Video off"
    static let noSignal = "No signal"
    static let waiting = "Waiting for video"
    static let unavailable = "Preview unavailable"

    static func resolve(slot: ShowInputSlot, facts: SourceRowFacts, sheet: SourceSheetState?) -> Result {
        guard let key = slot.sourceKey, slot.inShow, !slot.offline else { return .placeholder(notInMultiview) }
        if facts.signal == "VIDEO OFF" { return .placeholder(videoOff) }
        // Windows eligibility (PatchPreview :270-275): media always; capture only LIVE;
        // Zoom unless VIDEO OFF / NO INCOMING / IDLE.
        switch slot.kind {
        case "media": break
        case "capture": if facts.signal != "LIVE" { return .placeholder(noSignal) }
        default: if ["NO INCOMING", "IDLE"].contains(facts.signal) { return .placeholder(noSignal) }
        }
        guard let sheet else { return .placeholder(unavailable) }
        let index = slot.id - 1
        guard index >= 0, index < sheet.columns * sheet.rows,
              let cell = sheet.cells[index], cell.sourceId == key,
              cell.state == "live" || cell.state == "held" else { return .placeholder(waiting) }
        return .cell(surfaceId: sheet.surfaceId, column: index % sheet.columns, row: index / sheet.columns,
                     columns: sheet.columns, rows: sheet.rows)
    }

    /// Windows MultiviewInputRow.PreviewBorderBrush (:45-51), on Studio tokens.
    static func borderColor(tally: String, signal: String) -> Color {
        if signal == "VIDEO OFF" { return Studio.border }
        switch tally {
        case "pgm", "hold": return Studio.amber
        case "pvw": return Studio.accent
        case "talking": return Studio.textPrimary
        default: return Studio.border
        }
    }

    /// The full-sheet sublayer frame inside a cell-sized, geometry-FLIPPED container
    /// (y grows downward), so exactly that cell is visible. Contents keep their orientation
    /// under geometryFlipped (CALayer docs), as Program does in SurfaceView.
    static func sheetFrame(column: Int, row: Int, columns: Int, rows: Int, cell: CGSize) -> CGRect {
        CGRect(x: -CGFloat(column) * cell.width, y: -CGFloat(row) * cell.height,
               width: CGFloat(columns) * cell.width, height: CGFloat(rows) * cell.height)
    }
}

/// `set-source-preview-demand` is a 3 s lease in the core (core/SourcePreviewDemand.h):
/// while the Sources tab is visible the shell renews it every second, releases it once on
/// leave, and re-asserts it on every core generation (a respawned core holds no lease).
@MainActor
final class SourcePreviewDemandLease {
    private let send: (Bool) -> Void
    private(set) var visible = false

    init(send: @escaping (Bool) -> Void) { self.send = send }

    func tabVisible(_ isVisible: Bool) {
        guard isVisible != visible else { return }
        visible = isVisible
        send(isVisible)
    }

    /// Called once a second by the owner's timer.
    func tick() { if visible { send(true) } }

    func coreConnected() { if visible { send(true) } }
}

/// One 15 Hz timer re-marks every live thumbnail layer (the spec's rule: not one 60 Hz
/// timer per view, the SurfaceView pattern).
@MainActor
final class SourceThumbnailTicker {
    static let shared = SourceThumbnailTicker()
    private let views = NSHashTable<SourceThumbnailNSView>.weakObjects()
    private var timer: Timer?

    func add(_ view: SourceThumbnailNSView) {
        views.add(view)
        if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 15.0, repeats: true) { _ in
                MainActor.assumeIsolated { SourceThumbnailTicker.shared.fire() }
            }
        }
    }

    private func fire() {
        let live = views.allObjects
        if live.isEmpty { timer?.invalidate(); timer = nil; return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for view in live { view.remark() }
        CATransaction.commit()
    }
}

final class SourceThumbnailNSView: NSView {
    private let sheetLayer = CALayer()
    private var surfaceId: UInt32 = 0
    private var surface: IOSurfaceRef?
    private var grid = (column: 0, row: 0, columns: 1, rows: 1)

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.isGeometryFlipped = true
        layer?.backgroundColor = NSColor.black.cgColor
        sheetLayer.contentsGravity = .resize   // the sheet is exactly 5x2 cells of 16:9
        layer?.addSublayer(sheetLayer)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    func show(surfaceId: UInt32, column: Int, row: Int, columns: Int, rows: Int) {
        grid = (column, row, columns, rows)
        if surfaceId != self.surfaceId {
            self.surfaceId = surfaceId
            surface = IOSurfaceLookup(surfaceId)
            if surface == nil { ShellLog.write("source sheet: IOSurfaceLookup FAILED for id \(surfaceId)") }
        }
        layout()
        MainActor.assumeIsolated { SourceThumbnailTicker.shared.add(self) }
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sheetLayer.frame = SourceThumbnailPolicy.sheetFrame(column: grid.column, row: grid.row,
                                                           columns: grid.columns, rows: grid.rows,
                                                           cell: bounds.size)
        CATransaction.commit()
    }

    /// Same contents object re-assigned is a CA no-op; detach + re-attach re-textures from
    /// the live IOSurface, zero-copy (SurfaceNSView's reason).
    func remark() {
        guard let surface else { return }
        sheetLayer.contents = nil
        sheetLayer.contents = surface
    }
}

struct SourceSheetCellView: NSViewRepresentable {
    let surfaceId: UInt32
    let column: Int, row: Int, columns: Int, rows: Int

    func makeNSView(context: Context) -> SourceThumbnailNSView { SourceThumbnailNSView() }

    func updateNSView(_ view: SourceThumbnailNSView, context: Context) {
        view.show(surfaceId: surfaceId, column: column, row: row, columns: columns, rows: rows)
    }
}

/// A row (96x54) or inspector (320x180) thumbnail: the slot's sheet cell or the reason it
/// has none, inside the tally border.
struct SourceThumbnail: View {
    @EnvironmentObject var model: AppModel
    let slot: ShowInputSlot
    @ObservedObject var box: SlotFactsBox
    var width: CGFloat = 96
    var height: CGFloat = 54

    var body: some View {
        let result = SourceThumbnailPolicy.resolve(slot: slot, facts: box.facts, sheet: model.sourceSheet)
        let corner: CGFloat = width > 200 ? 8 : 6
        ZStack {
            RoundedRectangle(cornerRadius: corner).fill(Studio.field)
            switch result {
            case let .cell(surfaceId, column, row, columns, rows):
                SourceSheetCellView(surfaceId: surfaceId, column: column, row: row, columns: columns, rows: rows)
                    .clipShape(RoundedRectangle(cornerRadius: corner))
            case let .placeholder(text):
                Text(text).font(.grotesk(width > 200 ? 11 : 9)).foregroundStyle(Studio.textDim)
                    .multilineTextAlignment(.center).padding(4)
            }
        }
        .frame(width: width, height: height)
        .overlay(RoundedRectangle(cornerRadius: corner)
            .stroke(SourceThumbnailPolicy.borderColor(tally: box.facts.tally, signal: box.facts.signal),
                    lineWidth: 2))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Self.accessibility(slot: slot.id, result: result))
    }

    static func accessibility(slot: Int, result: SourceThumbnailPolicy.Result) -> String {
        if case let .placeholder(text) = result { return "Input \(slot) preview: \(text)" }
        return "Input \(slot) preview"
    }
}

// Studio scene editing (Tier 0.14; Windows StudioWorkspace.xaml:446-468,
// 536-570 POS-1/POS-2, the scene rail's per-row menu :343-364, and
// ShowMultiviewHost's tile drag-swap).
//
// The Mac's "Edit layout" button changed the MULTIVIEW layout (Windows keeps
// that in Production settings, as the Mac Settings tab does), scene
// rename/duplicate/remove worked only on the cued scene in the Scenes tab,
// there was no Add overlay, and multiview tiles could not be dragged. Ported:
//
//  - Edit layout: an in-place editor over the multiview for the PREVIEW scene
//    (the same CanvasEditor the Scenes tab uses — Windows reuses its
//    SceneCanvasEditorControl the same way), with Done. Edits ride Preview;
//    Program changes exclusively on Take (the edit target is
//    sceneEditTargetId: never Program, never a solo cue).
//  - Add overlay: a still from the media bin, a show input or the active
//    speaker, at a corner/center/full-canvas preset inside the 5% safe area,
//    15% wide and aspect-locked (Windows OverlayLayerService), appended as
//    the Preview scene's top-most layer.
//  - Rail rows: Rename scene / Remove scene context menu and a Duplicate
//    button on every row, by stable id, without cueing (#1196 / #1070 rules
//    already on AppModel).
//  - Drag one multiview source tile onto another to swap the two input SLOTS
//    (Windows SwapMultiviewTiles). An operator write: both slots stay
//    assigned, nothing is cleared, so THE LAW's auto-fill has nothing to take.

import AppKit
import SwiftUI

// MARK: - Add overlay placement (Windows OverlayLayerService)

enum OverlayPlacementPolicy {
    static let defaultWidthFraction = 0.15
    static let safeAreaMargin = 0.05
    static let canvasAspect = 16.0 / 9.0
    static let minDimension = 0.08

    /// Menu order and labels, verbatim from Windows PlacementOptions.
    static let options: [(value: String, label: String)] = [
        ("full-canvas", "Full canvas (transparent overlay)"),
        ("top-left", "Top left"),
        ("top-right", "Top right"),
        ("bottom-left", "Bottom left"),
        ("bottom-right", "Bottom right"),
        ("center", "Center"),
        ("free", "Free (drag to place)"),
    ]

    /// Windows PlacementLabel (the short name in the status line).
    static func label(_ placement: String) -> String {
        switch placement {
        case "full-canvas": return "Full canvas"
        case "top-left": return "Top left"
        case "top-right": return "Top right"
        case "bottom-left": return "Bottom left"
        case "bottom-right": return "Bottom right"
        case "center": return "Center"
        default: return "Free"
        }
    }

    /// Windows ComputeOverlayRect: 15% of the canvas width, height from the
    /// source aspect (16:9 when unknown), snapped inside the safe area.
    static func rect(_ placement: String, sourceAspect: Double?) -> (x: Double, y: Double, w: Double, h: Double) {
        if placement == "full-canvas" { return (0, 0, 1, 1) }
        let aspect = sourceAspect.flatMap { $0.isFinite && $0 > 0 ? $0 : nil } ?? canvasAspect
        let margin = safeAreaMargin
        var width = min(max(defaultWidthFraction, minDimension), 1 - 2 * margin)
        var height = width * canvasAspect / aspect
        if height < minDimension {
            height = minDimension
            width = height * aspect / canvasAspect
        }
        let maxDimension = 1 - 2 * margin
        width = min(width, maxDimension)
        height = min(height, maxDimension)
        let x: Double
        switch placement {
        case "top-left", "bottom-left": x = margin
        case "top-right", "bottom-right": x = 1 - margin - width
        default: x = (1 - width) / 2
        }
        let y: Double
        switch placement {
        case "top-left", "top-right": y = margin
        case "bottom-left", "bottom-right": y = 1 - margin - height
        default: y = (1 - height) / 2
        }
        return (x, y, width, height)
    }
}

/// What an overlay shows.
enum OverlaySource: Equatable {
    case media(String)       // media-bin asset id (stills)
    case slot(Int)           // a show input
    case activeSpeaker
}

// MARK: - Multiview drag-swap (Windows ShowMultiviewHost)

enum MultiviewDragPolicy {
    /// The source tile a drag that started on `from` was released over, in
    /// normalized multiview coordinates; nil for the bus cells, empty space
    /// or the tile it started on (a release there is not a swap).
    static func dropTarget(tiles: [MultiviewTile], from: MultiviewTile,
                           at point: CGPoint) -> MultiviewTile? {
        let x = Double(point.x), y = Double(point.y)
        return tiles.first { tile in
            tile.role == "source" && tile.id != from.id
                && x >= tile.x && x <= tile.x + tile.w
                && y >= tile.y && y <= tile.y + tile.h
        }
    }
}

// MARK: - Model

extension AppModel {
    static let noPreviewSceneMessage =
        "No scene on Preview — cue one to edit it. Program is never edited directly."

    /// Windows EditLayoutToggle: opens the in-place editor on the PREVIEW scene.
    func toggleStudioLayoutEditing() {
        if studioLayoutEditing {
            studioLayoutEditing = false
            return
        }
        guard let target = sceneEditTargetId else {
            studioCommandStatus = Self.noPreviewSceneMessage
            return
        }
        if refuseLayerEditOnTiles(target) { return }
        // Seed the editable layers from the preset the scene already shows,
        // so opening the editor never changes what is on screen.
        ensureLayers(for: target)
        studioLayoutEditing = true
    }

    /// Stills only: an overlay is composited as a held picture, and the Mac
    /// has no per-layer clip transport yet.
    var overlayMediaAssets: [MediaAsset] { mediaAssets.filter(\.isStillImage) }

    /// Windows AddOverlayLayer. Returns the refusal, or nil when added.
    @discardableResult
    func addOverlayLayer(placement: String, source: OverlaySource) -> String? {
        guard let sceneId = sceneEditTargetId,
              let index = scenes.firstIndex(where: { $0.id == sceneId }) else {
            studioCommandStatus = Self.noPreviewSceneMessage
            return Self.noPreviewSceneMessage
        }
        if refuseLayerEditOnTiles(sceneId) { return Self.tilesLayersRefusal }
        var aspect: Double?
        var slotId: Int?
        var mediaAssetId: String?
        let label: String
        switch source {
        case .media(let assetId):
            guard let asset = mediaAssets.first(where: { $0.id == assetId }) else {
                let refusal = "Add overlay failed: that media asset no longer exists - re-import it on the Media tab"
                studioCommandStatus = refusal
                return refusal
            }
            if asset.naturalWidth > 0, asset.naturalHeight > 0 {
                aspect = Double(asset.naturalWidth) / Double(asset.naturalHeight)
            }
            mediaAssetId = asset.id
            label = asset.name
        case .slot(let id):
            slotId = id
            let slot = slots.first { $0.id == id }
            label = (slot?.name.isEmpty ?? true) ? "Input \(id)" : slot!.name
        case .activeSpeaker:
            label = "Active speaker"
        }
        ensureLayers(for: sceneId)
        let rect = OverlayPlacementPolicy.rect(placement, sourceAspect: aspect)
        let layer = SceneLayer(
            id: "\(sceneId)-overlay-\(scenes[index].layers.count)-\(UUID().uuidString.prefix(8))",
            slotId: slotId, x: rect.x, y: rect.y, width: rect.w, height: rect.h,
            fitMode: mediaAssetId == nil ? "fill" : "fit", opacity: 1, mediaAssetId: mediaAssetId)
        scenes[index].layers.append(layer)   // last = top-most
        ShellLog.write("overlay: added scene=\(sceneId) source=\(label) placement=\(placement)")
        studioCommandStatus = "\(label) overlay added to \(scenes[index].name) "
            + "(\(OverlayPlacementPolicy.label(placement))) - drag or use Edit layout to fine-tune"
        syncScenes()
        return nil
    }

    /// Rail "Remove scene" is offered only where the #1070 rules allow it
    /// (Windows RemoveButtonVisibility).
    func railCanRemoveScene(_ sceneId: String) -> Bool { sceneRemovalRefusal(sceneId) == nil }

    /// Windows SwapMultiviewTiles: swap two input SLOT assignments (the wall
    /// order is the slot order). Each source keeps its own settings — name,
    /// microphone pairing, key, dropout policy, ISO — they follow the source,
    /// not the slot (Tier 0.8). Routes bound to a slot number re-sync, as on
    /// Windows.
    func swapMultiviewSlots(_ from: Int, _ to: Int) {
        guard from != to,
              let a = slots.firstIndex(where: { $0.id == from }),
              let b = slots.firstIndex(where: { $0.id == to }) else { return }
        var movedA = slots[a]
        var movedB = slots[b]
        movedA.id = to
        movedB.id = from
        var swapped = slots
        swapped[a] = movedB
        swapped[b] = movedA
        ShowInputWriteScope.run("drag-swap") { slots = swapped }
        let now = slots
        func describe(_ index: Int) -> String {
            "slot\(now[index].id)=\(now[index].kind)/\(now[index].sourceId)"
        }
        ShellLog.write("mv-drag: swapped slot\(from)<->slot\(to) now \(describe(a)) \(describe(b)) by=operator-swap")
        studioCommandStatus = "Inputs \(from) and \(to) swapped on the multiviewer."
        recomputeFromSlots()
    }
}

// MARK: - Views

/// The POS-1 overlay: the Scenes tab's canvas editor, in place over the
/// multiview, editing the PREVIEW scene.
struct StudioLayoutEditorOverlay: View {
    @EnvironmentObject var model: AppModel
    @State private var selectedLayerId: String?

    var body: some View {
        ZStack {
            Rectangle().fill(Studio.background.opacity(0.92))
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .top, spacing: 8) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Edit preview layout").font(.grotesk(13, .semibold))
                        // Honest about the Mac editor: no snapping, Shift aspect
                        // lock or arrow nudge yet (Windows has all three).
                        Text("Drag to move · corner grip resizes. Rides the PREVIEW until you Take.")
                            .font(.grotesk(11)).foregroundStyle(Studio.textDim)
                    }
                    Spacer()
                    Button("Done") { model.studioLayoutEditing = false }
                        .buttonStyle(GhostButtonStyle())
                }
                if let target = model.sceneEditTargetId {
                    CanvasEditor(sceneId: target, selectedLayerId: $selectedLayerId)
                        .onAppear { model.ensureLayers(for: target) }
                        .onChange(of: target) { next in
                            selectedLayerId = nil
                            model.ensureLayers(for: next)
                        }
                } else {
                    Text(AppModel.noPreviewSceneMessage)
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .padding(14)
            .frame(maxWidth: 1060)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
            .padding(16)
        }
    }
}

/// The POS-2 "Add overlay" menu: source, then placement.
struct AddOverlayMenu: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Menu {
            let assets = model.overlayMediaAssets
            if assets.isEmpty {
                Button("No media assets - import logos on the Media tab") {}.disabled(true)
            } else {
                ForEach(assets) { asset in
                    placements(asset.name, .media(asset.id))
                }
            }
            Divider()
            Menu("Show inputs") {
                ForEach(model.slots.filter { $0.kind != "unassigned" }) { slot in
                    placements("Input \(slot.id) · \(slot.name)", .slot(slot.id))
                }
            }
            Menu("Live sources") {
                placements("Active speaker", .activeSpeaker)
            }
        } label: {
            HStack(spacing: 4) {
                Image(systemName: "plus.square.on.square").font(.system(size: 9))  // design-lint: allow
                Text("Add overlay").font(.grotesk(11, .semibold))
            }
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .foregroundStyle(Studio.textPrimary)
        .help("Add a small overlay layer (logo/bug) to the preview scene: pick a source, then a corner or center. Fine-tune with Edit layout. Changes ride the preview until you Take.")
    }

    func placements(_ label: String, _ source: OverlaySource) -> some View {
        Menu(label) {
            ForEach(OverlayPlacementPolicy.options, id: \.value) { option in
                Button(option.label) { model.addOverlayLayer(placement: option.value, source: source) }
            }
        }
    }
}

/// Windows SceneNamingDialog (rename): "Rename scene", "Scene name", error
/// kept in the dialog, Rename / Cancel. Identity is captured on open.
struct SceneRenameSheet: View {
    @EnvironmentObject var model: AppModel
    let sceneId: String
    @Binding var presented: String?
    @State private var name = ""
    @State private var error = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Rename scene").font(.grotesk(14, .semibold))
            Text("Scene name").font(.grotesk(11)).foregroundStyle(Studio.secondary)
            TextField("Enter a scene name", text: $name)
                .textFieldStyle(StudioFieldStyle())
                .frame(minWidth: 240)
                .onSubmit(rename)
                .onChange(of: name) { _ in error = "" }
            if !error.isEmpty {
                Text(error).font(.grotesk(11)).foregroundStyle(Studio.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("Cancel") { presented = nil }
                    .buttonStyle(GhostButtonStyle())
                    .keyboardShortcut(.cancelAction)
                Button("Rename", action: rename)
                    .buttonStyle(AccentButtonStyle())
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 320)
        .onAppear { name = model.scenes.first { $0.id == sceneId }?.name ?? "" }
    }

    func rename() {
        if let refusal = model.renameScene(sceneId, to: name) {
            error = refusal
        } else {
            presented = nil
        }
    }
}

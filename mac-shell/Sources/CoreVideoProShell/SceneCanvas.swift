// Scenes tab — the 16:9 canvas editor (docs/sources-redesign-spec.md §B /
// the WinUI SourcesPage scene builder): drag/resize layers on the preview
// canvas, with the layer dock for source binding, numeric rects, fit mode
// and opacity. Edits target the PREVIEW scene draft — program is untouched
// until Take, which is the S2b contract.
//
// The canvas is a normalized 0..1 space; only rect/fit/opacity + the slot
// binding reach the wire (SceneRoute.json), so nothing here can invent a
// field the core would ignore.

import SwiftUI

struct ScenesPane: View {
    @EnvironmentObject var model: AppModel
    @State private var selectedLayerId: String?

    var sceneId: String {
        model.previewSceneId.isEmpty ? model.programSceneId : model.previewSceneId
    }

    var scene: SceneDef? { model.scenes.first { $0.id == sceneId } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Text("Scene builder").font(.grotesk(15, .semibold))
                Text(scene?.name ?? "No scene")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                MonoChip("1920×1080")
                Spacer()
                Button("Add layer") { model.addLayer(to: sceneId) }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(sceneId.isEmpty)
                Button("Reset to preset") { model.resetLayers(for: sceneId) }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(scene?.layers.isEmpty ?? true)
                Button("Take") { model.take() }
                    .buttonStyle(AccentButtonStyle())
                    .disabled(model.previewSceneId.isEmpty
                              || model.previewSceneId == model.programSceneId)
            }
            Text("Editing the PREVIEW scene — program is untouched until Take. "
                 + "Drag to move, drag the corner to resize.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            HStack(alignment: .top, spacing: 10) {
                CanvasEditor(sceneId: sceneId, selectedLayerId: $selectedLayerId)
                LayerDock(sceneId: sceneId, selectedLayerId: $selectedLayerId)
                    .frame(width: 280)
            }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
        .frame(maxWidth: 1000)
        .onAppear { model.ensureLayers(for: sceneId) }
    }
}

struct CanvasEditor: View {
    @EnvironmentObject var model: AppModel
    let sceneId: String
    @Binding var selectedLayerId: String?
    @State private var dragOrigin: SceneLayer?

    var layers: [SceneLayer] {
        model.scenes.first { $0.id == sceneId }?.layers ?? []
    }

    func label(for layer: SceneLayer) -> String {
        guard let slotId = layer.slotId,
              let slot = model.slots.first(where: { $0.id == slotId }),
              slot.kind != "unassigned" else { return "Active speaker" }
        return slot.name.isEmpty ? "Input \(slotId)" : slot.name
    }

    var body: some View {
        GeometryReader { geometry in
            let size = geometry.size
            ZStack(alignment: .topLeading) {
                Rectangle().fill(Color.black)
                // Safe-area guide (5%, the overlay-placement margin).
                Rectangle()
                    .stroke(Studio.line2, style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
                    .padding(size.width * 0.05)
                ForEach(layers) { layer in
                    let selected = selectedLayerId == layer.id
                    ZStack(alignment: .bottomTrailing) {
                        Rectangle()
                            .fill(Studio.accent.opacity(selected ? 0.16 : 0.08))
                            .overlay(Rectangle().stroke(
                                selected ? Studio.accent : Studio.secondary.opacity(0.7),
                                lineWidth: selected ? 2 : 1))
                            .overlay(
                                Text(label(for: layer))
                                    .font(.grotesk(11, .medium))
                                    .foregroundStyle(Studio.textPrimary)
                                    .padding(4),
                                alignment: .topLeading)
                        // Resize grip.
                        Rectangle()
                            .fill(selected ? Studio.accent : Studio.secondary)
                            .frame(width: 12, height: 12)
                            .gesture(DragGesture()
                                .onChanged { value in
                                    if dragOrigin == nil { dragOrigin = layer }
                                    guard let origin = dragOrigin else { return }
                                    let width = max(0.08, min(1 - origin.x,
                                        origin.width + value.translation.width / size.width))
                                    let height = max(0.08, min(1 - origin.y,
                                        origin.height + value.translation.height / size.height))
                                    model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                                        $0.width = width
                                        $0.height = height
                                    }
                                }
                                .onEnded { _ in dragOrigin = nil })
                    }
                    .frame(width: max(8, layer.width * size.width),
                           height: max(8, layer.height * size.height))
                    .offset(x: layer.x * size.width, y: layer.y * size.height)
                    .opacity(max(0.25, layer.opacity))
                    .onTapGesture { selectedLayerId = layer.id }
                    .gesture(DragGesture()
                        .onChanged { value in
                            if dragOrigin == nil {
                                dragOrigin = layer
                                selectedLayerId = layer.id
                            }
                            guard let origin = dragOrigin else { return }
                            let x = min(max(0, origin.x + value.translation.width / size.width),
                                        1 - layer.width)
                            let y = min(max(0, origin.y + value.translation.height / size.height),
                                        1 - layer.height)
                            model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                                $0.x = x
                                $0.y = y
                            }
                        }
                        .onEnded { _ in dragOrigin = nil })
                }
                if layers.isEmpty {
                    Text("This scene follows its layout preset. "
                         + "Add a layer to take manual control.")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .overlay(Rectangle().stroke(Studio.border, lineWidth: 1))
    }
}

struct LayerDock: View {
    @EnvironmentObject var model: AppModel
    let sceneId: String
    @Binding var selectedLayerId: String?

    var layers: [SceneLayer] {
        model.scenes.first { $0.id == sceneId }?.layers ?? []
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            MonoLabel("Layers")
            if layers.isEmpty {
                Text("No manual layers.").font(.grotesk(12))
                    .foregroundStyle(Studio.secondary)
            }
            ScrollView {
                VStack(spacing: 8) {
                    ForEach(Array(layers.enumerated()), id: \.element.id) { index, layer in
                        LayerCard(sceneId: sceneId, layer: layer, index: index,
                                  selected: selectedLayerId == layer.id) {
                            selectedLayerId = layer.id
                        }
                    }
                }
            }
        }
    }
}

struct LayerCard: View {
    @EnvironmentObject var model: AppModel
    let sceneId: String
    let layer: SceneLayer
    let index: Int
    let selected: Bool
    let select: () -> Void

    var slotLabel: String {
        guard let slotId = layer.slotId,
              let slot = model.slots.first(where: { $0.id == slotId }),
              slot.kind != "unassigned" else { return "Active speaker" }
        return "Input \(slotId) · \(slot.name)"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 4) {
                Text("\(index + 1)").font(.plexMono(10, .semibold))
                    .foregroundStyle(Studio.secondary)
                Menu {
                    Button("Active speaker") {
                        model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                            $0.slotId = nil
                        }
                    }
                    ForEach(model.slots.filter { $0.kind != "unassigned" }) { slot in
                        Button("Input \(slot.id) · \(slot.name)") {
                            model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                                $0.slotId = slot.id
                            }
                        }
                    }
                } label: {
                    Text(slotLabel).font(.grotesk(11)).lineLimit(1)
                }
                .menuStyle(.borderlessButton)
                Spacer()
                Button {
                    model.moveLayer(sceneId: sceneId, layerId: layer.id, forward: false)
                } label: {
                    Image(systemName: "chevron.up").font(.system(size: 9))  // design-lint: allow
                }
                .buttonStyle(.plain).foregroundStyle(Studio.secondary)
                Button {
                    model.moveLayer(sceneId: sceneId, layerId: layer.id, forward: true)
                } label: {
                    Image(systemName: "chevron.down").font(.system(size: 9))  // design-lint: allow
                }
                .buttonStyle(.plain).foregroundStyle(Studio.secondary)
                Button {
                    model.removeLayer(sceneId: sceneId, layerId: layer.id)
                } label: {
                    Image(systemName: "trash").font(.system(size: 9))  // design-lint: allow
                }
                .buttonStyle(.plain).foregroundStyle(Studio.red)
            }
            HStack(spacing: 4) {
                rectField("X", layer.x) { value in
                    model.updateLayer(sceneId: sceneId, layerId: layer.id) { $0.x = value }
                }
                rectField("Y", layer.y) { value in
                    model.updateLayer(sceneId: sceneId, layerId: layer.id) { $0.y = value }
                }
                rectField("W", layer.width) { value in
                    model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                        $0.width = max(0.08, value)
                    }
                }
                rectField("H", layer.height) { value in
                    model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                        $0.height = max(0.08, value)
                    }
                }
            }
            HStack(spacing: 6) {
                Picker("", selection: Binding(
                    get: { layer.fitMode },
                    set: { value in
                        model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                            $0.fitMode = value
                        }
                    })) {
                    Text("Fit").tag("fit")
                    Text("Fill").tag("fill")
                    Text("Stretch").tag("stretch")
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 150)
                Text("\(Int(layer.opacity * 100))%")
                    .font(.plexMono(9)).foregroundStyle(Studio.secondary)
            }
            Slider(value: Binding(
                get: { layer.opacity },
                set: { value in
                    model.updateLayer(sceneId: sceneId, layerId: layer.id) {
                        $0.opacity = value
                    }
                }), in: 0.1...1)
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8)
            .fill(selected ? Studio.surfaceRaised : Studio.surface))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(selected ? Studio.accent.opacity(0.6) : Studio.border, lineWidth: 1))
        .onTapGesture(perform: select)
    }

    func rectField(_ label: String, _ value: Double,
                   _ set: @escaping (Double) -> Void) -> some View {
        VStack(spacing: 1) {
            Text(label).font(.plexMono(8)).foregroundStyle(Studio.textDim)
            TextField("", value: Binding(get: { value }, set: set),
                      format: .number.precision(.fractionLength(2)))
                .textFieldStyle(.plain)
                .font(.plexMono(10))
                .frame(width: 44)
                .padding(.horizontal, 4).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 4).fill(Studio.field))
                .overlay(RoundedRectangle(cornerRadius: 4)
                    .stroke(Studio.border, lineWidth: 1))
        }
    }
}

// VST3 plug-in inserts — the macOS surface for the out-of-process plugin host
// (`corevideo-plugin-host`, docs/vst-host-spec.md + docs/master-vst-round2-spec.md).
//
// ─── WHAT ACTUALLY WORKS ON macOS (verified 2026-08-06, not assumed) ──────────
//
// The host binary builds and RUNS on macOS, but only ONE of its five modes is
// implemented off Windows. Everything else is inside `#ifdef _WIN32`:
//
//   native/plugin-host/plugin-host.cpp  main()
//     --scan             POSIX + Win32   → WORKS on macOS
//     --probe            #ifdef _WIN32   → "probe unsupported on this platform"
//     --process          #ifdef _WIN32   → "process unsupported on this platform"
//     --state-roundtrip  #ifdef _WIN32   → "unsupported on this platform"
//     --serve            #ifdef _WIN32   → "serve unsupported on this platform", exit 3
//
//   native/src/modules/PluginHostClient.h — the resident audio transport (SHM
//   block + req/done/editor/param/state events) is Win32 kernel objects. The
//   non-Win32 branch is a stub: start()/ready()/requestEditor()/setParam()/
//   exchange() all return false and params() returns {}.
//
// Rig proof on this Mac (native/build-metal/corevideo-plugin-host):
//   --serve testinstance → {"cmd":"error","msg":"serve unsupported on this platform"} (exit 3)
//   --probe <bundle>     → {"cmd":"probe-result",...,"pass":false,
//                           "reasons":["probe unsupported on this platform"]}
//   --scan               → enumerated 200+ real bundles (UAD, Waves) once pointed
//                          at /Library/Audio/Plug-Ins/VST3
//
// SO: macOS can ENUMERATE installed VST3 plug-ins. It cannot load, probe,
// process, show an editor, or read parameters from one. This file therefore
// ships a real scan/browse surface and DIMS every control that would need the
// audio transport — it never renders a live-looking insert that does nothing.
// The gating is derived from the core's runtime `pluginHost.serve.running`
// (never a hardcoded platform check), so the day the host gains a macOS
// transport this surface lights up with no change here.
//
// ─── HOUSE RULES HONORED ─────────────────────────────────────────────────────
// • Parameter writes are CANCEL-AND-DEBOUNCE coalesced (the AppModel
//   .applyColorGrade shape). A per-delta push starved the command queue.
// • The HOST is the parameter value authority: a local value is held only for
//   a short quiet window after the operator touches it, then the host's
//   published value wins — so the sliders never fight an open plug-in editor.
// • Honest states only: host down / scan empty / not-verifiable are all said
//   out loud. Nothing is faked and nothing silently no-ops.

import Foundation
import SwiftUI

// ── model ────────────────────────────────────────────────────────────────────

// One scanned bundle. `id` is the bundle path — the core's stable identity
// across scan/probe/load (PluginHostScan.h PluginHostPluginInfo).
struct VstPlugin: Identifiable, Equatable {
    var id = ""
    var name = ""
    var vendor = ""
    var probe = "pending"          // pending|pass|fail — MEANINGLESS off Windows
    var classNames: [String] = []  // audio classes; "vst:<class>" selects one
}

// One host-published parameter (pluginHost.serve.params[], spec A2).
struct VstParam: Identifiable, Equatable {
    var id: UInt32 = 0
    var title = ""
    var units = ""
    var display = ""     // the PLUG-IN's own formatting — preferred over a raw %
    var stepCount = 0
    var normalized = 0.0
}

// The `pluginHost` snapshot node (MediaCore::pluginHostState).
struct VstHostState: Equatable {
    var status = "absent"          // absent|scanning|probing|ready|error
    var serveRunning = false
    var activePlugin = ""
    var lastError = ""
    var editorLastError = ""
    var latencyMs = 0.0
    var paramPluginClass = ""
    var paramTotalCount = 0
    var params: [VstParam] = []
    var respawnAttempts = 0
    var respawnGaveUp = false
}

// Non-published scratch: debounce task + the quiet-window bookkeeping. Kept off
// @Published so gesture-rate bookkeeping never republishes the whole model.
final class VstScratch {
    var paramSyncTask: Task<Void, Never>?
    var pendingParams: [UInt32: Double] = [:]
    var paramEditedAt: [UInt32: Date] = [:]
    var scanKicked = false
}

// How much of the plug-in host is actually usable right now.
enum VstCapability: Equatable {
    case missing      // the host executable was never found — status "absent"
    case scanOnly     // enumeration works; the audio transport does not
    case live         // the resident host is running: inserts/params/editor real
}

// The VST3 search root handed to the host. `scanRoots()` in plugin-host.cpp
// reads COMMONPROGRAMFILES / COMMONPROGRAMFILES(X86) (Windows-only env vars)
// plus ONE override, COREVIDEO_VST3_SCAN_PATH — so without the override a macOS
// scan walks nothing and returns zero plug-ins. The shell supplies the standard
// macOS system root.
//
// KNOWN LIMIT: the core accepts exactly one override path, so the per-user root
// (~/Library/Audio/Plug-Ins/VST3) is NOT scanned unless the operator points
// COREVIDEO_VST3_SCAN_PATH at it. Widening this needs a core change.
enum VstScanRoots {
    static let systemRoot = "/Library/Audio/Plug-Ins/VST3"
    static var userRoot: String { NSHomeDirectory() + "/Library/Audio/Plug-Ins/VST3" }

    // An operator-set value always wins; otherwise the system root, falling
    // back to the per-user root when only that one exists on this machine.
    static var effectiveRoot: String {
        if let override = ProcessInfo.processInfo.environment["COREVIDEO_VST3_SCAN_PATH"],
           !override.isEmpty {
            return override
        }
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: systemRoot, isDirectory: &isDir),
           isDir.boolValue {
            return systemRoot
        }
        return userRoot
    }
}

// ── AppModel: snapshot intake, commands, coalescing ──────────────────────────

extension AppModel {
    // Runtime truth, never a platform constant — see the header note.
    var vstCapability: VstCapability {
        if vstHost.serveRunning { return .live }
        if vstHost.status == "absent" { return .missing }
        return .scanOnly
    }

    var vstInsertsLive: Bool { vstCapability == .live }

    // Probe verdicts are produced by `--probe`, which is Windows-only, so off
    // Windows EVERY plug-in comes back "fail" with reason "probe unsupported on
    // this platform". Rendering that as a broken plug-in would be a lie.
    var vstProbeIsMeaningful: Bool { vstCapability == .live }

    var vstStatusLine: String {
        switch vstCapability {
        case .missing:
            return "Plug-in host not found — build corevideo-plugin-host into "
                + "native/build-metal, then rescan."
        case .scanOnly:
            return "Scan only on macOS. Plug-ins can be listed, but not loaded: the "
                + "isolated host's audio transport, editor and parameter bridge are "
                + "Windows-only, so inserts below stay disabled."
        case .live:
            return "Isolated VST3 host running — inserts process live."
        }
    }

    // pluginHost node → model. Assignments are change-gated: snapshots arrive at
    // ~10Hz and a scanned machine carries hundreds of bundles, so republishing
    // an identical list every tick would rebuild the list view 10x/s.
    func applyPluginHostSnapshot(_ node: JSONObject) {
        var next = VstHostState()
        next.status = node["status"] as? String ?? "absent"
        if let serve = node["serve"] as? JSONObject {
            next.serveRunning = serve["running"] as? Bool ?? false
            next.activePlugin = serve["activePlugin"] as? String ?? ""
            next.lastError = serve["lastError"] as? String ?? ""
            next.editorLastError = serve["editorLastError"] as? String ?? ""
            next.latencyMs = (serve["latencyMs"] as? NSNumber)?.doubleValue ?? 0
            next.paramPluginClass = serve["paramPluginClass"] as? String ?? ""
            next.paramTotalCount = (serve["paramTotalCount"] as? NSNumber)?.intValue ?? 0
            next.params = (serve["params"] as? [JSONObject] ?? []).map { entry in
                VstParam(
                    id: (entry["id"] as? NSNumber)?.uint32Value ?? 0,
                    title: entry["title"] as? String ?? "",
                    units: entry["units"] as? String ?? "",
                    display: entry["display"] as? String ?? "",
                    stepCount: (entry["stepCount"] as? NSNumber)?.intValue ?? 0,
                    normalized: (entry["normalized"] as? NSNumber)?.doubleValue ?? 0)
            }
            if let respawn = serve["respawn"] as? JSONObject {
                next.respawnAttempts = (respawn["attempts"] as? NSNumber)?.intValue ?? 0
                next.respawnGaveUp = respawn["gaveUp"] as? Bool ?? false
            }
        }
        if next != vstHost { vstHost = next }

        let plugins = (node["plugins"] as? [JSONObject] ?? []).map { entry in
            VstPlugin(
                id: entry["id"] as? String ?? "",
                name: entry["name"] as? String ?? "",
                vendor: entry["vendor"] as? String ?? "",
                probe: entry["probe"] as? String ?? "pending",
                classNames: entry["classNames"] as? [String] ?? [])
        }
        if plugins != vstPlugins { vstPlugins = plugins }

        expireVstParamOverrides()
    }

    // HOST IS THE AUTHORITY. A local value survives only ~0.6s past the last
    // touch — long enough to cover the debounce + one snapshot round trip, short
    // enough that an open plug-in editor moving the same parameter always wins.
    private func expireVstParamOverrides() {
        guard !vstParamLocal.isEmpty else { return }
        let now = Date()
        var stale: [UInt32] = []
        for (id, _) in vstParamLocal {
            let touched = vstScratch.paramEditedAt[id] ?? .distantPast
            if now.timeIntervalSince(touched) > 0.6 { stale.append(id) }
        }
        guard !stale.isEmpty else { return }
        for id in stale {
            vstParamLocal.removeValue(forKey: id)
            vstScratch.paramEditedAt.removeValue(forKey: id)
        }
    }

    // The value a slider shows: the operator's in-flight edit while it is warm,
    // otherwise whatever the host published.
    func vstParamValue(_ param: VstParam) -> Double {
        vstParamLocal[param.id] ?? param.normalized
    }

    // The insert name the core resolves against the scan (PluginHostScan.h
    // vstSelectionQueryFromInsertName / resolveVstInsertSelection). A bundle
    // that publishes classes is addressed "<bundle>/<class>" so shell bundles
    // (Waves) disambiguate; otherwise the plug-in name alone is enough.
    static func vstInsertName(plugin: VstPlugin, className: String?) -> String {
        if let className, !className.isEmpty {
            return "vst:\(plugin.name)/\(className)"
        }
        return "vst:\(plugin.name)"
    }

    // Channel insert list actually sent to the core. The VST selection is only
    // appended when the host can really run it — pushing a "vst:" insert at a
    // dead host would be an insert that silently does nothing in the audio path.
    func pluginInsertNames(for stripId: String) -> [String] {
        var names = (channelInserts[stripId] ?? ChannelInserts()).activeNames
        if vstInsertsLive, let selection = vstChannelSelection[stripId], !selection.isEmpty {
            names.append(selection)
        }
        return names
    }

    func setVstChannelSelection(_ stripId: String, _ insertName: String?) {
        if let insertName, !insertName.isEmpty {
            vstChannelSelection[stripId] = insertName
        } else {
            vstChannelSelection.removeValue(forKey: stripId)
        }
        scheduleMixSync()
    }

    // ── commands ─────────────────────────────────────────────────────────────

    // `scan-vst-plugins` is routed as a TOP-LEVEL request (JsonRpcServer.cpp
    // handle(): the open-vst-editor / scan-vst-plugins guard) and returns the
    // fresh snapshot. The core walks the roots on a detached thread.
    func scanVstPlugins() {
        guard let bridge else { return }
        vstScratch.scanKicked = true
        Task { [weak self] in
            do {
                _ = try await bridge.request(["type": "scan-vst-plugins"], timeout: 20)
            } catch {
                await MainActor.run {
                    self?.pushWarning("VST3 scan failed: \(error.localizedDescription)")
                }
            }
        }
    }

    func kickVstScanIfNeeded() {
        guard !vstScratch.scanKicked, bridge != nil else { return }
        scanVstPlugins()
    }

    // Opens the plug-in's own editor window in the isolated host process.
    func openVstEditor(selection: String) {
        guard let bridge, !selection.isEmpty else { return }
        Task { [weak self] in
            do {
                _ = try await bridge.request(
                    ["type": "open-vst-editor", "selection": selection], timeout: 10)
            } catch {
                await MainActor.run {
                    self?.pushWarning("open plug-in controls failed: \(error.localizedDescription)")
                }
            }
        }
    }

    // COALESCED — the load-bearing rule. A slider drag emits a value per frame;
    // one `set-vst-param` per delta is exactly what starved the command queue
    // until every request timed out. Deltas land in a latest-wins map and a
    // single cancel-and-restart task flushes it 60ms after the drag settles, so
    // a full drag costs ONE request per parameter touched, not one per frame.
    func setVstParam(selection: String, param: VstParam, normalized: Double) {
        let clamped = max(0, min(1, normalized))
        vstParamLocal[param.id] = clamped
        vstScratch.paramEditedAt[param.id] = Date()
        vstScratch.pendingParams[param.id] = clamped

        vstScratch.paramSyncTask?.cancel()
        vstScratch.paramSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 60_000_000)
            guard !Task.isCancelled else { return }
            await self?.flushVstParams(selection: selection)
        }
    }

    @MainActor
    private func flushVstParams(selection: String) async {
        guard let bridge else { return }
        let pending = vstScratch.pendingParams
        vstScratch.pendingParams.removeAll()
        guard !pending.isEmpty else { return }
        for (id, value) in pending {
            // A cheap ack route core-side (no snapshot rebuild per set).
            _ = try? await bridge.request([
                "type": "set-vst-param",
                "selection": selection,
                "paramId": Double(id),
                "normalized": value,
            ], timeout: 4)
        }
    }
}

// ── views ────────────────────────────────────────────────────────────────────

// The PLUG-INS surface of the audio console: host health, the scanned bundle
// browser, and the active selection's generic parameter sliders.
struct VstInsertsSurface: View {
    @EnvironmentObject var model: AppModel
    @State private var filter = ""

    var matches: [VstPlugin] {
        let needle = filter.trimmingCharacters(in: .whitespaces).lowercased()
        guard !needle.isEmpty else { return model.vstPlugins }
        return model.vstPlugins.filter {
            $0.name.lowercased().contains(needle) || $0.vendor.lowercased().contains(needle)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            statusBanner
            if !model.vstHost.lastError.isEmpty {
                calloutRow(model.vstHost.lastError, tint: Studio.red)
            }
            if !model.vstHost.editorLastError.isEmpty {
                calloutRow(model.vstHost.editorLastError, tint: Studio.amber)
            }
            if model.vstHost.respawnGaveUp {
                calloutRow("Isolated host gave up after \(model.vstHost.respawnAttempts) "
                           + "failed restarts — inserts stay bypassed until reselected.",
                           tint: Studio.red)
            }
            browser
            if !model.vstHost.params.isEmpty {
                VstParamRack()
            }
        }
        .onAppear { model.kickVstScanIfNeeded() }
    }

    var header: some View {
        HStack(spacing: 8) {
            Text("VST3 plug-ins").font(.grotesk(13, .semibold))
            statusChip
            Spacer()
            Text("\(model.vstPlugins.count) found")
                .font(.plexMono(10)).foregroundStyle(Studio.secondary)
            Button(model.vstHost.status == "scanning" || model.vstHost.status == "probing"
                   ? "Scanning…" : "Rescan") {
                model.scanVstPlugins()
            }
            .buttonStyle(GhostButtonStyle())
            .disabled(model.vstHost.status == "scanning" || model.vstHost.status == "probing")
        }
    }

    var statusChip: some View {
        let (label, tint): (String, Color) = {
            switch model.vstCapability {
            case .missing: return ("HOST ABSENT", Studio.red)
            case .scanOnly: return ("SCAN ONLY", Studio.amber)
            case .live: return ("HOST LIVE", Studio.accent)
            }
        }()
        return Text(label)
            .font(.plexMono(9, .semibold))
            .foregroundStyle(tint)
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 4).fill(tint.opacity(0.16)))
    }

    var statusBanner: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(model.vstStatusLine)
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Text("Scanning \(VstScanRoots.effectiveRoot) — one root only; set "
                 + "COREVIDEO_VST3_SCAN_PATH to scan a different folder.")
                .font(.plexMono(9)).foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
            if model.vstInsertsLive, !model.vstHost.activePlugin.isEmpty {
                Text("Active: \(model.vstHost.activePlugin)"
                     + (model.vstHost.latencyMs > 0
                        ? String(format: "  +%.1f ms", model.vstHost.latencyMs) : ""))
                    .font(.plexMono(10)).foregroundStyle(Studio.accent)
            }
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }

    func calloutRow(_ message: String, tint: Color) -> some View {
        Text(message)
            .font(.grotesk(12)).foregroundStyle(tint)
            .fixedSize(horizontal: false, vertical: true)
            .padding(8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(tint.opacity(0.12)))
    }

    @ViewBuilder var browser: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                MonoLabel("Installed")
                TextField("Filter by name or vendor", text: $filter)
                    .textFieldStyle(StudioFieldStyle())
                    .frame(width: 240)
            }
            if model.vstPlugins.isEmpty {
                Text(model.vstHost.status == "scanning" || model.vstHost.status == "probing"
                     ? "Scanning…"
                     : "No VST3 bundles found under \(VstScanRoots.effectiveRoot).")
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim)
                    .padding(.vertical, 6)
            } else if matches.isEmpty {
                Text("No plug-in matches “\(filter)”.")
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim)
                    .padding(.vertical, 6)
            } else {
                ScrollView {
                    LazyVStack(spacing: 3) {
                        ForEach(matches) { plugin in
                            VstPluginRow(plugin: plugin)
                        }
                    }
                }
                .frame(maxHeight: 260)
            }
        }
    }
}

// One scanned bundle. The probe badge is deliberately absent unless the host
// can actually probe — off Windows every verdict is a platform "fail" and
// showing it would flag every working plug-in as broken.
struct VstPluginRow: View {
    @EnvironmentObject var model: AppModel
    let plugin: VstPlugin

    var body: some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 1) {
                Text(plugin.name).font(.grotesk(12, .medium)).lineLimit(1)
                Text(subtitle).font(.plexMono(9)).foregroundStyle(Studio.textDim).lineLimit(1)
            }
            Spacer(minLength: 8)
            if model.vstProbeIsMeaningful {
                Text(plugin.probe.uppercased())
                    .font(.plexMono(9, .semibold))
                    .foregroundStyle(plugin.probe == "pass" ? Studio.accent
                                     : plugin.probe == "fail" ? Studio.red : Studio.textDim)
            } else {
                Text("NOT VERIFIED")
                    .font(.plexMono(9))
                    .foregroundStyle(Studio.textDim)
                    .help("Probing loads the plug-in, which the macOS host cannot do.")
            }
            Button("Open controls") {
                model.openVstEditor(selection: AppModel.vstInsertName(
                    plugin: plugin, className: plugin.classNames.first))
            }
            .buttonStyle(GhostButtonStyle())
            .disabled(!model.vstInsertsLive)
            .help(model.vstInsertsLive
                  ? "Open the plug-in's own editor in the isolated host"
                  : "Needs the isolated VST3 host, which does not run on macOS")
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(Studio.surface))
        .opacity(model.vstInsertsLive ? 1 : 0.75)
    }

    var subtitle: String {
        var parts: [String] = []
        if !plugin.vendor.isEmpty { parts.append(plugin.vendor) }
        if plugin.classNames.count > 1 {
            parts.append("\(plugin.classNames.count) classes")
        } else if let first = plugin.classNames.first, !first.isEmpty {
            parts.append(first)
        }
        return parts.isEmpty ? plugin.id : parts.joined(separator: " · ")
    }
}

// Generic parameter sliders for the host's ACTIVE selection (spec A2: first 64
// controller params). Only reachable when the host is live — the host publishes
// nothing otherwise, so this rack simply never appears on macOS today.
struct VstParamRack: View {
    @EnvironmentObject var model: AppModel

    // Every param slider addresses the active selection by its plug-in class.
    var selection: String { "vst:\(model.vstHost.paramPluginClass)" }

    let columns = [GridItem(.adaptive(minimum: 190), spacing: 10)]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                MonoLabel("Parameters")
                Text(model.vstHost.paramPluginClass)
                    .font(.grotesk(12, .medium))
                Spacer()
                if model.vstHost.paramTotalCount > model.vstHost.params.count {
                    Text("showing \(model.vstHost.params.count) of "
                         + "\(model.vstHost.paramTotalCount)")
                        .font(.plexMono(9)).foregroundStyle(Studio.textDim)
                }
            }
            Text("The plug-in is the value authority — its own editor and these "
                 + "sliders always agree.")
                .font(.plexMono(9)).foregroundStyle(Studio.textDim)
            LazyVGrid(columns: columns, alignment: .leading, spacing: 8) {
                ForEach(model.vstHost.params) { param in
                    VStack(alignment: .leading, spacing: 1) {
                        HStack(spacing: 4) {
                            Text(param.title.isEmpty ? "Param \(param.id)" : param.title)
                                .font(.plexMono(9)).foregroundStyle(Studio.secondary)
                                .lineLimit(1)
                            Spacer()
                            Text(readout(param))
                                .font(.plexMono(9)).foregroundStyle(Studio.textPrimary)
                                .lineLimit(1)
                        }
                        Slider(
                            value: Binding(
                                get: { model.vstParamValue(param) },
                                set: { value in
                                    model.setVstParam(
                                        selection: selection, param: param, normalized: value)
                                }),
                            in: 0...1)
                    }
                }
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }

    // Prefer the plug-in's OWN formatted display; fall back to a normalized
    // percentage only while a local edit is in flight (the host has not
    // republished a display string for the new value yet).
    func readout(_ param: VstParam) -> String {
        if model.vstParamLocal[param.id] == nil, !param.display.isEmpty {
            return param.units.isEmpty ? param.display : "\(param.display) \(param.units)"
        }
        return String(format: "%.0f%%", model.vstParamValue(param) * 100)
    }
}

// The per-channel insert control that replaces the old dead "+ Add processing"
// button in the processing workspace. Disabled — with the reason — whenever the
// host cannot actually run an insert, rather than pretending to assign one.
struct VstChannelInsertControl: View {
    @EnvironmentObject var model: AppModel
    let stripId: String

    var selection: String? { model.vstChannelSelection[stripId] }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel("VST3 insert", dim: !model.vstInsertsLive)
            if model.vstInsertsLive {
                Menu {
                    Button("None") { model.setVstChannelSelection(stripId, nil) }
                    Divider()
                    ForEach(model.vstPlugins.filter { $0.probe != "fail" }) { plugin in
                        pluginMenuItem(plugin)
                    }
                } label: {
                    Text(selectionLabel).font(.grotesk(12)).lineLimit(1)
                }
                .menuStyle(.borderlessButton)
                .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text(selectionLabel)
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim).lineLimit(2)
                Text(model.vstCapability == .missing
                     ? "Plug-in host not found."
                     : "Unavailable on macOS — the isolated VST3 host is Windows-only.")
                    .font(.plexMono(9)).foregroundStyle(Studio.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    @ViewBuilder
    func pluginMenuItem(_ plugin: VstPlugin) -> some View {
        if plugin.classNames.count > 1 {
            Menu(plugin.name) {
                ForEach(plugin.classNames, id: \.self) { className in
                    Button(className) {
                        model.setVstChannelSelection(
                            stripId,
                            AppModel.vstInsertName(plugin: plugin, className: className))
                    }
                }
            }
        } else {
            Button(plugin.name) {
                model.setVstChannelSelection(
                    stripId,
                    AppModel.vstInsertName(plugin: plugin, className: plugin.classNames.first))
            }
        }
    }

    var selectionLabel: String {
        guard let selection, !selection.isEmpty else { return "No plug-in" }
        return String(selection.dropFirst(4))  // strip the "vst:" scheme
    }
}

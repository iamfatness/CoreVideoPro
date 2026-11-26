// Volt rundown (RC6): `media.rundown.load`, `.preview`, `.take` on the Mac.
// A port of W/ViewModels/StudioViewModel.VoltRundown.cs; refusal texts are
// Windows' verbatim unless marked Mac. Plan defaults D9/D10:
//   - a staged clip is `<media root>/Volt Rundown/<key>.mp4` (kind volt-rundown);
//   - `route` names a Mac SceneLayer.id (wire route ids are positional); an
//     eligible layer has opacity above 0 (Mac layers have no visible, locked
//     or graphic flag); an empty route needs exactly one eligible layer;
//   - the Mac has no scene draft, so Windows' draft refusal does not apply.

import Foundation

extension AppModel {
    /// Windows VoltClipKey: `^[a-z0-9-]{1,72}$`.
    static func isVoltClipKey(_ key: String) -> Bool {
        guard (1...72).contains(key.count) else { return false }
        return key.unicodeScalars.allSatisfy { scalar in
            (scalar.value >= 0x61 && scalar.value <= 0x7a) || (scalar.value >= 0x30 && scalar.value <= 0x39) || scalar == "-"
        }
    }

    /// The staged clip for a key, only when the bin holds exactly the expected
    /// file and it exists (Windows VoltRundownClip).
    func voltRundownClip(_ key: String) -> MediaAsset? {
        guard Self.isVoltClipKey(key) else { return nil }
        let relative = MediaBin.voltRundownFolder + "/" + key + ".mp4"
        let expected = MediaBin.root + "/" + relative
        guard let asset = mediaAssets.first(where: { $0.id == MediaBin.assetId(relativePath: relative) }),
              asset.filePath.caseInsensitiveCompare(expected) == .orderedSame,
              FileManager.default.fileExists(atPath: expected) else { return nil }
        return asset
    }

    func loadVoltRundownClip(_ key: String) -> ControlInvokeResult {
        guard Self.isVoltClipKey(key) else { return .fail("Invalid staged clip key.") }
        refreshMediaBin()
        guard let clip = voltRundownClip(key) else {
            return .fail("The staged clip is missing from the local Volt Rundown bin.")
        }
        return .withData(#"{"sourceId":"media:\#(clip.id)"}"#)
    }

    /// The layer ids a scene would carry (its edited layers, else the preset
    /// layout's layer ids ensureLayers would create), without mutating it. Shared
    /// with scene.cueWithRoutes / scene.route.set: a Mac route id IS a layer id (D-C2).
    func sceneLayerCandidates(_ sceneId: String) -> [(id: String, opacity: Double)]? {
        guard let scene = scenes.first(where: { $0.id == sceneId }) else { return nil }
        if scene.tiles != nil { return [] }   // a Tiles wall has no clip route (D5)
        if !scene.layers.isEmpty { return scene.layers.map { ($0.id, $0.opacity) } }
        guard let rects = Self.layoutRects[scene.layout] else { return [] }
        return rects.indices.map { ("\(sceneId)-layer-\($0)", 1.0) }
    }

    /// Windows FindVoltClipRoute: exactly one eligible layer, else nil.
    func voltClipLayerId(scene sceneId: String, route: String) -> String? {
        let eligible = (sceneLayerCandidates(sceneId) ?? [])
            .filter { $0.opacity > 0 && (route.isEmpty || $0.id == route) }
        return eligible.count == 1 ? eligible[0].id : nil
    }

    func previewVoltRundownClip(_ key: String, scene sceneId: String, route: String) -> ControlInvokeResult {
        guard let clip = voltRundownClip(key) else { return .fail("The staged clip is missing. Send it from Volt again.") }
        if sceneId == programSceneId || takeInFlight {
            return .fail("The clip scene must be off Program before cueing another clip.")
        }
        guard !Self.isSoloScene(sceneId), scenes.contains(where: { $0.id == sceneId }),
              let layerId = voltClipLayerId(scene: sceneId, route: route) else {
            return .fail("The clip scene or its editable video route does not exist.")
        }
        guard mediaClipPlaybackAvailable else { return .fail("Clip playback is unavailable in this build") }   // Mac
        previewSceneId = sceneId
        ensureLayers(for: sceneId)
        guard let sceneIndex = scenes.firstIndex(where: { $0.id == sceneId }),
              let layerIndex = scenes[sceneIndex].layers.firstIndex(where: { $0.id == layerId }) else {
            return .fail("The clip route is unavailable on Preview.")
        }
        scenes[sceneIndex].layers[layerIndex].slotId = nil
        scenes[sceneIndex].layers[layerIndex].mediaAssetId = clip.id
        syncScenes()
        return .appliedPreview(sceneId)
    }

    /// Windows CheckVoltRundownPreview: nil when Take may run.
    func checkVoltRundownPreview(_ key: String, scene sceneId: String, route: String) -> String? {
        guard let clip = voltRundownClip(key), previewSceneId == sceneId, programSceneId != sceneId, !takeInFlight,
              let layerId = voltClipLayerId(scene: sceneId, route: route),
              let layer = scenes.first(where: { $0.id == sceneId })?.layers.first(where: { $0.id == layerId }),
              layer.slotId == nil, layer.mediaAssetId == clip.id else {
            return "The staged Volt clip must be cued on Preview before Take."
        }
        let source = mediaSources.first { $0.mediaAssetId == clip.id }
        if source?.onProgram == true {
            return "The staged clip is already on Program. Take it off Program before replaying it."
        }
        guard let source, source.onPreview, (source.decodedVideoFrames ?? 0) > 0 else {
            return "Wait for the staged clip to decode on Preview before Take."
        }
        return nil
    }
}

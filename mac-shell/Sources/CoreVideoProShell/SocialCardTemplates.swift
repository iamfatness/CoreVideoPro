// The social chat card templates, byte copies of contracts/graphics-social/
// (the Windows shell embeds the same files as resources:
// GraphicsSocialPlayout.LoadCardDefinition). Regenerate from the contract
// files; never hand-edit. social/templates-are-byte-copies-of-contracts pins
// the bytes.
//
// Base64, not a raw JSON literal: ContractParity scans product Swift for
// "type":"<name>" as types the shell sends to the core, and the card's layer
// and animation types would read as unhandled requests.

import Foundation

enum SocialCardTemplates {
    static let youTube: String = decode(
        "ewogICJzY2hlbWFWZXJzaW9uIjoxLCJ0ZW1wbGF0ZUlkIjoieW91dHViZS1jaGF0LWNhcmQiLCJ0ZW1wbGF0ZVZlcnNpb24iOjEs"
        + "Im5hbWUiOiJZb3VUdWJlIGNoYXQiLCJmYW1pbHkiOiJ0aXRsZSIsCiAgImNhbnZhcyI6eyJ3aWR0aCI6MTkyMCwiaGVpZ2h0Ijox"
        + "MDgwfSwiYm91bmRzIjp7IndpZHRoIjoxNjAwLCJoZWlnaHQiOjIzMn0sCiAgImJyYW5kVG9rZW5zIjpbXSwKICAiZmllbGRzIjpb"
        + "CiAgICB7ImlkIjoiYXV0aG9yIiwidHlwZSI6InN0cmluZyIsInJlcXVpcmVkIjp0cnVlLCJkZWZhdWx0IjoiVmlld2VyIiwibWF4"
        + "TGVuZ3RoIjo4MH0sCiAgICB7ImlkIjoibWVzc2FnZSIsInR5cGUiOiJzdHJpbmciLCJyZXF1aXJlZCI6dHJ1ZSwiZGVmYXVsdCI6"
        + "IkV4YW1wbGUgbWVzc2FnZSIsIm1heExlbmd0aCI6MTkyfV0sCiAgImxheWVycyI6WwogICAgeyJpZCI6InBsYXRlIiwidHlwZSI6"
        + "InJlY3QiLCJ4IjowLCJ5IjowLCJ3aWR0aCI6MTYwMCwiaGVpZ2h0IjoyMzIsImZpbGwiOiIjMTQxODIyIn0sCiAgICB7ImlkIjoi"
        + "c291cmNlIiwidHlwZSI6InRleHQiLCJ0ZXh0IjoiWW91VHViZSBsaXZlIGNoYXQiLCJ4IjozOCwieSI6MjAsIndpZHRoIjoxNTI0"
        + "LCJoZWlnaHQiOjM0LAogICAgICJmb250IjoiU2Vnb2UgVUkiLCJmb250U2l6ZSI6MjIsImZvbnRXZWlnaHQiOjYwMCwiY29sb3Ii"
        + "OiIjRkZBM0EzIn0sCiAgICB7ImlkIjoiYXV0aG9yLWxpbmUiLCJ0eXBlIjoidGV4dCIsInRleHQiOnsiZmllbGQiOiJhdXRob3Ii"
        + "fSwieCI6MzgsInkiOjU3LCJ3aWR0aCI6MTUyNCwiaGVpZ2h0Ijo0MywKICAgICAiZm9udCI6IlNlZ29lIFVJIiwiZm9udFNpemUi"
        + "OjMyLCJtaW5Gb250U2l6ZSI6MTgsImZvbnRXZWlnaHQiOjYwMCwiY29sb3IiOiIjRkZGRkZGIn0sCiAgICB7ImlkIjoibWVzc2Fn"
        + "ZS1saW5lcyIsInR5cGUiOiJ0ZXh0IiwidGV4dCI6eyJmaWVsZCI6Im1lc3NhZ2UifSwieCI6MzgsInkiOjExMCwid2lkdGgiOjE1"
        + "MjQsImhlaWdodCI6MTAyLAogICAgICJmb250IjoiU2Vnb2UgVUkiLCJmb250U2l6ZSI6MzAsIm1pbkZvbnRTaXplIjoxOCwibWF4"
        + "TGluZXMiOjMsIm92ZXJmbG93Ijoid3JhcC10aGVuLWVsbGlwc2lzIiwiY29sb3IiOiIjRkZGRkZGIn1dLAogICJhbmltYXRpb25z"
        + "Ijp7ImluIjp7InR5cGUiOiJub25lIiwiZHVyYXRpb25NcyI6MH0sIm91dCI6eyJ0eXBlIjoibm9uZSIsImR1cmF0aW9uTXMiOjB9"
        + "fSwKICAiZGVmYXVsdFBsYWNlbWVudCI6eyJhbmNob3IiOiJsb3dlci1jZW50ZXIiLCJ4IjowLCJ5IjotNTQsInNjYWxlIjoxfQp9"
        + "Cg==")

    static let twitch: String = decode(
        "ewogICJzY2hlbWFWZXJzaW9uIjoxLCJ0ZW1wbGF0ZUlkIjoidHdpdGNoLWNoYXQtY2FyZCIsInRlbXBsYXRlVmVyc2lvbiI6MSwi"
        + "bmFtZSI6IlR3aXRjaCBjaGF0IiwiZmFtaWx5IjoidGl0bGUiLAogICJjYW52YXMiOnsid2lkdGgiOjE5MjAsImhlaWdodCI6MTA4"
        + "MH0sImJvdW5kcyI6eyJ3aWR0aCI6MTYwMCwiaGVpZ2h0IjoyMzJ9LAogICJicmFuZFRva2VucyI6W10sCiAgImZpZWxkcyI6Wwog"
        + "ICAgeyJpZCI6ImF1dGhvciIsInR5cGUiOiJzdHJpbmciLCJyZXF1aXJlZCI6dHJ1ZSwiZGVmYXVsdCI6IlZpZXdlciIsIm1heExl"
        + "bmd0aCI6ODB9LAogICAgeyJpZCI6Im1lc3NhZ2UiLCJ0eXBlIjoic3RyaW5nIiwicmVxdWlyZWQiOnRydWUsImRlZmF1bHQiOiJF"
        + "eGFtcGxlIG1lc3NhZ2UiLCJtYXhMZW5ndGgiOjE5Mn1dLAogICJsYXllcnMiOlsKICAgIHsiaWQiOiJwbGF0ZSIsInR5cGUiOiJy"
        + "ZWN0IiwieCI6MCwieSI6MCwid2lkdGgiOjE2MDAsImhlaWdodCI6MjMyLCJmaWxsIjoiIzE0MTgyMiJ9LAogICAgeyJpZCI6InNv"
        + "dXJjZSIsInR5cGUiOiJ0ZXh0IiwidGV4dCI6IlR3aXRjaCBsaXZlIGNoYXQiLCJ4IjozOCwieSI6MjAsIndpZHRoIjoxNTI0LCJo"
        + "ZWlnaHQiOjM0LAogICAgICJmb250IjoiU2Vnb2UgVUkiLCJmb250U2l6ZSI6MjIsImZvbnRXZWlnaHQiOjYwMCwiY29sb3IiOiIj"
        + "RkZBM0EzIn0sCiAgICB7ImlkIjoiYXV0aG9yLWxpbmUiLCJ0eXBlIjoidGV4dCIsInRleHQiOnsiZmllbGQiOiJhdXRob3IifSwi"
        + "eCI6MzgsInkiOjU3LCJ3aWR0aCI6MTUyNCwiaGVpZ2h0Ijo0MywKICAgICAiZm9udCI6IlNlZ29lIFVJIiwiZm9udFNpemUiOjMy"
        + "LCJtaW5Gb250U2l6ZSI6MTgsImZvbnRXZWlnaHQiOjYwMCwiY29sb3IiOiIjRkZGRkZGIn0sCiAgICB7ImlkIjoibWVzc2FnZS1s"
        + "aW5lcyIsInR5cGUiOiJ0ZXh0IiwidGV4dCI6eyJmaWVsZCI6Im1lc3NhZ2UifSwieCI6MzgsInkiOjExMCwid2lkdGgiOjE1MjQs"
        + "ImhlaWdodCI6MTAyLAogICAgICJmb250IjoiU2Vnb2UgVUkiLCJmb250U2l6ZSI6MzAsIm1pbkZvbnRTaXplIjoxOCwibWF4TGlu"
        + "ZXMiOjMsIm92ZXJmbG93Ijoid3JhcC10aGVuLWVsbGlwc2lzIiwiY29sb3IiOiIjRkZGRkZGIn1dLAogICJhbmltYXRpb25zIjp7"
        + "ImluIjp7InR5cGUiOiJub25lIiwiZHVyYXRpb25NcyI6MH0sIm91dCI6eyJ0eXBlIjoibm9uZSIsImR1cmF0aW9uTXMiOjB9fSwK"
        + "ICAiZGVmYXVsdFBsYWNlbWVudCI6eyJhbmNob3IiOiJsb3dlci1jZW50ZXIiLCJ4IjowLCJ5IjotNTQsInNjYWxlIjoxfQp9Cg==")

    private static func decode(_ base64: String) -> String {
        String(decoding: Data(base64Encoded: base64) ?? Data(), as: UTF8.self)
    }

    static func templateId(provider: String) -> String {
        provider == "Twitch" ? "twitch-chat-card" : "youtube-chat-card"
    }

    static func text(provider: String) -> String { provider == "Twitch" ? twitch : youTube }

    static func definition(provider: String) -> JSONObject {
        (try? JSONSerialization.jsonObject(with: Data(text(provider: provider).utf8))) as? JSONObject ?? [:]
    }
}

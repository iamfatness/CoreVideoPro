import type { MediaAsset, MediaAssetKind } from "../domain/production";

export const mediaAssetKinds: MediaAssetKind[] = ["stinger", "lower-third", "audio-bed", "slate"];

const KIND_LABELS: Record<MediaAssetKind, string> = {
  stinger: "Stinger",
  "lower-third": "Lower-third",
  "audio-bed": "Audio bed",
  slate: "Slate"
};

const DEFAULT_DURATION_MS: Partial<Record<MediaAssetKind, number>> = {
  stinger: 900,
  "audio-bed": 30000
};

export function assetKindLabel(kind: MediaAssetKind): string {
  return KIND_LABELS[kind];
}

/**
 * Build a new media-bin asset of the given kind, numbering it within its kind
 * so repeated adds read "Stinger 2", "Stinger 3", and so on.
 */
export function createMediaAsset(kind: MediaAssetKind, existing: MediaAsset[]): MediaAsset {
  const ordinal = existing.filter((asset) => asset.kind === kind).length + 1;

  return {
    id: `${kind}-${ordinal}`,
    name: `${assetKindLabel(kind)} ${ordinal}`,
    kind,
    durationMs: DEFAULT_DURATION_MS[kind]
  };
}

export function addMediaAsset(assets: MediaAsset[], kind: MediaAssetKind): MediaAsset[] {
  return [...assets, createMediaAsset(kind, assets)];
}

export function removeMediaAsset(assets: MediaAsset[], assetId: string): MediaAsset[] {
  return assets.filter((asset) => asset.id !== assetId);
}

export type MediaBinGroup = {
  kind: MediaAssetKind;
  label: string;
  assets: MediaAsset[];
};

export function groupMediaBin(assets: MediaAsset[]): MediaBinGroup[] {
  return mediaAssetKinds
    .map((kind) => ({ kind, label: assetKindLabel(kind), assets: assets.filter((asset) => asset.kind === kind) }))
    .filter((group) => group.assets.length > 0);
}

export function summarizeMediaBin(assets: MediaAsset[]): string {
  if (assets.length === 0) {
    return "Empty bin";
  }

  const counts = groupMediaBin(assets)
    .map((group) => `${group.assets.length} ${group.label.toLowerCase()}${group.assets.length === 1 ? "" : "s"}`)
    .join(", ");

  return `${assets.length} assets - ${counts}`;
}

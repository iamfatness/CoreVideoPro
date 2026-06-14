import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { addMediaAsset, createMediaAsset, groupMediaBin, removeMediaAsset, summarizeMediaBin } from "./mediaBin";

describe("createMediaAsset", () => {
  it("numbers a new asset within its kind and applies a default duration", () => {
    const asset = createMediaAsset("stinger", initialProduction.mediaBin);
    expect(asset).toMatchObject({ id: "stinger-2", name: "Stinger 2", kind: "stinger", durationMs: 900 });
  });

  it("leaves duration undefined for kinds without a default", () => {
    const asset = createMediaAsset("slate", []);
    expect(asset).toMatchObject({ id: "slate-1", name: "Slate 1" });
    expect(asset.durationMs).toBeUndefined();
  });
});

describe("addMediaAsset / removeMediaAsset", () => {
  it("appends without mutating and can remove by id", () => {
    const added = addMediaAsset(initialProduction.mediaBin, "slate");
    expect(added).toHaveLength(initialProduction.mediaBin.length + 1);
    expect(initialProduction.mediaBin).toHaveLength(3);

    const removed = removeMediaAsset(added, "slate-1");
    expect(removed.some((asset) => asset.id === "slate-1")).toBe(false);
  });
});

describe("groupMediaBin", () => {
  it("groups present kinds in catalog order and omits empty kinds", () => {
    const groups = groupMediaBin(initialProduction.mediaBin);
    expect(groups.map((group) => group.kind)).toEqual(["stinger", "lower-third", "audio-bed"]);
    expect(groups[0].label).toBe("Stinger");
  });
});

describe("summarizeMediaBin", () => {
  it("summarizes counts per kind", () => {
    expect(summarizeMediaBin(initialProduction.mediaBin)).toBe(
      "3 assets - 1 stinger, 1 lower-third, 1 audio bed"
    );
  });

  it("reports an empty bin", () => {
    expect(summarizeMediaBin([])).toBe("Empty bin");
  });
});

import { describe, expect, it } from "vitest";
import { BrandKitStateModel } from "./brandKitState.js";

describe("BrandKitStateModel", () => {
  it("normalizes brand tokens and reports applied overlay count", () => {
    const model = new BrandKitStateModel();
    const brandKit = model.apply(
      {
        name: "Launch Night",
        logoText: "CoreVideo",
        brandColor: "#112233",
        accentColor: "#aabbcc",
        backgroundColor: "#0c1118",
        fontFamily: "Inter",
        lowerThirdStyle: "gradient"
      },
      2
    );

    expect(brandKit.summary).toBe("Launch Night applied to 2 overlays");
    expect(brandKit.logoText).toBe("CoreVideo");
    expect(brandKit.appliedOverlayCount).toBe(2);
  });
});
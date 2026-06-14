import type { OutputDestination, OutputProfile } from "../domain/production";

export type OutputProfileReadout = {
  profileId: string;
  isUltraHd: boolean;
  pixelsPerSecond: number;
  /** Encode budget relative to the 1080p60 reference (1 = one reference stream). */
  encoderLoadFactor: number;
  /** Estimated encoder headroom as a percentage; lower means a heavier profile. */
  encoderHeadroomPercent: number;
  /** Total upload bandwidth across every armed network destination, in Mbps. */
  uploadBandwidthMbps: number;
  /** Number of armed destinations that consume upload bandwidth (NDI is LAN-only). */
  networkDestinationCount: number;
  warnings: string[];
  summary: string;
};

// 1080p60 is the reference encode the headroom budget is sized against.
const REFERENCE_PIXELS_PER_SECOND = 1920 * 1080 * 60;
// A single 4K60 encode is roughly the ceiling a typical hardware encoder can
// sustain alongside compositing, so headroom drains to ~0 there.
const ENCODER_CEILING_FACTOR = 4;

function parseResolution(resolution: string): { width: number; height: number } {
  const [width, height] = resolution.toLowerCase().split("x").map((part) => Number.parseInt(part, 10));
  return {
    width: Number.isFinite(width) ? width : 0,
    height: Number.isFinite(height) ? height : 0
  };
}

export function isUltraHdProfile(profile: OutputProfile): boolean {
  const { width, height } = parseResolution(profile.resolution);
  return width >= 3840 || height >= 2160;
}

function roundTo(value: number, places: number): number {
  const factor = 10 ** places;
  return Math.round(value * factor) / factor;
}

/**
 * Derive an encoder-headroom and upload-bandwidth readout for the selected
 * output profile so the operator can see, before going live, whether a 4K
 * profile leaves enough encode budget and how much upload each armed
 * destination will demand. NDI is treated as LAN-only and excluded from the
 * upload total.
 */
export function computeOutputProfileReadout(
  profile: OutputProfile,
  destinations: OutputDestination[]
): OutputProfileReadout {
  const { width, height } = parseResolution(profile.resolution);
  const pixelsPerSecond = width * height * profile.fps;
  const encoderLoadFactor = roundTo(pixelsPerSecond / REFERENCE_PIXELS_PER_SECOND, 2);
  const encoderHeadroomPercent = Math.max(
    0,
    Math.round((1 - encoderLoadFactor / ENCODER_CEILING_FACTOR) * 100)
  );

  const networkDestinations = destinations.filter(
    (destination) => destination.enabled && destination.protocol !== "NDI"
  );
  const uploadBandwidthMbps = roundTo(
    networkDestinations.length * profile.targetBitrateMbps,
    1
  );

  const isUltraHd = isUltraHdProfile(profile);
  const warnings: string[] = [];

  if (encoderHeadroomPercent <= 15) {
    warnings.push(
      `${profile.name} leaves only ${encoderHeadroomPercent}% encoder headroom; close other GPU work before going live.`
    );
  }

  if (isUltraHd && networkDestinations.length > 1) {
    warnings.push(
      `Streaming ${profile.name} to ${networkDestinations.length} destinations needs ${uploadBandwidthMbps} Mbps upload; confirm sustained headroom.`
    );
  }

  return {
    profileId: profile.id,
    isUltraHd,
    pixelsPerSecond,
    encoderLoadFactor,
    encoderHeadroomPercent,
    uploadBandwidthMbps,
    networkDestinationCount: networkDestinations.length,
    warnings,
    summary: `${profile.name} - ${encoderHeadroomPercent}% encoder headroom, ${uploadBandwidthMbps} Mbps upload`
  };
}

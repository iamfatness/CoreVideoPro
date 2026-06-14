/**
 * Output watermark engine.
 * Manages text/logo watermark overlays on the program output.
 * Supports live (rendered on stream) and burn-in (baked to recording) modes,
 * with confidentiality classification banners for enterprise use.
 */

export type WatermarkPosition =
  | "top-left"
  | "top-center"
  | "top-right"
  | "bottom-left"
  | "bottom-center"
  | "bottom-right"
  | "center";

export type WatermarkMode = "live" | "burn-in" | "disabled";

export type ConfidentialityLevel =
  | "public"
  | "internal"
  | "confidential"
  | "restricted";

export type WatermarkConfig = {
  mode: WatermarkMode;
  text: string;
  position: WatermarkPosition;
  /** 0–100 */
  opacityPct: number;
  /** Font size in viewport-relative units (1–5) */
  sizePx: number;
  color: string;
  /** Whether to show a confidentiality classification banner */
  showClassification: boolean;
  classification: ConfidentialityLevel;
};

export type WatermarkRenderSpec = {
  config: WatermarkConfig;
  cssPosition: Record<string, string>;
  cssStyle: Record<string, string>;
  classificationBanner: string | null;
  active: boolean;
  summary: string;
};

const DEFAULT_CONFIG: WatermarkConfig = {
  mode: "disabled",
  text: "",
  position: "bottom-right",
  opacityPct: 30,
  sizePx: 14,
  color: "#ffffff",
  showClassification: false,
  classification: "internal",
};

const CLASSIFICATION_LABELS: Record<ConfidentialityLevel, string> = {
  public: "PUBLIC",
  internal: "INTERNAL USE ONLY",
  confidential: "CONFIDENTIAL",
  restricted: "RESTRICTED — DO NOT DISTRIBUTE",
};

const CLASSIFICATION_COLORS: Record<ConfidentialityLevel, string> = {
  public: "#22c55e",
  internal: "#60a5fa",
  confidential: "#f59e0b",
  restricted: "#ef4444",
};

/**
 * Create a default watermark configuration.
 */
export function createWatermark(overrides: Partial<WatermarkConfig> = {}): WatermarkConfig {
  return { ...DEFAULT_CONFIG, ...overrides };
}

/**
 * Update watermark config fields.
 */
export function updateWatermark(
  config: WatermarkConfig,
  update: Partial<WatermarkConfig>
): WatermarkConfig {
  const next = { ...config, ...update };
  // Clamp opacity
  next.opacityPct = Math.max(0, Math.min(100, next.opacityPct));
  // Clamp size
  next.sizePx = Math.max(8, Math.min(48, next.sizePx));
  return next;
}

/**
 * Compute CSS position properties for a given position string.
 */
export function positionToCSS(position: WatermarkPosition): Record<string, string> {
  const base: Record<string, string> = { position: "absolute" };

  switch (position) {
    case "top-left":      return { ...base, top: "5%", left: "2%" };
    case "top-center":    return { ...base, top: "5%", left: "50%", transform: "translateX(-50%)" };
    case "top-right":     return { ...base, top: "5%", right: "2%" };
    case "bottom-left":   return { ...base, bottom: "5%", left: "2%" };
    case "bottom-center": return { ...base, bottom: "5%", left: "50%", transform: "translateX(-50%)" };
    case "bottom-right":  return { ...base, bottom: "5%", right: "2%" };
    case "center":        return { ...base, top: "50%", left: "50%", transform: "translate(-50%, -50%)" };
  }
}

/**
 * Build the full render spec for the current watermark config.
 */
export function buildWatermarkSpec(config: WatermarkConfig): WatermarkRenderSpec {
  const active = config.mode !== "disabled" && config.text.trim().length > 0;

  const cssPosition = active ? positionToCSS(config.position) : {};
  const cssStyle: Record<string, string> = active
    ? {
        opacity: String(config.opacityPct / 100),
        fontSize: `${config.sizePx}px`,
        color: config.color,
        fontFamily: "monospace",
        fontWeight: "600",
        letterSpacing: "0.05em",
        pointerEvents: "none",
        userSelect: "none",
        whiteSpace: "nowrap",
      }
    : {};

  const classificationBanner =
    active && config.showClassification
      ? CLASSIFICATION_LABELS[config.classification]
      : null;

  const summary = buildSummary(config, active);

  return { config, cssPosition, cssStyle, classificationBanner, active, summary };
}

/**
 * Validate the watermark config and return any issues.
 */
export function validateWatermark(config: WatermarkConfig): string[] {
  const issues: string[] = [];

  if (config.mode !== "disabled" && config.text.trim() === "") {
    issues.push("Watermark text is empty — enter text or disable");
  }

  if (config.opacityPct > 70 && config.mode === "live") {
    issues.push(`High opacity (${config.opacityPct}%) may obstruct content — consider ≤50%`);
  }

  if (config.mode === "burn-in" && config.classification === "public") {
    issues.push("Burn-in watermark on public content — confirm this is intentional");
  }

  return issues;
}

/**
 * Get the classification banner color for a given level.
 */
export function classificationColor(level: ConfidentialityLevel): string {
  return CLASSIFICATION_COLORS[level];
}

/**
 * List all available positions.
 */
export const watermarkPositions: WatermarkPosition[] = [
  "top-left", "top-center", "top-right",
  "bottom-left", "bottom-center", "bottom-right",
  "center",
];

/**
 * List confidentiality levels in ascending order of sensitivity.
 */
export const confidentialityLevels: ConfidentialityLevel[] = [
  "public", "internal", "confidential", "restricted",
];

function buildSummary(config: WatermarkConfig, active: boolean): string {
  if (!active) return "Watermark disabled";
  const modeLabel = config.mode === "burn-in" ? "Burn-in" : "Live";
  const classLabel = config.showClassification
    ? ` + ${CLASSIFICATION_LABELS[config.classification]}`
    : "";
  return `${modeLabel} watermark "${config.text}" @ ${config.position} ${config.opacityPct}%${classLabel}`;
}

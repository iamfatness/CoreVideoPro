import type { ColorGrade, ColorGradeLut } from "../domain/production";

type GradeOffsets = Pick<ColorGrade, "exposure" | "contrast" | "saturation" | "temperature">;

export const colorGradeLuts: ColorGradeLut[] = ["none", "neutral", "warm-film", "cool-broadcast", "punch"];

const lutPresets: Record<ColorGradeLut, GradeOffsets> = {
  none: { exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
  neutral: { exposure: 0, contrast: 5, saturation: 5, temperature: 0 },
  "warm-film": { exposure: 4, contrast: 10, saturation: -5, temperature: 24 },
  "cool-broadcast": { exposure: 0, contrast: 12, saturation: 10, temperature: -20 },
  punch: { exposure: 6, contrast: 22, saturation: 22, temperature: 4 }
};

function clamp(value: number): number {
  if (Number.isNaN(value)) {
    return 0;
  }

  return Math.min(50, Math.max(-50, Math.round(value)));
}

/**
 * Combine the selected LUT preset with the operator's manual offsets into the
 * effective grade, clamped to a safe ±50 range on each axis.
 */
export function resolveColorGrade(grade: ColorGrade): GradeOffsets {
  const preset = lutPresets[grade.lut];
  return {
    exposure: clamp(preset.exposure + grade.exposure),
    contrast: clamp(preset.contrast + grade.contrast),
    saturation: clamp(preset.saturation + grade.saturation),
    temperature: clamp(preset.temperature + grade.temperature)
  };
}

/**
 * Render the effective grade as a CSS filter applied to the program canvas.
 * Temperature warms via sepia and cools via a hue rotation.
 */
export function colorGradeFilter(grade: ColorGrade): string {
  const resolved = resolveColorGrade(grade);
  const brightness = (1 + resolved.exposure / 100).toFixed(2);
  const contrast = (1 + resolved.contrast / 100).toFixed(2);
  const saturate = (1 + resolved.saturation / 100).toFixed(2);
  const sepia = (Math.max(0, resolved.temperature) / 200).toFixed(2);
  const hueRotate = Math.round(-resolved.temperature * 0.2);

  return `brightness(${brightness}) contrast(${contrast}) saturate(${saturate}) sepia(${sepia}) hue-rotate(${hueRotate}deg)`;
}

export function isNeutralGrade(grade: ColorGrade): boolean {
  const resolved = resolveColorGrade(grade);
  return resolved.exposure === 0 && resolved.contrast === 0 && resolved.saturation === 0 && resolved.temperature === 0;
}

export function summarizeColorGrade(grade: ColorGrade): string {
  if (grade.lut === "none" && isNeutralGrade(grade)) {
    return "No grade - source passthrough";
  }

  const resolved = resolveColorGrade(grade);
  return `${grade.lut} - exp ${signed(resolved.exposure)} / con ${signed(resolved.contrast)} / sat ${signed(resolved.saturation)} / temp ${signed(resolved.temperature)}`;
}

function signed(value: number): string {
  return value > 0 ? `+${value}` : `${value}`;
}

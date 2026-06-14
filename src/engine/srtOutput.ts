export type SrtMode = "caller" | "listener";
export type SrtKeyLength = 16 | 24 | 32;

export type SrtConnectionParams = {
  host: string;
  port: number;
  mode: SrtMode;
  /** TSBPD latency in milliseconds */
  latencyMs: number;
  passphrase: string | null;
  /** AES key length in bytes: 16=AES-128, 24=AES-192, 32=AES-256 */
  keyLength: SrtKeyLength;
};

export type SrtValidation = {
  valid: boolean;
  params: SrtConnectionParams | null;
  errors: string[];
  warnings: string[];
};

const SRT_MIN_PASSPHRASE = 10;
const SRT_MAX_PASSPHRASE = 79;
const SRT_DEFAULT_LATENCY_MS = 120;
const VALID_KEY_LENGTHS: SrtKeyLength[] = [16, 24, 32];

export function parseSrtUrl(url: string): SrtValidation {
  const errors: string[] = [];
  const warnings: string[] = [];

  if (!url) {
    return { valid: false, params: null, errors: ["SRT endpoint is required."], warnings };
  }

  if (!url.startsWith("srt://")) {
    return { valid: false, params: null, errors: ["SRT endpoint must start with srt://."], warnings };
  }

  let parsed: URL;
  try {
    // URL API needs a valid scheme; srt:// is non-standard so we substitute https for parsing.
    parsed = new URL(url.replace(/^srt:\/\//, "https://"));
  } catch {
    return { valid: false, params: null, errors: [`Invalid SRT URL: ${url}`], warnings };
  }

  const host = parsed.hostname;
  if (!host) {
    errors.push("SRT endpoint is missing a host.");
  }

  const port = parsed.port ? parseInt(parsed.port, 10) : 0;
  if (!port || port < 1 || port > 65535) {
    errors.push("SRT endpoint requires a valid port (1–65535).");
  }

  const modeParam = parsed.searchParams.get("mode");
  const mode: SrtMode = modeParam === "listener" ? "listener" : "caller";

  const latencyParam = parsed.searchParams.get("latency");
  let latencyMs = latencyParam ? parseInt(latencyParam, 10) : SRT_DEFAULT_LATENCY_MS;
  if (Number.isNaN(latencyMs) || latencyMs < 20) {
    warnings.push("SRT latency is very low — consider at least 120 ms for reliable delivery.");
    latencyMs = Math.max(latencyMs, 20);
  }
  if (latencyMs > 8000) {
    warnings.push("SRT latency above 8 s may cause significant glass-to-glass delay.");
  }

  const passphraseParam = parsed.searchParams.get("passphrase");
  const passphrase = passphraseParam && passphraseParam.length > 0 ? passphraseParam : null;
  if (passphrase !== null) {
    if (passphrase.length < SRT_MIN_PASSPHRASE) {
      errors.push(`Passphrase must be at least ${SRT_MIN_PASSPHRASE} characters for AES encryption.`);
    } else if (passphrase.length > SRT_MAX_PASSPHRASE) {
      errors.push(`Passphrase must not exceed ${SRT_MAX_PASSPHRASE} characters.`);
    }
  }

  const keyLenParam = parsed.searchParams.get("pbkeylen");
  let keyLength: SrtKeyLength = 16;
  if (keyLenParam) {
    const parsed_len = parseInt(keyLenParam, 10) as SrtKeyLength;
    if (VALID_KEY_LENGTHS.includes(parsed_len)) {
      keyLength = parsed_len;
    } else {
      warnings.push(`Unknown pbkeylen ${keyLenParam} — defaulting to AES-128 (16).`);
    }
  }

  if (mode === "listener") {
    warnings.push("Listener mode: CoreVideo Pro will wait for an inbound SRT caller.");
  }

  if (errors.length > 0) {
    return { valid: false, params: null, errors, warnings };
  }

  return {
    valid: true,
    params: { host, port, mode, latencyMs, passphrase, keyLength },
    errors,
    warnings
  };
}

/**
 * Recommend SRT TSBPD latency as 2.5× the measured RTT, minimum 120 ms.
 * SRT spec recommends at least 4× to guarantee retransmit in lossy conditions.
 */
export function recommendSrtLatency(rttMs: number): number {
  return Math.max(SRT_DEFAULT_LATENCY_MS, Math.round(rttMs * 2.5));
}

/**
 * Estimate SRT protocol overhead as a percentage of raw bitrate.
 * Higher latency = more ARQ retransmit budget = slightly more overhead.
 * Baseline ~3 %, adds ~0.5 % per 500 ms latency, caps at 8 %.
 */
export function estimateSrtOverheadPercent(latencyMs: number): number {
  const base = 3;
  const extra = Math.floor(latencyMs / 500) * 0.5;
  return Math.min(8, base + extra);
}

/**
 * Effective bitrate after SRT overhead is removed from the target bitrate.
 */
export function effectiveSrtBitrateKbps(targetKbps: number, latencyMs: number): number {
  const overhead = estimateSrtOverheadPercent(latencyMs) / 100;
  return Math.round(targetKbps * (1 - overhead));
}

export function describeSrtConnection(params: SrtConnectionParams): string {
  const enc = params.passphrase ? `AES-${params.keyLength * 8}` : "unencrypted";
  const overhead = estimateSrtOverheadPercent(params.latencyMs);
  return `${params.host}:${params.port} (${params.mode}, ${params.latencyMs} ms TSBPD, ${enc}, ~${overhead}% overhead)`;
}

export function srtKeyLengthLabel(keyLength: SrtKeyLength): string {
  return `AES-${keyLength * 8}`;
}

import { createHash, randomBytes } from "node:crypto";

export function randomBase64Url(byteCount: number): string {
  return randomBytes(byteCount).toString("base64url");
}

export function pkceChallenge(verifier: string): string {
  return createHash("sha256").update(verifier).digest("base64url");
}
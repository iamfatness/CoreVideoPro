import { describe, expect, it } from "vitest";
import { createServer } from "node:http";
import { nodeFetch } from "./nodeFetch.js";

describe("nodeFetch", () => {
  it("honors an AbortSignal: a hung endpoint rejects with AbortError when the signal fires", async () => {
    const server = createServer(() => { /* never respond */ });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("no port");
    const controller = new AbortController();
    const pending = nodeFetch(`http://127.0.0.1:${address.port}/hang`, { signal: controller.signal });
    setTimeout(() => controller.abort(), 20);
    await expect(pending).rejects.toMatchObject({ name: "AbortError" });
    server.closeAllConnections();
    await new Promise<void>((r) => server.close(() => r()));
  });

  it("returns status and body text for a normal response", async () => {
    const server = createServer((_, res) => { res.statusCode = 200; res.end('{"ok":1}'); });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("no port");
    const response = await nodeFetch(`http://127.0.0.1:${address.port}/x`, {});
    expect(response.status).toBe(200);
    expect(await response.text()).toBe('{"ok":1}');
    await new Promise<void>((r) => server.close(() => r()));
  });
});

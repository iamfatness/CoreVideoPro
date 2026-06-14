import { describe, expect, it } from "vitest";
import { createNativeHostTransport, type NativeHostBridge } from "./nativeHostBridge";
import type { NativeBridgeCommand } from "./nativeBridgeProtocol";

describe("native host bridge", () => {
  it("adapts any desktop shell preload bridge into the native transport contract", async () => {
    const commands: NativeBridgeCommand[] = [];
    const bridge: NativeHostBridge = {
      host: "test-host",
      platform: "win32",
      async request(command) {
        commands.push(command);
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Native media core is not running."
          }
        };
      }
    };

    const transport = createNativeHostTransport(bridge);
    const response = await transport.request({ id: "1", type: "snapshot" });

    expect(commands).toEqual([{ id: "1", type: "snapshot" }]);
    expect(response).toMatchObject({
      ok: false,
      error: {
        code: "native-unavailable"
      }
    });
  });
});

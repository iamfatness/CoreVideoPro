import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { App } from "./App";
import { createTestCaptureDevices, MockCaptureDeviceEngine } from "./engine/captureDevices";
import { createMockEngineBundle, type EngineBundle } from "./engine/engineBundle";
import { mapCaptureSnapshot } from "./engine/captureSnapshotMapper";
import { InMemoryMediaCoreSyncEngine } from "./engine/mediaCoreSync";
import { initialParticipants } from "./domain/production";
import type { ProductionState } from "./domain/production";
import type { NativeMediaCoreEvent, NativeMediaCoreOperatorAction } from "./engine/nativeMediaCoreProtocol";
import type { RuntimeEnvironment } from "./engine/runtimeEnvironment";

const mockRuntime: RuntimeEnvironment = {
  status: "mock",
  label: "Mock studio",
  host: "browser-preview",
  platform: "web",
  warnings: ["Running with simulated Zoom and output engines."],
  capabilities: []
};

function renderApp(engines: EngineBundle = createMockEngineBundle(), runtime: RuntimeEnvironment = mockRuntime) {
  return render(<App engines={engines} runtime={runtime} />);
}

async function goToTab(user: ReturnType<typeof userEvent.setup>, name: RegExp | string) {
  await user.click(screen.getByRole("button", { name }));
}

describe("App production controls", () => {
  it("shows the desktop runtime contract in the operator surface", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const runtime = screen.getByLabelText("Desktop runtime");
    expect(within(runtime).getByText("Mock studio")).toBeInTheDocument();
    expect(within(runtime).getByText("browser-preview / web")).toBeInTheDocument();
  });

  it("shows native core sync status from production state", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const nativeCore = await screen.findByLabelText("Native core sync");
    expect(nativeCore).toHaveTextContent("Synced scenespeaker-slides");
    expect(nativeCore).toHaveTextContent("Routes2");
    expect(nativeCore).toHaveTextContent("Frames2");
    expect(nativeCore).toHaveTextContent("Overlays1");
    expect(nativeCore).toHaveTextContent("OutputsIdle");
    expect(nativeCore).toHaveTextContent("RecordingOff");

    await user.click(screen.getByRole("button", { name: "Record" }));
    await waitFor(() => expect(nativeCore).toHaveTextContent("Recordingrecording"));
    expect(nativeCore).toHaveTextContent("Frames3");
    expect(nativeCore).toHaveTextContent("Sources7");
    expect(nativeCore).toHaveTextContent("Resolved routes2");
    expect(nativeCore).toHaveTextContent("Render plan3 layers");
    expect(nativeCore).toHaveTextContent(/Plan IDrp-/);
    expect(nativeCore).toHaveTextContent("Media sourcetest-pattern");
    expect(nativeCore).toHaveTextContent(/Program frames[1-9]\d*/);
    expect(nativeCore).toHaveTextContent("Transportpublishing");
    expect(nativeCore).toHaveTextContent("Compositorlive");
    expect(nativeCore).toHaveTextContent("Encoderencoding");
    expect(nativeCore).toHaveTextContent("Senders0 idle");
    expect(nativeCore).toHaveTextContent("Outputsrecording");
    expect(nativeCore).toHaveTextContent("Profile1080p60");
    expect(nativeCore).toHaveTextContent("Disk rate7.49 MB/s");
    expect(nativeCore).toHaveTextContent("Output healthlive");
    expect(nativeCore).toHaveTextContent("Audio mix");
    expect(nativeCore).toHaveTextContent("Caption tracklive");
    expect(nativeCore).toHaveTextContent("ActionNone");
  });

  it("executes media-core recovery actions from Settings", async () => {
    const user = userEvent.setup();
    const action: NativeMediaCoreOperatorAction = {
      actionId: "sender:rtmp:recover",
      severity: "critical",
      area: "sender",
      title: "Recover RTMP sender",
      detail: "RTMP connection refused.",
      command: "recover-output-sender:rtmp",
      relatedId: "rtmp:program"
    };
    class ActionMediaCoreEngine extends InMemoryMediaCoreSyncEngine {
      private recovered = false;
      override async executeOperatorAction(
        state: ProductionState,
        pendingAction: NativeMediaCoreOperatorAction,
        elapsedMs: number
      ) {
        this.recovered = true;
        return super.executeOperatorAction(state, pendingAction, elapsedMs);
      }
      override async syncProduction(state: ProductionState, elapsedMs: number) {
        const snapshot = await super.syncProduction(state, elapsedMs);
        // Once the operator recovers the sender, the core stops re-emitting the
        // action; mirror that so the readout deterministically clears instead of
        // racing the next sync tick.
        if (this.recovered) {
          return snapshot;
        }
        return {
          ...snapshot,
          operatorActions: [action],
          eventLog: [],
          diagnostics: {
            ...snapshot.diagnostics,
            operatorActions: [action],
            eventLog: []
          }
        };
      }
    }
    const mediaCore = new ActionMediaCoreEngine();
    const executeOperatorAction = vi.spyOn(mediaCore, "executeOperatorAction");
    const engines: EngineBundle = {
      ...createMockEngineBundle(),
      mediaCore
    };
    renderApp(engines);

    await goToTab(user, "Settings");
    const nativeCore = await screen.findByLabelText("Native core sync");
    expect(screen.getByRole("button", { name: /Recover RTMP sender/i })).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Recover RTMP sender/i }));

    await waitFor(() => expect(executeOperatorAction).toHaveBeenCalledWith(expect.any(Object), action, expect.any(Number)));
    await waitFor(() => expect(nativeCore).toHaveTextContent("ActionNone"));
    expect(screen.getAllByText("Recover RTMP sender executed").length).toBeGreaterThan(0);
  });

  it("filters media-core diagnostics events by severity and recovery", async () => {
    const user = userEvent.setup();
    const events: NativeMediaCoreEvent[] = [
      {
        eventId: "critical-sender",
        atMs: 3000,
        severity: "critical",
        area: "sender",
        title: "RTMP sender failed",
        detail: "RTMP connection refused.",
        relatedId: "rtmp",
        commandType: "fail-output-sender"
      },
      {
        eventId: "warning-route",
        atMs: 2000,
        severity: "warning",
        area: "routing",
        title: "Media core warning",
        detail: "Screen share route requested but no active screen share source is available."
      },
      {
        eventId: "info-recovery",
        atMs: 5000,
        severity: "info",
        area: "sender",
        title: "RTMP sender recovery requested",
        detail: "RTMP sender recovered from operator action.",
        relatedId: "rtmp",
        commandType: "recover-output-sender"
      }
    ];
    class DiagnosticsMediaCoreEngine extends InMemoryMediaCoreSyncEngine {
      override async syncProduction(state: ProductionState, elapsedMs: number) {
        const snapshot = await super.syncProduction(state, elapsedMs);
        return {
          ...snapshot,
          eventLog: events,
          diagnostics: {
            ...snapshot.diagnostics,
            eventLog: events
          }
        };
      }
    }
    renderApp({
      ...createMockEngineBundle(),
      mediaCore: new DiagnosticsMediaCoreEngine()
    });

    await goToTab(user, "Settings");
    const diagnostics = await screen.findByLabelText("Media core diagnostics");
    const timeline = screen.getByLabelText("Media core event timeline");
    expect(within(diagnostics).getByText("3 events")).toBeInTheDocument();
    expect(within(timeline).getByText("RTMP sender failed")).toBeInTheDocument();
    expect(within(timeline).getByText("Media core warning")).toBeInTheDocument();
    expect(within(timeline).getByText("RTMP sender recovery requested")).toBeInTheDocument();

    await user.click(within(screen.getByLabelText("Media core diagnostics filter")).getByRole("button", { name: "Critical" }));
    expect(within(timeline).getByText("RTMP sender failed")).toBeInTheDocument();
    expect(within(timeline).queryByText("Media core warning")).not.toBeInTheDocument();

    await user.click(within(screen.getByLabelText("Media core diagnostics filter")).getByRole("button", { name: "Warnings" }));
    expect(within(timeline).getByText("Media core warning")).toBeInTheDocument();
    expect(within(timeline).queryByText("RTMP sender failed")).not.toBeInTheDocument();

    await user.click(within(screen.getByLabelText("Media core diagnostics filter")).getByRole("button", { name: "Recovery" }));
    expect(within(timeline).getByText("RTMP sender recovery requested")).toBeInTheDocument();
    expect(within(timeline).queryByText("Media core warning")).not.toBeInTheDocument();
  });

  it("renders AI Studio show notes and highlight suggestions", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const notes = await screen.findByLabelText("AI Studio show notes");
    expect(within(notes).getByText("Q2 Product Update show notes")).toBeInTheDocument();
    expect(within(notes).getByText(/David Chen/i)).toBeInTheDocument();

    const highlights = screen.getByLabelText("AI Studio highlights");
    expect(within(highlights).getByText("Presenter plus slides moment")).toBeInTheDocument();
    expect(within(highlights).getByText("q2-product-update-david-chen-speaker-slides")).toBeInTheDocument();
  });

  it("runs Magic Scene through the engine and updates status", async () => {
    const user = userEvent.setup();
    renderApp();

    await user.click(screen.getByRole("button", { name: /Magic Scene/i }));
    await goToTab(user, "Automation");

    expect(screen.getByText(/Magic Scene built 5 scenes/i)).toBeInTheDocument();
    expect(screen.getByText(/Robert Smith is low resolution/i)).toBeInTheDocument();
  });

  it("drives production from the Stream Deck control surface", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");
    const surface = screen.getByLabelText("Control surface");
    expect(within(surface).getByText("6 buttons mapped - Stream Deck / Companion ready")).toBeInTheDocument();

    // Set & Forget starts on (seed mode is set-and-forget); the key reflects the flip.
    const autoKey = () => within(surface).getByRole("button", { name: "Control Set & Forget" });
    expect(within(autoKey()).getByText("On")).toBeInTheDocument();
    await user.click(autoKey());
    expect(within(autoKey()).getByText("Off")).toBeInTheDocument();

    // Record toggles the program to live.
    const recordKey = () => within(surface).getByRole("button", { name: "Control Record" });
    expect(within(recordKey()).getByText("Armed")).toBeInTheDocument();
    await user.click(recordKey());
    await waitFor(() => expect(within(recordKey()).getByText("Live")).toBeInTheDocument());
    expect(recordKey().className).toContain("active");
  });

  it("hydrates media frame telemetry into the program preview", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    expect(within(program).getByLabelText("Speaker slides layout")).toBeInTheDocument();
    expect((await within(program).findAllByText("David Chen")).length).toBeGreaterThan(0);
    expect(within(program).getAllByText("1920x1080 30fps").length).toBeGreaterThan(0);
    expect(within(program).getByText("Building what matters next")).toBeInTheDocument();
    expect(within(program).getAllByText(/crop \d+% \/ \d+ms/i).length).toBeGreaterThan(0);
    expect(within(program).getAllByText("David Chen").length).toBeGreaterThan(0);
    expect(within(program).getByText(/Lower-third lifted to protect slide captions/i)).toBeInTheDocument();
    expect(within(program).getByText("CoreVideo")).toBeInTheDocument();

    await goToTab(user, "Settings");
    expect(screen.getByText("Caption confidence")).toBeInTheDocument();
    expect(screen.getByText("Overlay guard")).toBeInTheDocument();
    expect(within(program).queryByText("Waiting for frame")).not.toBeInTheDocument();
  });

  it("toggles graphics library overlays in the program preview", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");

    expect(await within(program).findByText("CoreVideo")).toBeInTheDocument();
    expect(within(program).queryByText("Live")).not.toBeInTheDocument();

    await goToTab(user, "Overlays");
    await user.click(screen.getByRole("button", { name: /Live banner/i }));
    expect(within(program).getByText("Live")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Question CTA/i }));
    expect(within(program).getByText("Submit questions")).toBeInTheDocument();
  });

  it("edits the brand kit and applies it to the brand bug graphic", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    expect(await within(program).findByText("CoreVideo")).toBeInTheDocument();

    await goToTab(user, "Overlays");

    const logoInput = screen.getByLabelText("Brand logo text");
    await user.clear(logoInput);
    await user.type(logoInput, "Acme Live");

    await user.click(screen.getByRole("button", { name: /Apply brand kit to graphics/i }));

    expect(within(program).getByText("Acme Live")).toBeInTheDocument();
    expect(within(program).queryByText("CoreVideo")).not.toBeInTheDocument();
  });

  it("applies a brand background image to the program canvas", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const canvasStyle = () => (program.querySelector(".program-canvas") as HTMLElement).getAttribute("style") ?? "";
    expect(canvasStyle()).toContain("linear-gradient(135deg");
    expect(canvasStyle()).not.toContain("url(");

    await goToTab(user, "Overlays");
    await user.type(screen.getByLabelText("Brand background image URL"), "https://cdn.example.com/stage.jpg");

    expect(canvasStyle()).toContain("stage.jpg");
  });

  it("applies a program color grade LUT to the program canvas", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const canvasStyle = () => (program.querySelector(".program-canvas") as HTMLElement).getAttribute("style") ?? "";
    expect(canvasStyle()).toContain("contrast(1.00)");

    await goToTab(user, "Media");
    const gradePanel = screen.getByLabelText("Program color grade");
    expect(within(gradePanel).getByText("No grade - source passthrough")).toBeInTheDocument();

    await user.selectOptions(screen.getByLabelText("Color LUT"), "punch");

    expect(canvasStyle()).toContain("contrast(1.22)");
    expect(within(gradePanel).getByText(/^punch - exp/)).toBeInTheDocument();
  });

  it("applies caption style controls to the program caption", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const captionSpan = () => program.querySelector(".caption-strip-text span") as HTMLElement;
    await waitFor(() => expect(captionSpan().textContent).toMatch(/active speaker/i));
    const original = captionSpan().textContent ?? "";
    expect(original).not.toBe(original.toUpperCase());

    await goToTab(user, "Overlays");
    await user.click(screen.getByLabelText("Uppercase captions"));

    expect(captionSpan().textContent).toBe(original.toUpperCase());
  });

  it("shows per-speaker caption attribution in the transcript", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");
    const transcript = screen.getByLabelText("Speaker captions");

    // Captions stream into a rolling, per-speaker attributed transcript. Assert
    // the attribution structure rather than a specific (rolling-window) cue so
    // the test stays stable as caption cadence evolves.
    const participantNames = new Set(initialParticipants.map((participant) => participant.name));
    await waitFor(() => {
      const speakers = Array.from(transcript.querySelectorAll(".transcript-meta strong")).map(
        (node) => node.textContent?.trim() ?? ""
      );
      expect(speakers.some((name) => participantNames.has(name))).toBe(true);
    });

    // Each entry carries the speaker's role and confidence, plus a live caption line.
    expect(within(transcript).getAllByText(/ - \d+%/).length).toBeGreaterThan(0);
    expect(
      within(transcript).getAllByText(
        /active speaker is framed|Magic Scene is holding|Lower-thirds and captions|Audio leveling is smoothing/
      ).length
    ).toBeGreaterThan(0);
  });

  it("saves and reloads a show preset", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Save Show" }));
    expect(await screen.findByText("Q2 Product Update Show saved")).toBeInTheDocument();

    await goToTab(user, "Overlays");
    await user.click(screen.getByRole("button", { name: /Live banner/i }));
    expect(within(program).getByText("Live")).toBeInTheDocument();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: /Q2 Product Update Show/i }));
    await screen.findAllByText("Q2 Product Update Show loaded");
    expect(within(program).queryByText("Live")).not.toBeInTheDocument();
  });

  it("exports a support bundle with production triage lines", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Export Bundle" }));

    expect((await screen.findAllByText(/support-q2-product-update/i)).length).toBeGreaterThan(0);
    const summary = screen.getByLabelText("Support bundle summary");
    expect(within(summary).getByText("Show: Q2 Product Update (set-and-forget)")).toBeInTheDocument();
    expect(within(summary).getByText(/Outputs: Outputs idle/i)).toBeInTheDocument();
    expect(within(summary).getByText(/ISO runway:/i)).toBeInTheDocument();
    expect(screen.getByText("Support bundle")).toBeInTheDocument();
  });

  it("queues scene templates and takes them to program with transitions", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Panel/i }));
    expect(screen.getByText("Panel queued")).toBeInTheDocument();
    expect(within(program).getByLabelText("Speaker slides layout")).toBeInTheDocument();

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    expect(screen.getByText("Interview queued")).toBeInTheDocument();
    await goToTab(user, "Automation");
    await user.click(within(screen.getByLabelText("Transition controls")).getByRole("button", { name: "slide" }));
    expect(screen.getByText("slide transition selected")).toBeInTheDocument();
    await goToTab(user, "Studio");
    await user.click(screen.getByRole("button", { name: "Take" }));
    expect(await screen.findByText("Interview taken with slide")).toBeInTheDocument();
    expect(within(program).getByLabelText("Interview layout")).toBeInTheDocument();

    await user.click(within(scenes).getByRole("button", { name: /Intro/i }));
    await user.click(screen.getByRole("button", { name: "Take" }));
    expect(within(program).getByLabelText("Intro layout")).toBeInTheDocument();
    await goToTab(user, "Overlays");
    expect(await screen.findByText("lower left")).toBeInTheDocument();
    expect(await screen.findByText("top")).toBeInTheDocument();
  });

  it("selects a stinger transition and plays a branded wipe on take", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const scenes = screen.getByLabelText("Scenes");
    expect(program.querySelector(".transition-wipe")).toBeNull();

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await goToTab(user, "Automation");
    await user.click(within(screen.getByLabelText("Transition controls")).getByRole("button", { name: "stinger" }));
    expect(screen.getByText("Stinger - 900ms branded wipe")).toBeInTheDocument();

    await goToTab(user, "Studio");
    await user.click(screen.getByRole("button", { name: "Take" }));

    expect(await screen.findByText("Interview taken with stinger")).toBeInTheDocument();
    expect(program.querySelector(".transition-wipe.wipe-stinger")).not.toBeNull();
  });

  it("runs live production commands from keyboard shortcuts", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.keyboard("t");
    expect(await screen.findByText("Interview taken with fade")).toBeInTheDocument();
    expect(within(program).getByLabelText("Interview layout")).toBeInTheDocument();
    expect(screen.getAllByText("Take").length).toBeGreaterThan(0);

    await user.keyboard("p");
    expect(screen.getByLabelText("Preview monitor")).toBeInTheDocument();
    expect(screen.getAllByText("Preview Monitor").length).toBeGreaterThan(0);

    await user.keyboard("f");
    expect((await screen.findAllByText(/00:08/i)).length).toBeGreaterThan(0);
    await goToTab(user, "Settings");
    expect(screen.getAllByText("Refresh feeds").length).toBeGreaterThan(0);

    await user.keyboard("m");
    await goToTab(user, "Automation");
    expect(await screen.findByText(/Magic Scene built 5 scenes/i)).toBeInTheDocument();

    await goToTab(user, "Studio");
    await user.keyboard("r");
    expect(await screen.findByRole("button", { name: "Recording" })).toBeInTheDocument();

    await user.keyboard("s");
    expect(await screen.findByRole("button", { name: "Streaming" })).toBeInTheDocument();
  });

  it("does not trigger keyboard shortcuts while editing fields", async () => {
    const user = userEvent.setup();
    renderApp();

    const filename = screen.getByLabelText("Recording filename prefix");
    await user.click(filename);
    await user.keyboard("r");

    expect(screen.getByRole("button", { name: "Record" })).toBeInTheDocument();
    expect(filename).toHaveValue("Q2_Product_Updater");
  });

  it("keeps the preview monitor optional for program-first workflows", async () => {
    const user = userEvent.setup();
    renderApp();

    expect(screen.queryByLabelText("Preview monitor")).not.toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Preview Monitor/i }));

    const preview = screen.getByLabelText("Preview monitor");
    expect(within(preview).getByText("Speaker + Slides")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Preview Monitor/i }));
    expect(screen.queryByLabelText("Preview monitor")).not.toBeInTheDocument();
  });

  it("assigns Zoom participants to preview scene slots before taking program", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.click(screen.getByRole("button", { name: /Preview Monitor/i }));
    await user.selectOptions(screen.getByLabelText("Slot 1 participant"), "p3");

    expect(screen.getByText("Jeremy Collins + Sophia Martinez")).toBeInTheDocument();
    expect(screen.getByText("Interview route 1 updated")).toBeInTheDocument();
    expect(within(program).queryByText("Jeremy Collins")).not.toBeInTheDocument();
    expect(within(screen.getByLabelText("Preview monitor")).getByText("Jeremy Collins")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Take" }));

    expect(within(program).getByLabelText("Interview layout")).toBeInTheDocument();
    expect(within(program).getAllByText("Jeremy Collins").length).toBeGreaterThanOrEqual(1);
  });

  it("routes preview scene slots by active speaker with audio role metadata", async () => {
    const user = userEvent.setup();
    renderApp();

    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.click(screen.getByRole("button", { name: /Preview Monitor/i }));
    await user.selectOptions(screen.getByLabelText("Slot 1 route mode"), "active-speaker");
    await user.selectOptions(screen.getByLabelText("Slot 1 audio role"), "mix");

    expect(screen.getByText("David Chen + Sophia Martinez")).toBeInTheDocument();
    expect(screen.getByText("Interview route 1 updated")).toBeInTheDocument();
    expect(within(screen.getByLabelText("Preview monitor")).getByText("David Chen")).toBeInTheDocument();
  });

  it("warns operators about duplicate fixed routes and duplicated isolated audio", async () => {
    const user = userEvent.setup();
    renderApp();

    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.selectOptions(screen.getByLabelText("Slot 1 participant"), "p2");
    await user.selectOptions(screen.getByLabelText("Slot 2 participant"), "p2");

    const warnings = screen.getByLabelText("Route warnings");
    expect(within(warnings).getByText("David Chen is assigned to multiple fixed routes.")).toBeInTheDocument();
    expect(within(warnings).getByText("David Chen has duplicated isolated audio.")).toBeInTheDocument();
    expect(screen.getByText("Route health")).toBeInTheDocument();
  });

  it("warns when a scene expects screen share but no share is available", async () => {
    const user = userEvent.setup();
    renderApp();
    await screen.findByText(/in meeting - 7 participants/i);
    await user.click(screen.getByRole("button", { name: /Set & Forget/i }));

    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });
    for (let index = 0; index < 4 && screen.queryAllByText("Slot 2 screen share is unavailable.").length === 0; index += 1) {
      await user.click(refresh);
      await waitFor(() => expect(screen.getByText(/in meeting - 7 participants/i)).toBeInTheDocument());
    }

    expect(screen.getAllByText("Slot 2 screen share is unavailable.").length).toBeGreaterThan(0);
  });

  it("auto-takes recommended scenes in Set & Forget mode", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });

    for (let index = 0; index < 8 && !within(program).queryByLabelText("Smart panel grid"); index += 1) {
      await user.click(refresh);
    }

    expect(within(program).getByLabelText("Smart panel grid")).toBeInTheDocument();
    expect(screen.getByText(/Set & Forget took Panel/i)).toBeInTheDocument();
    expect(screen.getByText("Auto director")).toBeInTheDocument();
    expect(screen.getByText(/Auto: (take|hold) \d+%/i)).toBeInTheDocument();
  });

  it("toggles recording and streaming states", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    expect(await screen.findByText("2 armed")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: /NDI Program/i }));
    expect(screen.getByText("3 armed")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Record" }));
    await user.click(screen.getByRole("button", { name: "Stream" }));

    expect(screen.getByRole("button", { name: "Recording" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Streaming" })).toBeInTheDocument();
    expect(screen.getByText(/Recording program \+ 2 ISOs and streaming to 3 destinations/i)).toBeInTheDocument();
    expect(screen.getByText(/Recordings\/CoreVideo Pro\/Q2_Product_Update.mp4/i)).toBeInTheDocument();
    expect(screen.getByText(/custom-rtmp/i)).toBeInTheDocument();
    expect(screen.getByText(/ndi-program/i)).toBeInTheDocument();
    expect(screen.getByText(/live 12 Mbps/i)).toBeInTheDocument();
  });

  it("plans multitrack audio from the ISO recording selection", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const plan = await screen.findByLabelText("Multitrack audio plan");

    // Seeded ISO selection is p1 + p2 → master + two ISO tracks.
    expect(within(plan).getByText("3 tracks - 4 ch - 0.58 MB/s")).toBeInTheDocument();
    expect(within(plan).getByText(/Program master - stereo/)).toBeInTheDocument();

    await user.click(screen.getByLabelText("Michael Thompson ISO recording"));
    expect(within(plan).getByText(/Michael Thompson ISO - mono/)).toBeInTheDocument();
    expect(within(plan).getByRole("status")).toHaveTextContent(/Michael Thompson's ISO track will be silent while muted/);
  });

  it("publishes the program through the virtual camera output", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const vcam = screen.getByLabelText("Virtual camera");
    expect(within(vcam).getByText("Virtual camera offline")).toBeInTheDocument();

    await user.click(within(vcam).getByRole("button", { name: /Start virtual camera/i }));
    expect(within(vcam).getByText("Publishing program to CoreVideo Pro Camera")).toBeInTheDocument();
    expect(within(vcam).getByText("1920x1080 60fps")).toBeInTheDocument();

    await user.click(within(vcam).getByLabelText("Mirror virtual camera"));
    expect(within(vcam).getByText("1920x1080 60fps mirrored")).toBeInTheDocument();
  });

  it("adds and removes assets in the media bin", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");
    const bin = screen.getByLabelText("Media bin");
    expect(within(bin).getByText("Empty bin")).toBeInTheDocument();

    await user.selectOptions(within(bin).getByLabelText("Media asset type"), "stinger");
    await user.click(within(bin).getByRole("button", { name: /Add asset/i }));
    expect(within(bin).getByText("Stinger 1")).toBeInTheDocument();
    expect(within(bin).getByText("1 assets - 1 stinger")).toBeInTheDocument();

    await user.selectOptions(within(bin).getByLabelText("Media asset type"), "slate");
    await user.click(within(bin).getByRole("button", { name: /Add asset/i }));
    expect(within(bin).getByText("Slate 1")).toBeInTheDocument();
    expect(within(bin).getByText("2 assets - 1 stinger, 1 slate")).toBeInTheDocument();

    await user.click(within(bin).getByRole("button", { name: "Remove Stinger 1" }));
    expect(within(bin).queryByText("Stinger 1")).not.toBeInTheDocument();
    expect(within(bin).getByText("1 assets - 1 slate")).toBeInTheDocument();
  });

  it("uses editable recording settings when recording starts", async () => {
    const user = userEvent.setup();
    const engines = createMockEngineBundle();
    renderApp(engines);

    await goToTab(user, "Settings");
    fireEvent.change(screen.getByLabelText("Recording folder"), {
      target: { value: "Recordings/Client Shows" }
    });
    fireEvent.change(screen.getByLabelText("Recording filename prefix"), {
      target: { value: "Customer_Panel" }
    });
    await user.selectOptions(screen.getByLabelText("Recording format"), "mkv");
    await user.selectOptions(screen.getByLabelText("Recording quality"), "archive");
    await user.click(screen.getByLabelText("Sophia Martinez ISO recording"));
    await user.click(screen.getByLabelText("Linda Park ISO recording"));
    await user.click(screen.getByRole("button", { name: "Record" }));

    const session = await engines.output.getSession();
    expect(session.recording).toBe(true);
    expect(session.recordingFile).toBe("Recordings/Client Shows/Customer_Panel.mkv");
    expect(session.recordingSettings).toMatchObject({
      format: "mkv",
      quality: "archive",
      isoParticipantIds: ["p2", "p7"]
    });
    expect(screen.getByLabelText("Recording folder")).toBeDisabled();
    expect(screen.getByLabelText("Linda Park ISO recording")).toBeDisabled();
    expect(screen.getByText(/Recordings\/Client Shows\/Customer_Panel.mkv/i)).toBeInTheDocument();
    expect(screen.getByText(/Recording program \+ 2 ISOs locally/i)).toBeInTheDocument();
  });

  it("blocks recording when recording preflight fails", async () => {
    const user = userEvent.setup();
    const engines = createMockEngineBundle();
    renderApp(engines);

    await goToTab(user, "Settings");
    fireEvent.change(screen.getByLabelText("Recording filename prefix"), {
      target: { value: "" }
    });
    await user.click(screen.getByRole("button", { name: "Record" }));

    const session = await engines.output.getSession();
    expect(session.recording).toBe(false);
    expect(screen.getByRole("button", { name: "Record" })).toBeInTheDocument();
    expect(screen.getByText(/Enter a recording filename before recording/i)).toBeInTheDocument();
  });

  it("edits stream destination settings before going live", async () => {
    const user = userEvent.setup();
    const engines = createMockEngineBundle();
    renderApp(engines);

    await goToTab(user, "Settings");
    fireEvent.change(screen.getByLabelText("Custom RTMP endpoint"), {
      target: { value: "rtmp://studio.example.com/live" }
    });
    fireEvent.change(screen.getByLabelText("Custom RTMP stream key"), {
      target: { value: "updated-key" }
    });
    await user.click(screen.getByRole("button", { name: "Stream" }));

    const session = await engines.output.getSession();
    expect(session.destinations.find((destination) => destination.id === "custom-rtmp")).toMatchObject({
      endpoint: "rtmp://studio.example.com/live",
      streamKey: "updated-key",
      active: true
    });
    expect(screen.getByLabelText("Custom RTMP endpoint")).toBeDisabled();
    expect(screen.getByText(/Streaming to 2 destinations/i)).toBeInTheDocument();
  });

  it("adds a Twitch destination from the streaming presets", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    expect(screen.queryByLabelText("Twitch endpoint")).not.toBeInTheDocument();

    await user.selectOptions(screen.getByLabelText("Streaming preset"), "twitch");
    await user.click(screen.getByRole("button", { name: /Add destination/i }));

    expect(screen.getByLabelText("Twitch endpoint")).toHaveValue("rtmp://live.twitch.tv/app");
    expect(screen.getByLabelText("Twitch stream key")).toBeInTheDocument();
  });

  it("blocks streaming when output preflight fails", async () => {
    const user = userEvent.setup();
    const engines = createMockEngineBundle();
    renderApp(engines);

    await goToTab(user, "Settings");
    fireEvent.change(screen.getByLabelText("Custom RTMP stream key"), {
      target: { value: "" }
    });
    await user.click(screen.getByRole("button", { name: "Stream" }));

    const session = await engines.output.getSession();
    expect(session.streaming).toBe(false);
    expect(screen.getByRole("button", { name: "Stream" })).toBeInTheDocument();
    expect(screen.getByText(/Custom RTMP needs a stream key/i)).toBeInTheDocument();
  });

  it("changes output profile before recording or streaming", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    await user.click(await screen.findByRole("button", { name: /4K 30/i }));

    expect(screen.getAllByText(/3840x2160/i).length).toBeGreaterThan(0);
    expect(screen.getAllByText("4K 30").length).toBeGreaterThan(0);
    expect(screen.getByText("30 fps")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Stream" }));
    expect(screen.getByText(/Streaming to 2 destinations at 36 Mbps/i)).toBeInTheDocument();
  });

  it("applies smart audio mix overrides from participant controls", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");
    expect(await screen.findByText("Audio status")).toBeInTheDocument();
    expect(screen.getByText("ducking")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Mute in mix" }));

    expect(screen.getByText("muted")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Unmute in mix" })).toBeInTheDocument();
    expect(screen.getByText(/Smart mix:/i)).toBeInTheDocument();
  });

  it("applies manual audio gain from participant controls", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");
    fireEvent.change(screen.getByLabelText("Manual audio gain"), {
      target: { value: "4" }
    });

    expect(await screen.findByText("+4 dB")).toBeInTheDocument();
    expect(screen.getByText(/Smart mix:/i)).toHaveTextContent("manual");
    expect(screen.getByText("balanced")).toBeInTheDocument();
  });

  it("lets producers override participant roles for Magic Scene automation", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");
    await user.click(screen.getByRole("button", { name: /Ava Patel/i }));
    await user.selectOptions(screen.getByLabelText("Production role"), "Host");

    expect(screen.getByText("Ava Patel set as Host for scene automation")).toBeInTheDocument();
    expect(screen.getByText("Ava Patel role set to Host")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Magic Scene/i }));

    expect(await screen.findByText(/Host open with Ava Patel/i)).toBeInTheDocument();
    await goToTab(user, "Studio");
    const participantPanel = screen.getByLabelText("Participants and source controls");
    const participantList = participantPanel.querySelector(".participant-list") as HTMLElement;
    expect(within(participantList).getByText("Ava Patel", { selector: "strong" }).closest(".participant-card")).toHaveTextContent("HOST");
  });

  it("preserves producer role overrides across Zoom feed refreshes", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");
    await user.click(screen.getByRole("button", { name: /Ava Patel/i }));
    await user.selectOptions(screen.getByLabelText("Production role"), "Host");
    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: /Refresh feeds/i }));

    expect((await screen.findAllByText(/00:08/i)).length).toBeGreaterThan(0);
    await goToTab(user, "Audio");
    expect(screen.getByLabelText("Production role")).toHaveValue("Host");
    await goToTab(user, "Studio");
    const participantPanel = screen.getByLabelText("Participants and source controls");
    const participantList = participantPanel.querySelector(".participant-list") as HTMLElement;
    expect(within(participantList).getByText("Ava Patel", { selector: "strong" }).closest(".participant-card")).toHaveTextContent("HOST");
  });

  it("applies participant video effects from smart handling controls", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");

    await goToTab(user, "Media");
    expect(await screen.findByText("Crop mode")).toBeInTheDocument();
    expect(screen.getAllByText("auto").length).toBeGreaterThan(0);

    await user.click(screen.getByRole("button", { name: "Manual crop" }));
    expect(screen.getByText("manual")).toBeInTheDocument();
    expect(screen.getByText("1.18x")).toBeInTheDocument();
    expect(within(program).getByText("manual 1.18x")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Enable chroma" }));
    expect(screen.getByText("green on")).toBeInTheDocument();
    expect(within(program).getByText("Chroma key")).toBeInTheDocument();
  });

  it("shows face-aware auto-crop framing and warns on low-quality feeds", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");
    const panel = await screen.findByLabelText("Face-aware auto-crop");
    expect(within(panel).getByText("Auto framing")).toBeInTheDocument();
    expect(within(panel).getByText("good")).toBeInTheDocument();
    expect(within(panel).queryByRole("status")).not.toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Engine On/i }));
    await goToTab(user, "Audio");
    await user.click(screen.getByRole("button", { name: /Robert Smith/i }));
    await goToTab(user, "Media");

    const lowQualityPanel = screen.getByLabelText("Face-aware auto-crop");
    expect(within(lowQualityPanel).getByText("low")).toBeInTheDocument();
    expect(within(lowQualityPanel).getByRole("status")).toHaveTextContent(/Low-quality feed for Robert Smith/i);
  });

  it("lists only video-enabled participants in the host current room", async () => {
    renderApp();

    const strip = await screen.findByLabelText("Video-enabled participants in current room");
    expect(screen.getByText("Main room", { selector: ".current-room-label" })).toBeInTheDocument();
    expect(within(strip).getByText(/Sophia Martinez/i)).toBeInTheDocument();
    expect(within(strip).getByText(/David Chen/i)).toBeInTheDocument();
    expect(within(strip).queryByText(/Robert Smith/i)).not.toBeInTheDocument();
    expect(within(strip).queryByText(/Linda Park/i)).not.toBeInTheDocument();
  });

  it("leaves, rejoins, and refreshes simulated Zoom feeds", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Leave" }));
    expect(screen.getByRole("button", { name: "Join Zoom" })).toBeInTheDocument();
    expect(screen.getByText(/idle - 0 participants/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Join Zoom" }));
    expect(screen.getByRole("button", { name: "Leave" })).toBeInTheDocument();
    expect(screen.getByText(/in meeting - 7 participants/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Refresh feeds/i }));
    expect(screen.getAllByText(/00:08/i).length).toBeGreaterThan(0);
  });

  it("passes editable Zoom connection details to the capture engine", async () => {
    const user = userEvent.setup();
    const baseEngines = createMockEngineBundle();
    const join = vi.fn(baseEngines.zoom.join.bind(baseEngines.zoom));
    const engines: EngineBundle = {
      ...baseEngines,
      zoom: {
        join,
        leave: baseEngines.zoom.leave.bind(baseEngines.zoom),
        getParticipants: baseEngines.zoom.getParticipants.bind(baseEngines.zoom),
        getSnapshot: baseEngines.zoom.getSnapshot.bind(baseEngines.zoom),
        advanceSimulation: baseEngines.zoom.advanceSimulation?.bind(baseEngines.zoom)
      }
    };

    renderApp(engines);

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Leave" }));
    fireEvent.change(screen.getByLabelText("Zoom meeting URL or ID"), {
      target: { value: "https://zoom.us/j/987654321" }
    });
    fireEvent.change(screen.getByLabelText("Producer display name"), {
      target: { value: "Guest Producer" }
    });
    await user.click(screen.getByLabelText("Webinar"));
    await user.click(screen.getByRole("button", { name: "Join Zoom" }));

    expect(join).toHaveBeenCalledWith({
      meetingUrl: "https://zoom.us/j/987654321",
      displayName: "Guest Producer",
      webinar: false
    });
    expect(await screen.findByText("Joined as Guest Producer")).toBeInTheDocument();
  });

  it("shows the screen-share fallback when sharing stops", async () => {
    const user = userEvent.setup();
    renderApp();
    await screen.findByText(/in meeting - 7 participants/i);
    await user.click(screen.getByRole("button", { name: /Set & Forget/i }));

    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });
    for (let index = 0; index < 4 && screen.queryAllByText(/No screen share/i).length === 0; index += 1) {
      await user.click(refresh);
      await waitFor(() => expect(screen.getByText(/in meeting - 7 participants/i)).toBeInTheDocument());
    }

    expect(screen.getAllByText(/No screen share/i).length).toBeGreaterThanOrEqual(1);
    await goToTab(user, "Studio");
    expect(within(screen.getByLabelText("Program preview")).getByText(/Waiting for screen share/i)).toBeInTheDocument();
  });

  it("renders from injected engine implementations", async () => {
    const injectedSnapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 5,
      caption: "Injected caption from native bridge.",
      activeSpeakerId: "native-1",
      participants: [
        {
          userId: "native-1",
          displayName: "Injected Speaker",
          role: "Presenter",
          title: "Native Bridge",
          talking: true,
          sharingScreen: false,
          networkQuality: "good"
        }
      ]
    });
    const engines: EngineBundle = {
      ...createMockEngineBundle(),
      zoom: {
        join: async () => injectedSnapshot,
        leave: async () => ({ ...injectedSnapshot, meetingState: "idle", participants: [], mediaFrames: [] }),
        getParticipants: async () => injectedSnapshot.participants,
        getSnapshot: async () => injectedSnapshot
      }
    };

    renderApp(engines);

    expect((await screen.findAllByText(/Injected Speaker/i)).length).toBeGreaterThan(0);
    expect(screen.getAllByText(/Injected caption from native bridge/i).length).toBeGreaterThan(0);
    expect(screen.getAllByText(/No screen share/i).length).toBeGreaterThan(0);
  });

  it("brings a second capture device online for dual capture", async () => {
    const user = userEvent.setup();
    const engines = createMockEngineBundle();
    (engines.captureDevices as MockCaptureDeviceEngine).seedDevices(createTestCaptureDevices());
    renderApp(engines);

    await goToTab(user, "Sources");
    const fleet = await screen.findByLabelText("Capture fleet");
    expect(within(fleet).getByText(/1 live \/ 1 connected/)).toBeInTheDocument();
    expect(within(fleet).queryByText("Dual capture live")).not.toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Bring online as program source/i }));

    expect(within(fleet).getByText(/2 live \/ 2 connected/)).toBeInTheDocument();
    expect(within(fleet).getByText("Dual capture live")).toBeInTheDocument();
  });

  it("summarizes NDI/SRT arming readiness as destinations are armed", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const arming = screen.getByLabelText("Arming readiness");
    expect(within(arming).getByText("2/2 ready - 2 RTMP")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /NDI Program/i }));
    await user.click(screen.getByRole("button", { name: /SRT Backup/i }));
    expect(within(arming).getByText("4/4 ready - 2 RTMP, 1 NDI, 1 SRT")).toBeInTheDocument();
    expect(screen.getByText(/NDI Program will publish on the local network/i)).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText("SRT Backup stream key"), { target: { value: "short" } });
    expect(within(arming).getByText("3/4 ready - 2 RTMP, 1 NDI, 1 SRT")).toBeInTheDocument();
    expect(within(arming).getByText(/SRT Backup passphrase must be at least 10 characters/i)).toBeInTheDocument();
  });

  it("shows encoder headroom and upload bandwidth for the selected output profile", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const headroom = screen.getByLabelText("Encoder headroom");
    expect(within(headroom).getByText("75%")).toBeInTheDocument();
    expect(within(headroom).getByText("16.4 Mbps")).toBeInTheDocument();

    await user.click(await screen.findByRole("button", { name: /4K 60/i }));
    expect(within(headroom).getByText("0%")).toBeInTheDocument();
    expect(within(headroom).getByText(/only 0% encoder headroom/i)).toBeInTheDocument();
  });
});

describe("stream health panel", () => {
  it("shows idle message when no destinations are active", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const panel = screen.getByLabelText("Stream health");
    expect(within(panel).getByText(/No active destinations/i)).toBeInTheDocument();
  });

  it("shows per-destination scores and aggregate after stream starts", async () => {
    const user = userEvent.setup();
    renderApp();

    await user.click(screen.getByRole("button", { name: "Stream" }));
    await waitFor(() => expect(screen.queryByRole("button", { name: "Streaming" })).toBeInTheDocument());

    await goToTab(user, "Settings");
    const panel = screen.getByLabelText("Stream health");
    expect(within(panel).getByText(/Avg score:/i)).toBeInTheDocument();
    expect(within(panel).getAllByText(/\/100/).length).toBeGreaterThan(0);
  });
});

describe("SRT output engine", () => {
  it("shows SRT connection detail and latency hint when SRT destination is armed", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: /SRT Backup/i }));

    const detail = await screen.findByLabelText("SRT Backup SRT detail");
    expect(detail).toHaveTextContent(/backup\.example\.com:9000/);
    expect(detail).toHaveTextContent(/caller/);
    // Recommended latency should differ from the parsed URL latency (120ms vs 1050ms)
    expect(detail).toHaveTextContent(/Recommended latency/i);
  });
});

describe("WebRTC monitor output", () => {
  it("lists the WebRTC WHIP preset in the streaming preset selector", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const select = screen.getByRole("combobox", { name: "Streaming preset" });
    expect(within(select as HTMLElement).getByRole("option", { name: "WebRTC (WHIP)" })).toBeInTheDocument();
  });

  it("adds a WebRTC destination from the WHIP preset", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    await user.selectOptions(screen.getByRole("combobox", { name: "Streaming preset" }), "webrtc-whip");
    await user.click(screen.getByRole("button", { name: /Add destination/i }));

    expect(screen.getByLabelText("WebRTC (WHIP) endpoint")).toBeInTheDocument();
    expect(screen.getByLabelText("WebRTC (WHIP) stream key")).toBeInTheDocument();
  });
});

describe("feed health roster", () => {
  it("shows feed health summary and per-participant badges in the Sources tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");
    const roster = screen.getByLabelText("Feed health roster");
    const summary = within(roster).getByLabelText("Feed health summary");

    // Main-room roster: 5 participants; most live, one muted
    expect(summary).toHaveTextContent(/\/5/);
    expect(summary).toHaveTextContent(/live/i);

    // Spot check: Sophia Martinez (live/Host) should show "Live" badge
    expect(within(roster).getByText("Sophia Martinez")).toBeInTheDocument();
    // Michael Thompson is seeded muted in the host room
    expect(within(roster).getByText("Michael Thompson")).toBeInTheDocument();
  });
});

describe("disk space monitor", () => {
  it("shows disk space status in the recording section", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    const disk = screen.getByLabelText("Disk space status");
    expect(disk).toHaveTextContent(/GB/i);
    // Seeded with 247.3 GB — should show ok status
    expect(disk).toHaveTextContent(/247/);
    expect(disk).toHaveTextContent(/free/i);
  });
});

describe("NDI output engine", () => {
  it("shows NDI source detail and bandwidth when the NDI destination is armed", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    // Arm the NDI Program destination
    await user.click(screen.getByRole("button", { name: /NDI Program/i }));

    const detail = await screen.findByLabelText("NDI Program NDI detail");
    expect(detail).toHaveTextContent(/local network/i);
    expect(detail).toHaveTextContent(/Mbps/i);
  });
});

describe("bandwidth planner", () => {
  it("shows the bandwidth plan panel in Settings", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Bandwidth plan panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText(/Total/i)).toBeInTheDocument();
    expect(within(panel).getByText(/Usage/i)).toBeInTheDocument();
  });

  it("shows OK or caution status based on active destinations", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Bandwidth plan panel");
    // Status should be one of the valid values
    expect(within(panel).getByText(/^safe$|^caution$|^overloaded$|No destinations/i)).toBeInTheDocument();
  });
});

describe("stinger presets", () => {
  it("shows the stinger presets panel in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Stinger list");
    expect(panel).toBeInTheDocument();
    // Should show at least 4 built-in presets
    const rows = within(panel).getAllByText(/\d+ms total/i);
    expect(rows.length).toBeGreaterThanOrEqual(4);
  });

  it("lists Brand Wipe and Logo Reveal presets", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    expect(screen.getByLabelText("Stinger Brand Wipe")).toBeInTheDocument();
    expect(screen.getByLabelText("Stinger Logo Reveal")).toBeInTheDocument();
  });
});

describe("latency budget", () => {
  it("shows the latency budget panel in Settings", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Latency budget panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText(/glass-to-glass/i)).toBeInTheDocument();
  });

  it("shows six stage rows in the latency breakdown", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Latency budget panel");
    expect(within(panel).getByText(/Zoom capture/i)).toBeInTheDocument();
    expect(within(panel).getByText(/Encoder buffer/i)).toBeInTheDocument();
    expect(within(panel).getByText(/Protocol/i)).toBeInTheDocument();
  });
});

describe("participant spotlight", () => {
  it("shows the spotlight panel with a summary in the Sources tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Spotlight decision");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText(/Auto:|Pinned:/i)).toBeInTheDocument();
  });

  it("lists eligible participants with scores", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Spotlight decision");
    const rows = within(panel).getAllByText(/pts/i);
    expect(rows.length).toBeGreaterThanOrEqual(1);
  });
});

describe("caption quality", () => {
  it("shows the caption quality panel in the Overlays tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Caption quality panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText(/Avg confidence/i)).toBeInTheDocument();
    expect(within(panel).getByText(/Tier/i)).toBeInTheDocument();
  });

  it("shows quality summary text with confidence and sample count", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Caption quality panel");
    // The summary element (first match) should contain a tier label + confidence
    const summaries = within(panel).getAllByText(/Excellent|Good|Degraded|Poor|No caption/i);
    expect(summaries.length).toBeGreaterThanOrEqual(1);
  });
});

describe("scene intelligence", () => {
  it("shows the scene intelligence panel with a recommendation in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Scene intelligence panel");
    expect(panel).toBeInTheDocument();
    // Should show readout labels and a confidence value
    expect(within(panel).getAllByText(/Recommended/i).length).toBeGreaterThanOrEqual(1);
    expect(within(panel).getByText(/^high$|^medium$|^low$/i)).toBeInTheDocument();
  });

  it("shows the automation mode in the scene intelligence summary", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Scene intelligence panel");
    // Seed is set-and-forget mode → Auto prefix
    expect(within(panel).getByText(/^Auto:/i)).toBeInTheDocument();
  });
});

describe("ISO recording plan", () => {
  it("shows the ISO plan panel with a track summary in Settings > Recording", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    // Seed has p1 + p2 selected → 2 participants + program mix = 3 tracks
    const plan = screen.getByLabelText("ISO recording plan");
    expect(plan).toBeInTheDocument();
    expect(within(plan).getByText(/ISO track/i)).toBeInTheDocument();
    expect(within(plan).getByText(/Mbps/i)).toBeInTheDocument();
  });

  it("shows track file names in the ISO plan when feeds are selected", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const plan = screen.getByLabelText("ISO recording plan");
    // track files should show .mp4 filenames (ISO-NN-<SafeName>.mp4, see isoRecording.ts)
    const tracks = within(plan).getAllByText(/\.mp4$/i);
    expect(tracks.length).toBeGreaterThanOrEqual(1);
  });
});

describe("loudness normalisation", () => {
  it("shows the loudness panel with reading and target in the Audio tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");

    const panel = screen.getByLabelText("Loudness normalisation");
    const status = within(panel).getByLabelText("Loudness status");
    expect(status).toBeInTheDocument();
    // Shows LUFS reading and target
    expect(within(status).getAllByText(/LUFS/i).length).toBeGreaterThanOrEqual(1);
    expect(within(status).getByText(/target -\d+\.\d+ LUFS/i)).toBeInTheDocument();
  });

  it("shows the loudness target selector with at least 4 options", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");

    const panel = screen.getByLabelText("Loudness normalisation");
    const select = within(panel).getByRole("combobox", { name: /Loudness target/i });
    expect(select).toBeInTheDocument();
    expect(select.querySelectorAll("option").length).toBeGreaterThanOrEqual(4);
  });
});

describe("show clock", () => {
  it("shows the show clock display with 00:00 elapsed in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Show clock");
    const display = within(panel).getByLabelText("Show clock display");
    expect(display).toHaveTextContent("00:00");
    expect(within(panel).getByRole("button", { name: /Reset/i })).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /Next segment/i })).toBeInTheDocument();
  });

  it("advances to next segment when clicked", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");
    const panel = screen.getByLabelText("Show clock");

    // Click the first segment to start it
    const [firstSeg] = within(panel).getAllByRole("button", { name: /segment/i });
    await user.click(firstSeg);

    // Now advance to next
    await user.click(within(panel).getByRole("button", { name: /Next segment/i }));

    // Running segment should now be the second one
    const segs = within(panel).getAllByRole("button", { name: /segment/i });
    expect(segs.length).toBeGreaterThan(1);
  });
});

describe("pre-show countdown", () => {
  it("shows the T-minus label and phase in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Pre-show countdown panel");
    const tMinus = within(panel).getByLabelText("T-minus label");
    expect(tMinus).toBeInTheDocument();
    expect(tMinus.textContent).toMatch(/^T[+-]/);
  });

  it("shows all three pre-show cue milestones", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const cues = screen.getByLabelText("Pre-show cues");
    expect(within(cues).getByText("Lobby opens")).toBeInTheDocument();
    expect(within(cues).getByText("Hot zone")).toBeInTheDocument();
    expect(within(cues).getByText("Go live")).toBeInTheDocument();
  });
});

describe("tally lights", () => {
  it("shows the tally panel with participant rows in the Sources tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Tally panel");
    expect(panel).toBeInTheDocument();
    const rows = within(panel).getAllByRole("generic", { hidden: false }).filter(
      (el) => el.getAttribute("aria-label")?.includes("tally")
    );
    expect(rows.length).toBeGreaterThan(0);
  });

  it("shows on-air, in-preview, and idle counts in the tally panel", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Tally panel");
    expect(within(panel).getByText("On air")).toBeInTheDocument();
    expect(within(panel).getByText("In preview")).toBeInTheDocument();
    expect(within(panel).getByText("Idle")).toBeInTheDocument();
  });
});

describe("clip trimmer", () => {
  it("shows the clip trim panel with In/Out timecode readouts in the Media tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");

    const panel = screen.getByLabelText("Clip trim controls");
    expect(within(panel).getByText("In")).toBeInTheDocument();
    expect(within(panel).getByText("Out")).toBeInTheDocument();
    expect(within(panel).getByText("Duration")).toBeInTheDocument();
  });

  it("updates in point when trim In → button is clicked", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");

    const panel = screen.getByLabelText("Clip trim controls");
    const summaryBefore = within(panel).getByRole("paragraph", { hidden: true })?.textContent ?? panel.querySelector(".clip-trim-summary")?.textContent ?? "";
    await user.click(within(panel).getByRole("button", { name: /In →/i }));
    const summaryAfter = panel.querySelector(".clip-trim-summary")?.textContent ?? "";
    expect(summaryAfter).not.toBe(summaryBefore);
  });
});

describe("chapter markers", () => {
  it("shows the chapter panel with summary and mark button in the Media tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");

    const panel = screen.getByLabelText("Chapter panel");
    expect(within(panel).getByText("No chapters")).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /Mark chapter now/i })).toBeInTheDocument();
  });

  it("adds a chapter when the mark button is clicked and shows export format selector", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Media");

    const panel = screen.getByLabelText("Chapter panel");
    await user.click(within(panel).getByRole("button", { name: /Mark chapter now/i }));

    expect(within(panel).queryByText("No chapters")).not.toBeInTheDocument();
    expect(within(panel).getByLabelText("Chapter export format")).toBeInTheDocument();
  });
});

describe("speaker timer", () => {
  it("shows the speaker timer panel in the Sources tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Speaker timer panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText("Balance")).toBeInTheDocument();
  });

  it("shows a speaker row for each participant", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Sources");

    const panel = screen.getByLabelText("Speaker timer panel");
    const rows = within(panel).getAllByRole("generic").filter(
      (el) => el.getAttribute("aria-label")?.includes("speaker time")
    );
    expect(rows.length).toBeGreaterThan(0);
  });
});

describe("output watermark", () => {
  it("shows the watermark panel with mode selector in the Overlays tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Watermark panel");
    expect(within(panel).getByLabelText("Watermark mode")).toBeInTheDocument();
    expect(within(panel).getByLabelText("Watermark text")).toBeInTheDocument();
  });

  it("shows classification banner when enabled and mode is live with text", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Watermark panel");
    // Enable live mode
    await user.selectOptions(within(panel).getByLabelText("Watermark mode"), "live");
    await user.type(within(panel).getByLabelText("Watermark text"), "DRAFT");
    // Enable classification
    await user.click(within(panel).getByLabelText("Show classification banner"));

    expect(within(panel).getAllByLabelText("Classification banner").length).toBeGreaterThanOrEqual(1);
  });
});

describe("network diagnostics", () => {
  it("shows the network diagnostics panel in the Settings tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Network diagnostics panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByText("Quality")).toBeInTheDocument();
    expect(within(panel).getByText("Avg RTT")).toBeInTheDocument();
  });

  it("shows quality score and loss readouts", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    const panel = screen.getByLabelText("Network diagnostics panel");
    expect(within(panel).getByText("Score")).toBeInTheDocument();
    expect(within(panel).getByText("Loss")).toBeInTheDocument();
  });
});

describe("graphic animator", () => {
  it("shows the animator panel with preset selector in the Overlays tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Animator panel");
    expect(within(panel).getByLabelText("Animation preset")).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /Play/i })).toBeInTheDocument();
  });

  it("shows phase and opacity readouts", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Overlays");

    const panel = screen.getByLabelText("Animator panel");
    expect(within(panel).getByText("Phase")).toBeInTheDocument();
    expect(within(panel).getByText("Opacity")).toBeInTheDocument();
  });
});

describe("poll engine", () => {
  it("shows the audience poll panel with Open Poll button in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Poll panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /Open Poll/i })).toBeInTheDocument();
    expect(within(panel).getByLabelText("Poll options")).toBeInTheDocument();
  });

  it("transitions poll to open and shows Sim Vote button enabled", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Poll panel");
    const openBtn = within(panel).getByRole("button", { name: /Open Poll/i });
    expect(openBtn).not.toBeDisabled();

    await user.click(openBtn);

    expect(within(panel).getAllByText("open").length).toBeGreaterThanOrEqual(1);
    expect(within(panel).getByRole("button", { name: /Sim Vote/i })).not.toBeDisabled();
  });
});

describe("cue sheet", () => {
  it("shows the cue sheet panel with cue list in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Cue sheet panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByLabelText("Cue list")).toBeInTheDocument();
    expect(within(panel).getAllByRole("button", { name: /Live/i }).length).toBeGreaterThanOrEqual(1);
  });

  it("transitions a cue to live when Live button is clicked", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Cue sheet panel");
    const liveButtons = within(panel).getAllByRole("button", { name: /Live/i });
    await user.click(liveButtons[0]);

    const liveBadges = within(panel).getAllByText("live");
    expect(liveBadges.length).toBeGreaterThanOrEqual(1);
  });
});

describe("audience Q&A", () => {
  it("shows the Q&A panel with questions in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Q&A panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByLabelText("Q&A questions")).toBeInTheDocument();
    expect(within(panel).getAllByRole("button", { name: /Approve/i }).length).toBeGreaterThanOrEqual(1);
  });

  it("approves a pending question and reveals it as ready", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Q&A panel");
    const approveButtons = within(panel).getAllByRole("button", { name: /Approve/i });
    await user.click(approveButtons[0]);

    expect(within(panel).getAllByText("approved").length).toBeGreaterThanOrEqual(1);
  });
});

describe("teleprompter", () => {
  it("shows the teleprompter panel with the script in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Teleprompter panel");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByLabelText("Teleprompter script")).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /^Start$/ })).toBeInTheDocument();
  });

  it("starts scrolling when Start is clicked", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Teleprompter panel");
    const startBtn = within(panel).getByRole("button", { name: /^Start$/ });
    expect(startBtn).not.toBeDisabled();

    await user.click(startBtn);

    expect(within(panel).getAllByText("scrolling").length).toBeGreaterThanOrEqual(1);
    expect(within(panel).getByRole("button", { name: /^Pause$/ })).not.toBeDisabled();
  });
});

describe("lower thirds", () => {
  it("shows the lower-third deck with a queue in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Lower third deck");
    expect(panel).toBeInTheDocument();
    expect(within(panel).getByLabelText("Plate queue")).toBeInTheDocument();
    expect(within(panel).getByRole("button", { name: /Show Next/i })).toBeInTheDocument();
  });

  it("puts the next plate on air when Show Next is clicked", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("Lower third deck");
    await user.click(within(panel).getByRole("button", { name: /Show Next/i }));

    // The first queued plate comes from the live Zoom roster (active speaker first).
    expect(within(panel).getAllByText("David Chen").length).toBeGreaterThanOrEqual(1);
    expect(within(panel).getByRole("button", { name: /Take Down/i })).not.toBeDisabled();
  });
});

describe("news ticker", () => {
  it("shows the ticker panel with seeded items in the Automation tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("News ticker");
    expect(panel).toBeInTheDocument();
    const list = within(panel).getByLabelText("Ticker items");
    expect(list).toBeInTheDocument();
    expect(within(list).getAllByRole("button", { name: /Remove ticker item/i }).length).toBeGreaterThanOrEqual(1);
  });

  it("adds a new item via the text input and Add button", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const panel = screen.getByLabelText("News ticker");
    const input = within(panel).getByLabelText("New ticker text");
    fireEvent.change(input, { target: { value: "Special update" } });
    await user.click(within(panel).getByRole("button", { name: /^Add$/i }));

    const list = within(panel).getByLabelText("Ticker items");
    expect(within(list).getByText("Special update")).toBeInTheDocument();
  });
});

describe("Zoom SDK pre-flight", () => {
  it("shows a compact SDK status chip in the Settings tab", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");

    expect(screen.getByLabelText("SDK status")).toBeInTheDocument();
  });

  it("allows Zoom join when native media runtime is ready (vendored engine path)", async () => {
    const user = userEvent.setup();
    const nativeRuntime: RuntimeEnvironment = {
      status: "ready",
      label: "Native media ready",
      host: "native-shell",
      platform: "win32",
      warnings: [],
      capabilities: ["zoom-raw-video"]
    };
    renderApp(createMockEngineBundle(), nativeRuntime);
    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Leave" }));
    const joinBtn = screen.getByRole("button", { name: /Join Zoom/i });
    expect(joinBtn).not.toBeDisabled();
  });

  it("allows simulated Zoom join in mock runtime when SDK readiness is blocked", async () => {
    const user = userEvent.setup();
    renderApp();
    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Leave" }));
    const joinBtn = screen.getByRole("button", { name: /Join Zoom/i });
    expect(joinBtn).not.toBeDisabled();
    await user.click(joinBtn);
    expect(screen.getByText(/in meeting - 7 participants/i)).toBeInTheDocument();
  });
});

describe("supervisor health recovery", () => {
  it("describeRuntimeEnvironment returns degraded when health shows core is recovering", async () => {
    const { describeRuntimeEnvironment } = await import("./engine/runtimeEnvironment");
    const mockBridge = {
      host: "native-shell" as const,
      platform: "win32" as const,
      request: async () => ({ id: "x", ok: false as const, error: { code: "protocol-error" as const, message: "stub" } })
    };
    const runtime = describeRuntimeEnvironment(
      mockBridge,
      undefined,
      { restartCount: 2, recovering: true, stopped: false }
    );
    expect(runtime.status).toBe("degraded");
    expect(runtime.label).toMatch(/recovering/i);
    expect(runtime.warnings[0]).toMatch(/crashed/i);
  });
});

describe("A4 capability-gated outputs", () => {
  it("disables SRT arming when srt-output is absent from capabilities", async () => {
    const user = userEvent.setup();
    // Runtime with NDI/WebRTC but NOT srt-output — matches the gate test scenario.
    const capRuntime: RuntimeEnvironment = {
      ...mockRuntime,
      status: "ready",
      label: "Native media ready",
      capabilities: ["ndi-output", "webrtc-output", "rtmp-output", "program-recording", "iso-recording",
        "zoom-raw-video", "zoom-raw-audio", "gpu-compositor", "scene-graph-rendering",
        "dynamic-overlays", "chroma-key", "smart-framing", "audio-mixer",
        "local-audio-capture", "audio-monitor-output"]
    };
    renderApp(createMockEngineBundle(), capRuntime);

    await goToTab(user, "Settings");
    // NDI arming button should be enabled (capability present).
    const ndiBtn = screen.getByRole("button", { name: /NDI Program/i });
    expect(ndiBtn).not.toBeDisabled();
    // SRT arming button should be disabled (capability absent).
    const srtBtn = screen.getByRole("button", { name: /SRT Backup/i });
    expect(srtBtn).toBeDisabled();
  });

  it("shows virtual camera section in mock mode (capabilities empty = allow-all)", async () => {
    const user = userEvent.setup();
    renderApp(); // mock mode: capabilities = []
    await goToTab(user, "Settings");
    expect(screen.getByLabelText("Virtual camera")).toBeInTheDocument();
  });

  it("hides virtual camera section when virtual-camera capability is absent", async () => {
    const user = userEvent.setup();
    const capRuntime: RuntimeEnvironment = {
      ...mockRuntime,
      status: "ready",
      label: "Native media ready",
      capabilities: ["rtmp-output", "ndi-output"] // no virtual-camera
    };
    renderApp(createMockEngineBundle(), capRuntime);
    await goToTab(user, "Settings");
    expect(screen.queryByLabelText("Virtual camera")).not.toBeInTheDocument();
  });
});

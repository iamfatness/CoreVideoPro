import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { App } from "./App";
import { createMockEngineBundle, type EngineBundle } from "./engine/engineBundle";
import { mapCaptureSnapshot } from "./engine/captureSnapshotMapper";
import type { RuntimeEnvironment } from "./engine/runtimeEnvironment";

const mockRuntime: RuntimeEnvironment = {
  status: "mock",
  label: "Mock studio",
  host: "browser-preview",
  platform: "web",
  warnings: ["Running with simulated Zoom and output engines."]
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
    expect(nativeCore).toHaveTextContent("Sources8");
    expect(nativeCore).toHaveTextContent("Resolved routes2");
    expect(nativeCore).toHaveTextContent("Render plan3 layers");
    expect(nativeCore).toHaveTextContent(/Plan IDrp-/);
    expect(nativeCore).toHaveTextContent("Program frames3");
    expect(nativeCore).toHaveTextContent("Compositorlive");
    expect(nativeCore).toHaveTextContent("Outputsrecording");
    expect(nativeCore).toHaveTextContent("Profile1080p60");
    expect(nativeCore).toHaveTextContent("Disk rate7.49 MB/s");
    expect(nativeCore).toHaveTextContent("Output healthlive");
  });

  it("renders AI Studio show notes and highlight suggestions", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Automation");

    const notes = await screen.findByLabelText("AI Studio show notes");
    expect(within(notes).getByText("AI Product Launch Webinar show notes")).toBeInTheDocument();
    expect(within(notes).getByText(/Andre Wallace/i)).toBeInTheDocument();

    const highlights = screen.getByLabelText("AI Studio highlights");
    expect(within(highlights).getByText("Presenter plus slides moment")).toBeInTheDocument();
    expect(within(highlights).getByText("ai-product-launch-webinar-andre-wallace-speaker-slides")).toBeInTheDocument();
  });

  it("runs Magic Scene through the engine and updates status", async () => {
    const user = userEvent.setup();
    renderApp();

    await user.click(screen.getByRole("button", { name: /Magic Scene/i }));
    await goToTab(user, "Automation");

    expect(screen.getByText(/Magic Scene built 5 scenes/i)).toBeInTheDocument();
    expect(screen.getByText(/Priya Shah is low resolution/i)).toBeInTheDocument();
  });

  it("hydrates media frame telemetry into the program preview", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    expect(within(program).getByLabelText("Speaker slides layout")).toBeInTheDocument();
    expect((await within(program).findAllByText("Andre Wallace")).length).toBeGreaterThan(0);
    expect(within(program).getAllByText("1920x1080 30fps").length).toBeGreaterThan(0);
    expect(within(program).getByText(/Andre Wallace screen share - 1920x1080 - 75ms/i)).toBeInTheDocument();
    expect(within(program).getAllByText(/crop \d+% \/ \d+ms/i).length).toBeGreaterThan(0);
    expect(within(program).getAllByText("Andre Wallace").length).toBeGreaterThan(0);
    expect(within(program).getByText(/Lower-third lifted to protect slide captions/i)).toBeInTheDocument();
    expect(within(program).getByText("CoreVideo Pro")).toBeInTheDocument();

    await goToTab(user, "Settings");
    expect(screen.getByText("Caption confidence")).toBeInTheDocument();
    expect(screen.getByText("Overlay guard")).toBeInTheDocument();
    expect(within(program).queryByText("Waiting for frame")).not.toBeInTheDocument();
  });

  it("toggles graphics library overlays in the program preview", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");

    expect(await within(program).findByText("CoreVideo Pro")).toBeInTheDocument();
    expect(within(program).queryByText("Live webinar")).not.toBeInTheDocument();

    await goToTab(user, "Overlays");
    await user.click(screen.getByRole("button", { name: /Live banner/i }));
    expect(within(program).getByText("Live webinar")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Question CTA/i }));
    expect(within(program).getByText("Submit questions in chat")).toBeInTheDocument();
  });

  it("edits the brand kit and applies it to the brand bug graphic", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    expect(await within(program).findByText("CoreVideo Pro")).toBeInTheDocument();

    await goToTab(user, "Overlays");

    const logoInput = screen.getByLabelText("Brand logo text");
    await user.clear(logoInput);
    await user.type(logoInput, "Acme Live");

    await user.click(screen.getByRole("button", { name: /Apply brand kit to graphics/i }));

    expect(within(program).getByText("Acme Live")).toBeInTheDocument();
    expect(within(program).queryByText("CoreVideo Pro")).not.toBeInTheDocument();
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

    expect(within(transcript).getByText("Welcome to the AI Product Launch Webinar.")).toBeInTheDocument();
    expect(within(transcript).getByText("Maya Chen")).toBeInTheDocument();
    expect(within(transcript).getByText(/Host - 96%/)).toBeInTheDocument();
    expect(within(transcript).getAllByText("Andre Wallace").length).toBeGreaterThan(0);
  });

  it("saves and reloads a show preset", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Save Show" }));
    expect(await screen.findByText("AI Product Launch Webinar Show saved")).toBeInTheDocument();

    await goToTab(user, "Overlays");
    await user.click(screen.getByRole("button", { name: /Live banner/i }));
    expect(within(program).getByText("Live webinar")).toBeInTheDocument();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: /AI Product Launch Webinar Show/i }));
    await screen.findAllByText("AI Product Launch Webinar Show loaded");
    expect(within(program).queryByText("Live webinar")).not.toBeInTheDocument();
  });

  it("exports a support bundle with production triage lines", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Export Bundle" }));

    expect((await screen.findAllByText(/support-ai-product-launch-webinar/i)).length).toBeGreaterThan(0);
    const summary = screen.getByLabelText("Support bundle summary");
    expect(within(summary).getByText("Show: AI Product Launch Webinar (set-and-forget)")).toBeInTheDocument();
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
    expect(await screen.findByText(/00:08/i)).toBeInTheDocument();
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
    expect(filename).toHaveValue("AI_Product_Launch_Webinarr");
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

    expect(screen.getByText("Priya Shah + Maya Chen")).toBeInTheDocument();
    expect(screen.getByText("Interview route 1 updated")).toBeInTheDocument();
    expect(within(program).queryByText("Priya Shah")).not.toBeInTheDocument();
    expect(within(screen.getByLabelText("Preview monitor")).getByText("Priya Shah")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Take" }));

    expect(within(program).getByLabelText("Interview layout")).toBeInTheDocument();
    expect(within(program).getByText("Priya Shah")).toBeInTheDocument();
  });

  it("routes preview scene slots by active speaker with audio role metadata", async () => {
    const user = userEvent.setup();
    renderApp();

    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.click(screen.getByRole("button", { name: /Preview Monitor/i }));
    await user.selectOptions(screen.getByLabelText("Slot 1 route mode"), "active-speaker");
    await user.selectOptions(screen.getByLabelText("Slot 1 audio role"), "mix");

    expect(screen.getByText("Andre Wallace + Maya Chen")).toBeInTheDocument();
    expect(screen.getByText("Interview route 1 updated")).toBeInTheDocument();
    expect(within(screen.getByLabelText("Preview monitor")).getByText("Andre Wallace")).toBeInTheDocument();
  });

  it("warns operators about duplicate fixed routes and duplicated isolated audio", async () => {
    const user = userEvent.setup();
    renderApp();

    const scenes = screen.getByLabelText("Scenes");

    await user.click(within(scenes).getByRole("button", { name: /Interview/i }));
    await user.selectOptions(screen.getByLabelText("Slot 1 participant"), "p2");
    await user.selectOptions(screen.getByLabelText("Slot 2 participant"), "p2");

    const warnings = screen.getByLabelText("Route warnings");
    expect(within(warnings).getByText("Andre Wallace is assigned to multiple fixed routes.")).toBeInTheDocument();
    expect(within(warnings).getByText("Andre Wallace has duplicated isolated audio.")).toBeInTheDocument();
    expect(screen.getByText("Route health")).toBeInTheDocument();
  });

  it("warns when a scene expects screen share but no share is available", async () => {
    const user = userEvent.setup();
    renderApp();
    await screen.findByText(/in meeting - 8 participants/i);
    await user.click(screen.getByRole("button", { name: /Set & Forget/i }));

    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });
    for (let index = 0; index < 4 && screen.queryAllByText("Slot 2 screen share is unavailable.").length === 0; index += 1) {
      await user.click(refresh);
      await waitFor(() => expect(screen.getByText(/in meeting - 8 participants/i)).toBeInTheDocument());
    }

    expect(screen.getAllByText("Slot 2 screen share is unavailable.").length).toBeGreaterThan(0);
  });

  it("auto-takes recommended scenes in Set & Forget mode", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });

    for (let index = 0; index < 4 && !within(program).queryByLabelText("Smart panel grid"); index += 1) {
      await user.click(refresh);
    }

    expect(within(program).getByLabelText("Smart panel grid")).toBeInTheDocument();
    expect(screen.getByText(/Set & Forget took Panel/i)).toBeInTheDocument();
    expect(screen.getByText("Auto director")).toBeInTheDocument();
    expect(screen.getAllByText(/take 90%/i).length).toBeGreaterThan(0);
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
    expect(screen.getByText(/Recordings\/CoreVideo Pro\/AI_Product_Launch_Webinar.mp4/i)).toBeInTheDocument();
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
    expect(within(bin).getByText("3 assets - 1 stinger, 1 lower-third, 1 audio bed")).toBeInTheDocument();

    await user.selectOptions(within(bin).getByLabelText("Media asset type"), "slate");
    await user.click(within(bin).getByRole("button", { name: /Add asset/i }));
    expect(within(bin).getByText("Slate 1")).toBeInTheDocument();
    expect(within(bin).getByText("4 assets - 1 stinger, 1 lower-third, 1 audio bed, 1 slate")).toBeInTheDocument();

    await user.click(within(bin).getByRole("button", { name: "Remove Brand stinger" }));
    expect(within(bin).queryByText("Brand stinger")).not.toBeInTheDocument();
    expect(within(bin).getByText("3 assets - 1 lower-third, 1 audio bed, 1 slate")).toBeInTheDocument();
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
    await user.click(screen.getByLabelText("Maya Chen ISO recording"));
    await user.click(screen.getByLabelText("Noah Kim ISO recording"));
    await user.click(screen.getByRole("button", { name: "Record" }));

    const session = await engines.output.getSession();
    expect(session.recording).toBe(true);
    expect(session.recordingFile).toBe("Recordings/Client Shows/Customer_Panel.mkv");
    expect(session.recordingSettings).toMatchObject({
      format: "mkv",
      quality: "archive",
      isoParticipantIds: ["p2", "p4"]
    });
    expect(screen.getByLabelText("Recording folder")).toBeDisabled();
    expect(screen.getByLabelText("Noah Kim ISO recording")).toBeDisabled();
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
    await user.click(screen.getByRole("button", { name: /Noah Kim/i }));
    await user.selectOptions(screen.getByLabelText("Production role"), "Host");

    expect(screen.getByText("Noah Kim set as Host for scene automation")).toBeInTheDocument();
    expect(screen.getByText("Noah Kim role set to Host")).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Magic Scene/i }));

    expect(await screen.findByText(/Host open with Noah Kim/i)).toBeInTheDocument();
    await goToTab(user, "Studio");
    expect(within(screen.getByText("Zoom participants").closest("aside") as HTMLElement).getByRole("button", { name: /Noah Kim/i })).toHaveTextContent("Host");
  });

  it("preserves producer role overrides across Zoom feed refreshes", async () => {
    const user = userEvent.setup();
    renderApp();

    await goToTab(user, "Audio");
    await user.click(screen.getByRole("button", { name: /Noah Kim/i }));
    await user.selectOptions(screen.getByLabelText("Production role"), "Host");
    await goToTab(user, "Settings");
    await user.click(screen.getByRole("button", { name: /Refresh feeds/i }));

    expect(await screen.findByText(/00:08/i)).toBeInTheDocument();
    await goToTab(user, "Audio");
    expect(screen.getByLabelText("Production role")).toHaveValue("Host");
    await goToTab(user, "Studio");
    expect(within(screen.getByText("Zoom participants").closest("aside") as HTMLElement).getByRole("button", { name: /Noah Kim/i })).toHaveTextContent("Host");
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

    await goToTab(user, "Audio");
    await user.click(screen.getByRole("button", { name: /Priya Shah/i }));
    await goToTab(user, "Media");

    const lowQualityPanel = screen.getByLabelText("Face-aware auto-crop");
    expect(within(lowQualityPanel).getByText("low")).toBeInTheDocument();
    expect(within(lowQualityPanel).getByRole("status")).toHaveTextContent(/Low-quality feed for Priya Shah/i);
  });

  it("filters Zoom participants by breakout room", async () => {
    const user = userEvent.setup();
    renderApp();

    const program = screen.getByLabelText("Program preview");
    const breakouts = await screen.findByLabelText("Breakout rooms");

    expect(within(breakouts).getByRole("button", { name: /Customer panel/i })).toBeInTheDocument();
    await user.click(within(breakouts).getByRole("button", { name: /Customer panel/i }));

    expect(screen.getAllByText(/Customer panel/i).length).toBeGreaterThan(0);
    expect(screen.getAllByText(/Priya Shah/i).length).toBeGreaterThan(0);
    expect(screen.getAllByText(/Noah Kim/i).length).toBeGreaterThan(0);
    expect(within(screen.getByText("Zoom participants").closest("aside") as HTMLElement).queryByText(/Maya Chen/i)).not.toBeInTheDocument();
    expect(within(program).queryByText(/Andre Wallace screen share/i)).not.toBeInTheDocument();

    await user.click(within(breakouts).getByRole("button", { name: /All rooms/i }));
    expect(screen.getAllByText(/Maya Chen/i).length).toBeGreaterThan(0);
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
    expect(screen.getByText(/in meeting - 8 participants/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /Refresh feeds/i }));
    expect(screen.getByText(/00:08/i)).toBeInTheDocument();
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
    await user.clear(screen.getByLabelText("Zoom meeting URL or ID"));
    await user.type(screen.getByLabelText("Zoom meeting URL or ID"), "https://zoom.us/j/987654321");
    await user.clear(screen.getByLabelText("Producer display name"));
    await user.type(screen.getByLabelText("Producer display name"), "Guest Producer");
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
    await screen.findByText(/in meeting - 8 participants/i);
    await user.click(screen.getByRole("button", { name: /Set & Forget/i }));

    await goToTab(user, "Settings");
    const refresh = screen.getByRole("button", { name: /Refresh feeds/i });
    for (let index = 0; index < 4 && !screen.queryByText(/reserve this region/i); index += 1) {
      await user.click(refresh);
      await waitFor(() => expect(screen.getByText(/in meeting - 8 participants/i)).toBeInTheDocument());
    }

    expect(screen.getAllByText(/No screen share/i).length).toBeGreaterThanOrEqual(2);
    expect(screen.getByText(/reserve this region/i)).toBeInTheDocument();
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
    renderApp();

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

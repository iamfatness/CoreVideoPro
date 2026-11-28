#include <cmath>
#include <functional>
#include <memory>
#include "graphics/GraphicsCrawl.h"
#include "GraphicsMotionFixture.h"
#include "gtest/gtest.h"

using namespace corevideo::graphics;

TEST(GraphicsMotion, EasingMatchesTheWholeGraphicTimeline) {
  EXPECT_EQ(.75, motionEase("ease-out", .5));
  EXPECT_EQ(.25, motionEase("ease-in", .5));
  EXPECT_EQ(.5, motionEase("ease-in-out", .5));
  EXPECT_EQ(.3, motionEase("linear", .3));
}

TEST(GraphicsMotion, PreviewPlayModesMapToPhases) {
  using P = GraphicsMotionPhase;
  const auto phase = [](const char* mode, double ms) {
    return previewMotion(mode, ms, 500, 500, {}, {});
  };
  EXPECT_EQ(P::In, phase("in", 100).phase);
  EXPECT_EQ(P::Settled, phase("in", 600).phase);
  EXPECT_EQ(P::Out, phase("out", 100).phase);
  EXPECT_EQ(P::Hidden, phase("out", 700).phase);
  EXPECT_EQ(P::Settled, phase("out", 1001).phase);
  EXPECT_EQ(100., phase("cycle", 1600).elapsedMs);
  EXPECT_EQ(P::Hidden, phase("cycle", 2050).phase);
  EXPECT_EQ(P::Settled, phase("cycle", 2600).phase);
  EXPECT_EQ(P::In, phase("loop", 2600).phase);
  EXPECT_EQ(100., phase("loop", 2600).elapsedMs);
}

namespace {
using Phase = GraphicsMotionPhase;
using corevideo::graphics::test::motionDrawing;
GraphicsMotionState state(Phase phase, double ms) {
  return {phase, ms, {"fade", "left", "ease-out", 250}, {"fade", "left", "ease-in", 200}};
}
}

TEST(GraphicsMotion, BarFirstSamplesAtGivenMilliseconds) {
  const auto d = motionDrawing("bar-first.json");
  const auto plate = sampleRun(d, 0, state(Phase::In, 100));
  ASSERT_TRUE(plate.clip);
  EXPECT_NEAR(519.1836734693877, plate.clip->width, 1e-9);
  EXPECT_EQ(168., plate.clip->height);
  EXPECT_EQ(0., sampleRun(d, 1, state(Phase::In, 100)).opacity);
  EXPECT_NEAR(.64, sampleRun(d, -1, state(Phase::In, 100)).opacity, 1e-12);
  const auto name = sampleRun(d, 1, state(Phase::In, 350));
  EXPECT_NEAR(.75, name.opacity, 1e-12);
  EXPECT_NEAR(6., name.transform.ty, 1e-12);
}

TEST(GraphicsMotion, MirroredOutIsTheExactFrameReverseOfIn) {
  const auto d = motionDrawing("bar-first.json");
  for (int ms = 0; ms <= 500; ms += 10) {
    for (int unit : {0, 1}) {
      const auto in = sampleRun(d, unit, state(Phase::In, ms));
      const auto out = sampleRun(d, unit, state(Phase::Out, 500 - ms));
      EXPECT_NEAR(in.opacity, out.opacity, 1e-9);
      EXPECT_NEAR(in.transform.ty, out.transform.ty, 1e-9);
      ASSERT_TRUE(in.clip && out.clip);
      EXPECT_NEAR(in.clip->width, out.clip->width, 1e-9);
    }
  }
}

TEST(GraphicsMotion, GroupMotionComposesWithItsChildAndMovesItsMask) {
  const auto d = motionDrawing("nested-motion.json");
  const auto text = sampleRun(d, 1, state(Phase::In, 200));
  EXPECT_NEAR(-100., text.transform.tx, 1e-9);
  EXPECT_NEAR(12., text.transform.ty, 1e-9);
  EXPECT_NEAR(.5, text.opacity, 1e-12);
  ASSERT_TRUE(text.clip);
  EXPECT_NEAR(0., text.clip->x, 1e-9);
  EXPECT_NEAR(20., text.clip->y, 1e-9);
  EXPECT_NEAR(600., text.clip->width, 1e-9);
  const auto bar = sampleRun(d, 2, state(Phase::In, 200));
  ASSERT_TRUE(bar.clip);
  EXPECT_NEAR(0., bar.clip->x, 1e-9);
  EXPECT_NEAR(120., bar.clip->y, 1e-9);
  EXPECT_NEAR(300., bar.clip->width, 1e-9);
  EXPECT_NEAR(20., bar.clip->height, 1e-9);
}

TEST(GraphicsMotion, PopOvershootsAboutTheCentreAndGrowScalesFromItsEdge) {
  const GraphicsBox box{100, 50, 200, 100};
  GraphicsLayerMotion motion;
  motion.type = "pop";
  motion.durationMs = 1000;
  motion.easing = "linear";
  double peak = 0;
  for (int ms = 0; ms <= 1000; ++ms) {
    peak = std::max(peak, sampleUnit(motion, box, {}, Phase::In, ms).transform.sx);
  }
  EXPECT_NEAR(1.05, peak, 1e-4);
  EXPECT_EQ(1., sampleUnit(motion, box, {}, Phase::In, 1000).transform.sx);
  const auto pop = sampleUnit(motion, box, {}, Phase::In, 500).transform.apply(box);
  EXPECT_NEAR(200., pop.x + pop.width / 2, 1e-9);
  motion.type = "grow";
  motion.direction = "down";
  const auto grow = sampleUnit(motion, box, {}, Phase::In, 500).transform.apply(box);
  EXPECT_NEAR(50., grow.height, 1e-9);
  EXPECT_NEAR(150., grow.y + grow.height, 1e-9);
}

TEST(GraphicsMotion, RunsFollowPaintOrder) {
  const auto runs = planRuns(motionDrawing("bar-first.json"));
  ASSERT_EQ(4u, runs.size());
  EXPECT_EQ(0, runs[0].unit);
  EXPECT_EQ(-1, runs[1].unit);
  EXPECT_EQ(1, runs[2].unit);
  EXPECT_EQ(-1, runs[3].unit);
}

TEST(GraphicsMotion, AtlasStacksSlicesWithAGutterAndRefusesOversize) {
  const std::vector<GraphicsRun> runs{{0, 0, 1, {0, 0, 100.4, 50}}, {1, 1, 1, {10, 60, 40, 20}}};
  std::vector<GraphicsAtlasSlice> slices;
  int width = 0, height = 0;
  ASSERT_TRUE(packAtlas(runs, 1., slices, width, height));
  EXPECT_EQ(101, width);
  EXPECT_EQ(72, height);
  ASSERT_EQ(2u, slices.size());
  EXPECT_EQ(52, slices[1].y);
  EXPECT_EQ(40, slices[1].width);
  const std::vector<GraphicsRun> tall(9, GraphicsRun{0, 0, 1, {0, 0, 10, 2000}});
  EXPECT_FALSE(packAtlas(tall, 1., slices, width, height));
}

TEST(GraphicsCrawl, TravelKeepsOrderInBothDirections) {
  EXPECT_EQ(300., crawlItemX(300, 0, 40, 0, false));
  EXPECT_EQ(358., crawlItemX(300, 58, 25, 0, false));
  EXPECT_EQ(250., crawlItemX(300, 0, 40, 50, false));
  EXPECT_EQ(-40., crawlItemX(300, 0, 40, 0, true));
  EXPECT_EQ(-83., crawlItemX(300, 58, 25, 0, true));
  EXPECT_EQ(10., crawlItemX(300, 0, 40, 50, true));
}

TEST(GraphicsCrawl, LoopWrapsAtExactBoundaryAndAcrossSkippedTicks) {
  GraphicsCrawlClock clock;
  clock.restart(50);
  EXPECT_EQ(99., clock.sample(1040, 100, 100, true).distance);
  EXPECT_EQ(0., clock.sample(1050, 100, 100, true).distance);
  EXPECT_EQ(25., clock.sample(102300, 100, 100, true).distance);
  EXPECT_FALSE(clock.sample(102300, 100, 100, true).complete);
}

TEST(GraphicsCrawl, PauseResumeAndRestartAreAnchoredToTheInjectedTick) {
  GraphicsCrawlClock clock;
  clock.restart(100);
  clock.pause(400);
  clock.pause(500);
  EXPECT_EQ(30., clock.sample(1000, 100, 100, false).distance);
  clock.resume(1000);
  clock.resume(1050);
  EXPECT_EQ(99., clock.sample(1690, 100, 100, false).distance);
  EXPECT_TRUE(clock.sample(1700, 100, 100, false).complete);
  EXPECT_EQ(100., clock.sample(6000, 100, 100, false).distance);
  clock.restart(6000);
  EXPECT_EQ(0., clock.sample(6000, 100, 100, false).distance);
  EXPECT_FALSE(clock.sample(6000, 100, 100, false).complete);
}

// ── Motion polish (graphics styles slice 8: P5 reveal, P6 stagger, P7 loops) ──

TEST(GraphicsMotion, ExpoAndQuintOutStartFastAndLandExactly) {
  EXPECT_EQ(0., motionEase("expo-out", 0));
  EXPECT_EQ(1., motionEase("expo-out", 1));
  EXPECT_NEAR(1 - std::pow(2., -5), motionEase("expo-out", .5), 1e-12);
  EXPECT_EQ(0., motionEase("quint-out", 0));
  EXPECT_EQ(1., motionEase("quint-out", 1));
  EXPECT_NEAR(1 - std::pow(.5, 5), motionEase("quint-out", .5), 1e-12);
}

TEST(GraphicsMotion, RevealLeadTravelsTheBoxPlusItsFeather) {
  const GraphicsBox box{100, 50, 400, 100};
  GraphicsLayerMotion motion;
  motion.type = "reveal";
  motion.durationMs = 1000;
  motion.easing = "linear";
  motion.feather = 100;
  // From left, halfway: the lead has travelled (400 + 100) / 2 = 250.
  auto half = sampleUnit(motion, box, {}, Phase::In, 500);
  ASSERT_TRUE(half.clip && half.edge);
  if (!(half.clip && half.edge)) return;
  EXPECT_EQ(100., half.clip->x);
  EXPECT_EQ(250., half.clip->width);
  EXPECT_EQ((GraphicsRevealEdge{false, 1, 350, 100}), *half.edge);
  // From right: anchored at the right edge, keeping what lies above the lead.
  motion.direction = "right";
  half = sampleUnit(motion, box, {}, Phase::In, 500);
  ASSERT_TRUE(half.clip && half.edge);
  if (!(half.clip && half.edge)) return;
  EXPECT_EQ(250., half.clip->x);
  EXPECT_EQ(250., half.clip->width);
  EXPECT_EQ((GraphicsRevealEdge{false, -1, 250, 100}), *half.edge);
  // From the top and from the bottom: the vertical axis.
  motion.direction = "up";
  half = sampleUnit(motion, box, {}, Phase::In, 500);
  ASSERT_TRUE(half.clip && half.edge);
  if (!(half.clip && half.edge)) return;
  EXPECT_EQ(100., half.clip->height);  // min(100, (100 + 100) / 2)
  EXPECT_EQ((GraphicsRevealEdge{true, 1, 150, 100}), *half.edge);
  motion.direction = "down";
  half = sampleUnit(motion, box, {}, Phase::In, 250);
  ASSERT_TRUE(half.clip && half.edge);
  if (!(half.clip && half.edge)) return;
  EXPECT_EQ(100., half.clip->y);
  EXPECT_EQ(50., half.clip->height);
  EXPECT_EQ((GraphicsRevealEdge{true, -1, 100, 100}), *half.edge);
  // Fully revealed: the whole box, no ramp left. Nothing before the start.
  motion.direction = "left";
  const auto done = sampleUnit(motion, box, {}, Phase::In, 1000);
  ASSERT_TRUE(done.clip);
  if (!done.clip) return;
  EXPECT_EQ(400., done.clip->width);
  EXPECT_FALSE(done.edge);
  EXPECT_EQ(0., sampleUnit(motion, box, {}, Phase::In, 0).clip->width);
  // Feather 0 is a hard edge, exactly the wipe.
  motion.feather = 0;
  EXPECT_FALSE(sampleUnit(motion, box, {}, Phase::In, 500).edge);
  auto wipe = motion;
  wipe.type = "wipe";
  EXPECT_EQ(sampleUnit(wipe, box, {}, Phase::In, 500).clip->width, sampleUnit(motion, box, {}, Phase::In, 500).clip->width);
  // The way out is the frame reverse of the way in.
  motion.feather = 100;
  for (int ms = 0; ms <= 1000; ms += 50) {
    const auto in = sampleUnit(motion, box, {}, Phase::In, ms), out = sampleUnit(motion, box, {}, Phase::Out, 1000 - ms);
    EXPECT_NEAR(in.clip->width, out.clip->width, 1e-9);
    EXPECT_EQ(in.edge.has_value(), out.edge.has_value());
    if (in.edge && out.edge) EXPECT_NEAR(in.edge->lead, out.edge->lead, 1e-9);
  }
}

TEST(GraphicsMotion, AChildRevealEdgeMovesWithItsParentTransform) {
  GraphicsDrawing d;
  d.width = 1000;
  d.height = 200;
  GraphicsLayerMotion grow;
  grow.type = "grow";
  grow.direction = "left";
  grow.durationMs = 1000;
  grow.easing = "linear";
  GraphicsLayerMotion reveal;
  reveal.type = "reveal";
  reveal.durationMs = 1000;
  reveal.easing = "linear";
  reveal.feather = 100;
  d.units.push_back({"group", -1, true, grow, {}, {0, 0, 1000, 200}, {0, 0, 1000, 200}});
  d.units.push_back({"plate", 0, false, reveal, {}, {0, 0, 400, 100}, {0, 0, 1000, 200}});
  const auto sample = sampleRun(d, 1, state(Phase::In, 500));
  ASSERT_TRUE(sample.edge);
  if (!sample.edge) return;
  // Local lead 250 and width 100, scaled by the parent's 0.5 grow.
  EXPECT_NEAR(125., sample.edge->lead, 1e-9);
  EXPECT_NEAR(50., sample.edge->width, 1e-9);
  // Settled and hidden carry no edge.
  EXPECT_FALSE(sampleRun(d, 1, state(Phase::Settled, 0)).edge);
  EXPECT_EQ(0., sampleRun(d, 1, state(Phase::Hidden, 0)).opacity);
}

TEST(GraphicsMotion, RevealEdgeConstantsAreTargetPixels) {
  float out[4] = {9, 9, 9, 9};
  // A 1000x200 layout drawn at x 0.1, width 0.5 of a 1920 px frame: 0.96 px per unit.
  revealEdgeConstants(GraphicsRevealEdge{false, 1, 250, 100}, 1000, 200, .1, .7, .5, .2, 1920, 1080, out);
  EXPECT_NEAR(1.f, out[0], 1e-4);
  EXPECT_NEAR(0.f, out[1], 1e-4);
  EXPECT_NEAR(192.f + 240.f, out[2], 1e-4);
  EXPECT_NEAR(1.f / 96.f, out[3], 1e-4);
  revealEdgeConstants(GraphicsRevealEdge{true, -1, 100, 50}, 1000, 200, .1, .7, .5, .2, 1920, 1080, out);
  EXPECT_NEAR(0.f, out[0], 1e-4);
  EXPECT_NEAR(-1.f, out[1], 1e-4);
  EXPECT_NEAR(-(756.f + 108.f), out[2], 1e-4);  // y 0.7 * 1080 + 100 * 1.08
  EXPECT_NEAR(1.f / 54.f, out[3], 1e-4);
  revealEdgeConstants(std::nullopt, 1000, 200, .1, .7, .5, .2, 1920, 1080, out);
  for (float value : out) EXPECT_EQ(0.f, value);
  revealEdgeConstants(GraphicsRevealEdge{false, 1, 250, 0}, 1000, 200, .1, .7, .5, .2, 1920, 1080, out);
  EXPECT_EQ(0.f, out[3]);
}

TEST(GraphicsMotion, LoopsSampleSweepPulseAndDriftByPeriod) {
  const GraphicsBox box{0, 0, 80, 168}, area{0, 0, 1060, 168};
  GraphicsLayerLoop sweep;
  sweep.type = "sweep";
  sweep.periodMs = 8000;
  sweep.durationMs = 1000;
  // Enters from the left: wholly before the area at 0, wholly past it at the end.
  auto at = sampleLoop(sweep, box, area, 0);
  EXPECT_EQ(-80., at.transform.tx);
  ASSERT_TRUE(at.clip);
  if (!at.clip) return;
  EXPECT_EQ(1060., at.clip->width);
  EXPECT_NEAR((-80. + 1060.) / 2, sampleLoop(sweep, box, area, 500).transform.tx, 1e-9);
  EXPECT_EQ(0., sampleLoop(sweep, box, area, 1000).opacity);  // waits out the period
  EXPECT_EQ(0., sampleLoop(sweep, box, area, 7999).opacity);
  EXPECT_EQ(-80., sampleLoop(sweep, box, area, 8000).transform.tx);  // and repeats
  sweep.direction = "right";
  EXPECT_EQ(1060., sampleLoop(sweep, box, area, 0).transform.tx);
  GraphicsLayerLoop pulse;
  pulse.type = "pulse";
  pulse.periodMs = 1200;
  EXPECT_NEAR(1., sampleLoop(pulse, box, area, 0).opacity, 1e-12);
  EXPECT_NEAR(.4, sampleLoop(pulse, box, area, 600).opacity, 1e-12);
  EXPECT_NEAR(1., sampleLoop(pulse, box, area, 1200).opacity, 1e-12);
  EXPECT_FALSE(sampleLoop(pulse, box, area, 300).clip);
  GraphicsLayerLoop drift;
  drift.type = "drift";
  drift.direction = "up";
  drift.periodMs = 4000;
  drift.distance = 6;
  EXPECT_NEAR(-6., sampleLoop(drift, box, area, 1000).transform.ty, 1e-9);
  EXPECT_NEAR(6., sampleLoop(drift, box, area, 3000).transform.ty, 1e-9);
  EXPECT_EQ(0., sampleLoop(drift, box, area, 1000).transform.tx);
}

TEST(GraphicsMotion, LoopsMoveSettledLayersAndOnlyThem) {
  const auto d = motionDrawing("polish.json");
  int shine = -1, accent = -1, plate = -1;
  for (size_t i = 0; i < d.units.size(); ++i) {
    if (d.units[i].nodeId == "shine") shine = int(i);
    if (d.units[i].nodeId == "accent") accent = int(i);
    if (d.units[i].nodeId == "plate") plate = int(i);
  }
  ASSERT_GE(shine, 0);
  ASSERT_GE(accent, 0);
  ASSERT_GE(plate, 0);
  if (shine < 0 || accent < 0 || plate < 0) return;
  EXPECT_TRUE(d.units[shine].root);
  auto settled = state(Phase::Settled, 0);
  settled.loopMs = 8500;  // inside the second period's sweep
  const auto moving = sampleRun(d, shine, settled);
  EXPECT_NEAR(-80 + (1060. + 80) * .5, moving.transform.tx, 1e-9);
  settled.loopMs = 600;
  EXPECT_NEAR(.4, sampleRun(d, accent, settled).opacity, 1e-12);
  const auto still = sampleRun(d, plate, settled);
  EXPECT_EQ(1., still.opacity);
  EXPECT_EQ(0., still.transform.tx);
  EXPECT_FALSE(still.clip);
  // A loop also runs while the graphic enters (it follows the graphic fade).
  auto entering = state(Phase::In, 125);
  entering.loopMs = 600;
  EXPECT_NEAR(.4 * .75, sampleRun(d, accent, entering).opacity, 1e-9);
}

TEST(GraphicsMotion, LayerClockCountsLoopTimeFromTheEntrance) {
  GraphicsLayerClock clock;
  clock.configure(400, 300);
  clock.show(1000);
  EXPECT_EQ(250., clock.state(1250).loopMs);
  EXPECT_EQ(GraphicsMotionPhase::Settled, clock.state(5000).phase);
  EXPECT_EQ(4000., clock.state(5000).loopMs);
  clock.hide(5000);
  EXPECT_EQ(4100., clock.state(5100).loopMs);
  clock.show(5100);  // reversing the exit keeps the loop phase
  EXPECT_EQ(4200., clock.state(5200).loopMs);
  clock.hide(6000);
  EXPECT_EQ(GraphicsMotionPhase::Hidden, clock.state(7000).phase);
  clock.show(9000);
  EXPECT_EQ(0., clock.state(9000).loopMs);
}

TEST(GraphicsMotion, StaggeredTextSplitsIntoTimedBandsOfTheSameText) {
  const auto d = motionDrawing("polish.json");
  std::vector<const GraphicsPrimitive*> name, title;
  for (const auto& p : d.primitives) {
    if (p.id == "name-label") name.push_back(&p);
    if (p.id == "title-label") title.push_back(&p);
  }
  // Three words, three pieces of the same primitive, each its own unit.
  ASSERT_EQ(3u, name.size());
  if (name.size() != 3) return;
  double left = name[0]->clip.x;
  for (size_t k = 0; k < name.size(); ++k) {
    EXPECT_EQ(name[0]->text, name[k]->text);
    EXPECT_EQ(name[0]->box.x, name[k]->box.x);
    EXPECT_EQ(name[0]->box.width, name[k]->box.width);
    EXPECT_NEAR(left, name[k]->clip.x, 1e-9) << "pieces tile the layer with no gap or overlap";
    left = name[k]->clip.x + name[k]->clip.width;
    const auto& unit = d.units[size_t(name[k]->unit)];
    ASSERT_TRUE(unit.in);
    EXPECT_EQ(200. + 60. * double(k), unit.in->startMs);
    EXPECT_EQ(name[k]->clip.x, unit.box.x);
    EXPECT_EQ(name[k]->clip.width, unit.box.width);
  }
  EXPECT_NEAR(32. + 996., left, 1e-9);
  EXPECT_NE(name[0]->unit, name[1]->unit);
  EXPECT_GT(name[1]->clip.x, 32.);
  // Two wrapped lines, banded at the line pitch (28 * 1.2).
  ASSERT_EQ(2u, title.size());
  if (title.size() != 2) return;
  EXPECT_EQ(80., title[0]->clip.y);
  EXPECT_NEAR(80. + 33.6, title[0]->clip.y + title[0]->clip.height, 1e-9);
  EXPECT_NEAR(80. + 33.6, title[1]->clip.y, 1e-9);
  EXPECT_NEAR(80. + 72., title[1]->clip.y + title[1]->clip.height, 1e-9);
  EXPECT_EQ(400., d.units[size_t(title[1]->unit)].in->startMs);
  // Each piece is its own run, so its own atlas slice.
  const auto runs = planRuns(d);
  size_t pieces = 0;
  for (const auto& run : runs)
    if (d.primitives[run.first].id == "name-label" || d.primitives[run.first].id == "title-label") ++pieces;
  EXPECT_EQ(5u, pieces);
  // Mid-entrance, the first word has risen further than the second.
  const auto first = sampleRun(d, name[0]->unit, state(Phase::In, 300));
  const auto second = sampleRun(d, name[1]->unit, state(Phase::In, 300));
  EXPECT_LT(first.transform.ty, second.transform.ty);
  EXPECT_GT(first.opacity, second.opacity);
}

TEST(GraphicsMotion, PlaceRunIsTheSharedRunGeometry) {
  const auto drawing = std::make_shared<GraphicsDrawing>(motionDrawing("polish.json"));
  GraphicsRunLayout layout;
  layout.width = drawing->width;
  layout.height = drawing->height;
  layout.runs = planRuns(*drawing);
  ASSERT_TRUE(packAtlas(layout.runs, 1., layout.slices, layout.atlasWidth, layout.atlasHeight));
  layout.drawing = drawing;
  GraphicsRunPlacement placement;
  // The plate reveals with a feather mid-entrance.
  auto entering = state(Phase::In, 100);
  ASSERT_TRUE(placeRun(layout, 0, entering, std::nullopt, placement));
  EXPECT_EQ("plate", drawing->primitives[layout.runs[0].first].id);
  ASSERT_TRUE(placement.mask && placement.edge);
  if (!(placement.mask && placement.edge)) return;
  EXPECT_EQ(48., placement.edge->width);
  EXPECT_NEAR(placement.mask->x + placement.mask->width, std::min(1060., placement.edge->lead), 1e-9);
  // Settled: every run in place, no mask, no edge, full opacity, except loops.
  auto settled = state(Phase::Settled, 0);
  settled.loopMs = 2000;  // the sweep waits out its period; the pulse is mid-dip (800 of 1200)
  for (size_t i = 0; i < layout.runs.size(); ++i) {
    const auto& id = drawing->primitives[layout.runs[i].first].id;
    const bool drawn = placeRun(layout, i, settled, std::nullopt, placement);
    if (id == "shine") { EXPECT_FALSE(drawn); continue; }
    ASSERT_TRUE(drawn) << id;
    EXPECT_FALSE(placement.edge) << id;
    EXPECT_EQ(layout.runs[i].source.x, placement.destination.x) << id;
    if (id == "accent") EXPECT_LT(placement.opacity, 1.) << id;
    else EXPECT_EQ(1., placement.opacity) << id;
  }
  EXPECT_FALSE(placeRun(layout, layout.runs.size(), settled, std::nullopt, placement));
  EXPECT_FALSE(placeRun(layout, 0, state(Phase::Hidden, 0), std::nullopt, placement));
}

TEST(GraphicsMotion, PolishSummaryBoundsStaggerAndFlagsLoops) {
  std::string error;
  const auto definition = GraphicsTemplate::load(corevideo::graphics::test::motionFixture("polish.json"), error);
  ASSERT_TRUE(definition) << error;
  if (!definition) return;
  const auto& summary = definition->motionSummary();
  EXPECT_TRUE(summary.layered);
  EXPECT_TRUE(summary.loops);
  EXPECT_EQ(5u, summary.units);
  // name: at most 12 word pieces (11 more units); title: 2 lines (1 more).
  EXPECT_EQ(12u, summary.staggerUnits);
  // name: 200 + 300 + 11 * 60 is the last possible piece's end.
  EXPECT_EQ(1160., summary.inMs);
  auto input = std::make_shared<GraphicsTemplateInput>();
  input->definition = definition;
  // Admission covers 2 * (5 + 12) + 1 runs of the raster height.
  const uint64_t height = 168, runs = 2 * (5 + 12) + 1;
  EXPECT_EQ(uint64_t(1 + 1060) * (runs * (height + 3) - 2) * 4, overlayAdmissionBytes(input.get(), 1060, 168));
}

TEST(GraphicsMotion, PolishValidationRefusesBadShapesWithExactReasons) {
  using J = corevideo::rpc::Json;
  using M = J::Object;
  const auto parsed = J::parse(corevideo::graphics::test::motionFixture("polish.json"));
  ASSERT_TRUE(parsed);
  if (!parsed) return;
  const auto edit = [&](const char* id, const std::function<void(M&)>& change) {
    auto document = parsed->asObject();
    auto layers = document.at("layers").asArray();
    for (auto& layer : layers) {
      if (layer.getString("id") != id) continue;
      auto node = layer.asObject();
      change(node);
      layer = node;
    }
    document["layers"] = layers;
    std::string error;
    const bool loaded = bool(GraphicsTemplate::load(J(document).stringify(), error));
    return loaded ? std::string("loaded") : error;
  };
  const auto motion = [](M value) { return M{{"in", J(std::move(value))}}; };
  EXPECT_EQ("loaded", edit("plate", [](M&) {}));
  // P5 reveal and the new easings.
  EXPECT_EQ("template-layer-motion", edit("plate", [&](M& n) {
    n["animations"] = motion(M{{"type","wipe"},{"durationMs",300},{"feather",10}}); }));
  EXPECT_EQ("template-layer-motion", edit("plate", [&](M& n) {
    n["animations"] = motion(M{{"type","reveal"},{"durationMs",300},{"feather",513}}); }));
  EXPECT_EQ("template-layer-motion", edit("plate", [&](M& n) {
    n["animations"] = motion(M{{"type","reveal"},{"durationMs",300},{"easing","bounce"}}); }));
  EXPECT_EQ("loaded", edit("plate", [&](M& n) {
    n["animations"] = motion(M{{"type","reveal"},{"durationMs",300},{"direction","up"},{"feather",0}}); }));
  // P6 stagger.
  EXPECT_EQ("template-stagger", edit("name-label", [](M& n) { n["stagger"] = M{{"by","char"},{"ms",60}}; }));
  EXPECT_EQ("template-stagger", edit("name-label", [](M& n) { n["stagger"] = M{{"by","word"},{"ms",5}}; }));
  EXPECT_EQ("template-stagger", edit("name-label", [](M& n) { n["stagger"] = M{{"by","word"}}; }));
  EXPECT_EQ("unsupported-template-property", edit("name-label", [](M& n) { n["stagger"] = M{{"by","word"},{"ms",60},{"x",1}}; }));
  EXPECT_EQ("template-stagger-needs-motion", edit("name-label", [](M& n) { n.erase("animations"); }));
  EXPECT_EQ("template-stagger-word-wrap", edit("title-label", [](M& n) { n["stagger"] = M{{"by","word"},{"ms",60}}; }));
  // 200 + 300 + 11 * 230 = 3030 ms: the last word would finish after 3.0 s.
  EXPECT_EQ("template-motion-too-long", edit("name-label", [](M& n) { n["stagger"] = M{{"by","word"},{"ms",230}}; }));
  EXPECT_EQ("unsupported-template-property", edit("plate", [](M& n) { n["stagger"] = M{{"by","line"},{"ms",60}}; }));
  // P7 loops.
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","spin"},{"periodMs",1000}}; }));
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","pulse"}}; }));
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","pulse"},{"periodMs",100}}; }));
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","pulse"},{"periodMs",60001}}; }));
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","pulse"},{"periodMs",1000},{"durationMs",500}}; }));
  EXPECT_EQ("template-layer-loop", edit("shine", [](M& n) { n["loop"] = M{{"type","sweep"},{"periodMs",1000},{"durationMs",1500}}; }));
  EXPECT_EQ("template-layer-loop", edit("shine", [](M& n) { n["loop"] = M{{"type","sweep"},{"periodMs",1000},{"distance",4}}; }));
  EXPECT_EQ("template-layer-loop", edit("accent", [](M& n) { n["loop"] = M{{"type","drift"},{"periodMs",1000},{"direction","diagonal"}}; }));
  EXPECT_EQ("loaded", edit("accent", [](M& n) { n["loop"] = M{{"type","drift"},{"periodMs",4000},{"distance",6},{"direction","left"}}; }));
}

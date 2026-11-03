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

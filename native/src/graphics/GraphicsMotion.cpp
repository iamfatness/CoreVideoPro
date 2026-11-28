#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsCrawl.h"
#include "graphics/GraphicsTemplate.h"

#include <cmath>

namespace corevideo::graphics {
namespace {
constexpr double kPi = 3.14159265358979323846;
// The parent transform applied to a child's reveal edge (axis-aligned scale
// and offset); a collapsed axis has no ramp left.
std::optional<GraphicsRevealEdge> transformEdge(const GraphicsAffine& transform, const GraphicsRevealEdge& edge) {
  const double scale = edge.vertical ? transform.sy : transform.sx;
  if (!(scale > 0)) return std::nullopt;
  auto result = edge;
  result.lead = scale * edge.lead + (edge.vertical ? transform.ty : transform.tx);
  result.width = scale * edge.width;
  return result;
}
}  // namespace

double motionEase(const std::string& easing, double t) {
  if (easing == "ease-in") return t * t;
  if (easing == "ease-out") return t * (2 - t);
  if (easing == "ease-in-out") return t * t * (3 - 2 * t);
  if (easing == "expo-out") return t >= 1 ? 1. : 1 - std::pow(2., -10 * t);
  if (easing == "quint-out") { const double u = 1 - t; return 1 - u * u * u * u * u; }
  return t;
}

GraphicsBox GraphicsAffine::apply(const GraphicsBox& box) const {
  return {sx * box.x + tx, sy * box.y + ty, sx * box.width, sy * box.height};
}

GraphicsAffine GraphicsAffine::then(const GraphicsAffine& inner) const {
  return {sx * inner.sx, sy * inner.sy, sx * inner.tx + tx, sy * inner.ty + ty};
}

GraphicsMotionSample sampleUnit(const GraphicsLayerMotion& motion, const GraphicsBox& box,
    const std::optional<GraphicsBox>& mask, GraphicsMotionPhase phase, double elapsedMs) {
  GraphicsMotionSample sample;
  if (phase == GraphicsMotionPhase::Hidden) {
    sample.opacity = 0;
    return sample;
  }
  const double u = motion.durationMs == 0 ? (elapsedMs >= motion.startMs ? 1. : 0.) :
      std::clamp((elapsedMs - motion.startMs) / motion.durationMs, 0., 1.);
  const double eased = motionEase(motion.easing, u);
  const double q = phase == GraphicsMotionPhase::Settled ? 1. :
      phase == GraphicsMotionPhase::Out ? 1. - eased : eased;
  const bool left = motion.direction == "left", right = motion.direction == "right";
  const bool up = motion.direction == "up";
  const auto area = mask.value_or(box);
  double distance = motion.distance;
  if (motion.distanceEdge) {
    if (motion.type == "rise") distance = area.y + area.height - box.y;
    else if (left) distance = box.x + box.width - area.x;
    else if (right) distance = area.x + area.width - box.x;
    else if (up) distance = box.y + box.height - area.y;
    else distance = area.y + area.height - box.y;
    distance = std::max(1., distance);
  }
  sample.clip = mask;
  auto& transform = sample.transform;
  if (motion.type == "fade" || motion.type == "rise") {
    sample.opacity = q;
    if (motion.type == "rise") transform.ty = (1 - q) * distance;
  } else if (motion.type == "slide") {
    const double travel = (1 - q) * distance;
    if (left || right) transform.tx = left ? -travel : travel;
    else transform.ty = up ? -travel : travel;
  } else if (motion.type == "wipe") {
    GraphicsBox wipe = box;
    if (left || right) {
      wipe.width *= q;
      if (right) wipe.x += box.width - wipe.width;
    } else {
      wipe.height *= q;
      if (!up) wipe.y += box.height - wipe.height;
    }
    sample.clip = mask ? intersect(*mask, wipe) : wipe;
  } else if (motion.type == "reveal") {
    // A wipe whose leading edge is a feather ramp: the lead travels the box
    // plus the feather, so the whole box is fully covered exactly at q = 1.
    const bool vertical = !(left || right);
    const double extent = vertical ? box.height : box.width;
    const double travel = q * (extent + motion.feather);
    const double shown = std::min(extent, travel);
    const bool fromStart = left || up;
    GraphicsBox reveal = box;
    GraphicsRevealEdge edge{vertical, fromStart ? 1. : -1., 0, motion.feather};
    if (vertical) {
      reveal.height = shown;
      if (!fromStart) reveal.y += box.height - shown;
      edge.lead = fromStart ? box.y + travel : box.y + box.height - travel;
    } else {
      reveal.width = shown;
      if (!fromStart) reveal.x += box.width - shown;
      edge.lead = fromStart ? box.x + travel : box.x + box.width - travel;
    }
    sample.clip = mask ? intersect(*mask, reveal) : reveal;
    if (motion.feather > 0 && q < 1) sample.edge = edge;
  } else if (motion.type == "grow") {
    if (left || right) {
      transform.sx = q;
      transform.tx = (box.x + (right ? box.width : 0.)) * (1 - q);
    } else {
      transform.sy = q;
      transform.ty = (box.y + (up ? 0. : box.height)) * (1 - q);
    }
  } else if (motion.type == "pop") {
    const double t = q - 1;
    const double scale = 1 + .5 * t * t * (2.70158 * t + 1.70158);
    transform = {scale, scale, (box.x + box.width / 2) * (1 - scale),
        (box.y + box.height / 2) * (1 - scale)};
    sample.opacity = std::min(1., q / .4);
  } else {
    sample.opacity = q >= 1 ? 1 : 0;
  }
  return sample;
}

GraphicsMotionSample sampleLoop(const GraphicsLayerLoop& loop, const GraphicsBox& box,
    const std::optional<GraphicsBox>& mask, double loopMs) {
  GraphicsMotionSample sample;
  if (!(loop.periodMs > 0)) return sample;
  const double at = std::fmod(std::max(0., loopMs), loop.periodMs);
  const bool left = loop.direction == "left", right = loop.direction == "right", up = loop.direction == "up";
  if (loop.type == "sweep") {
    sample.clip = mask;
    const double u = loop.durationMs > 0 ? at / loop.durationMs : 1.;
    if (u >= 1) { sample.opacity = 0; return sample; }
    const double e = u * u * (3 - 2 * u);  // ease-in-out
    const auto area = mask.value_or(box);
    if (left || right) {
      const double before = area.x - (box.x + box.width), after = area.x + area.width - box.x;
      sample.transform.tx = left ? before + (after - before) * e : after + (before - after) * e;
    } else {
      const double before = area.y - (box.y + box.height), after = area.y + area.height - box.y;
      sample.transform.ty = up ? before + (after - before) * e : after + (before - after) * e;
    }
  } else if (loop.type == "pulse") {
    sample.opacity = 1 - .6 * (1 - std::cos(2 * kPi * at / loop.periodMs)) / 2;
  } else if (loop.type == "drift") {
    const double offset = loop.distance * std::sin(2 * kPi * at / loop.periodMs);
    if (left || right) sample.transform.tx = left ? -offset : offset;
    else sample.transform.ty = up ? -offset : offset;
  }
  return sample;
}

GraphicsLayerMotion graphicMotion(const GraphicsTransition& transition, double width, double height) {
  GraphicsLayerMotion motion;
  if (transition.type == "none") return motion;
  motion.type = transition.type;
  motion.durationMs = transition.durationMs;
  motion.easing = transition.easing;
  motion.direction = transition.direction;
  motion.distance = motion.direction == "up" || motion.direction == "down" ? height : width;
  return motion;
}

GraphicsMotionSample sampleRun(const GraphicsDrawing& drawing, int unit, const GraphicsMotionState& state) {
  GraphicsMotionSample result;
  if (state.phase == GraphicsMotionPhase::Hidden) {
    result.opacity = 0;
    return result;
  }
  // Settled draws every layer in place; only loops still move it.
  const bool moving = state.phase != GraphicsMotionPhase::Settled;
  const bool entering = state.phase == GraphicsMotionPhase::In;
  const auto& graphic = entering ? state.graphicIn : state.graphicOut;
  const GraphicsBox whole{0, 0, drawing.width, drawing.height};
  const auto combine = [&](const GraphicsMotionSample& sample) {
    if (sample.clip) {
      const auto clip = result.transform.apply(*sample.clip);
      result.clip = result.clip ? intersect(*result.clip, clip) : clip;
    }
    if (sample.edge) result.edge = transformEdge(result.transform, *sample.edge);
    result.transform = result.transform.then(sample.transform);
    result.opacity *= sample.opacity;
  };
  const auto apply = [&](const GraphicsLayerMotion& motion, const GraphicsBox& box,
                         const std::optional<GraphicsBox>& mask) {
    combine(sampleUnit(motion, box, mask, state.phase, state.elapsedMs));
  };
  const auto follow = [&] { apply(graphicMotion(graphic, whole.width, whole.height), whole, {}); };
  if (unit < 0) {
    if (moving) follow();
    return result;
  }
  std::array<const GraphicsMotionUnit*, 16> chain{};
  size_t count = 0;
  for (int index = unit; index >= 0 && count < chain.size(); index = drawing.units[index].parent)
    chain[count++] = &drawing.units[index];
  while (count > 0) {
    const auto& layer = *chain[--count];
    if (moving) {
      const auto& own = entering ? layer.in : layer.out;
      if (own) apply(*own, layer.box, layer.mask);
      else if (layer.root) follow();
    }
    if (layer.loop) combine(sampleLoop(*layer.loop, layer.box, layer.mask, state.loopMs));
  }
  return result;
}

double motionTotal(const GraphicsMotionSummary& summary, const GraphicsTransition& graphic, bool in) {
  const bool follows = in ? summary.inFollows : summary.outFollows;
  return std::max(in ? summary.inMs : summary.outMs, follows && graphic.type != "none" ? graphic.durationMs : 0.);
}

std::vector<GraphicsRun> planRuns(const GraphicsDrawing& drawing) {
  std::vector<GraphicsRun> runs;
  for (size_t i = 0; i < drawing.primitives.size(); ++i) {
    const auto& primitive = drawing.primitives[i];
    if (runs.empty() || runs.back().unit != primitive.unit || primitive.crawlStart ||
        drawing.primitives[runs.back().first].crawlStart)
      runs.push_back({primitive.unit, i, 0, {}});
    auto& run = runs.back();
    ++run.count;
    const auto include = [&](const GraphicsBox& clip) {
      if (clip.width <= 0 || clip.height <= 0) return;
      if (run.source.width == 0 || run.source.height == 0) {
        run.source = clip;
        return;
      }
      const double right = std::max(run.source.x + run.source.width, clip.x + clip.width);
      const double bottom = std::max(run.source.y + run.source.height, clip.y + clip.height);
      run.source.x = std::min(run.source.x, clip.x);
      run.source.y = std::min(run.source.y, clip.y);
      run.source.width = right - run.source.x;
      run.source.height = bottom - run.source.y;
    };
    // A shape shadow paints outside its box, inside its own clip (styles P4).
    if (primitive.shapeShadow) include(primitive.shadowClip);
    include(primitive.clip);
  }
  return runs;
}

bool packAtlas(const std::vector<GraphicsRun>& runs, double scale,
    std::vector<GraphicsAtlasSlice>& slices, int& width, int& height) {
  slices.clear();
  width = height = 0;
  for (const auto& run : runs) {
    const double w = std::ceil(run.source.width * scale), h = std::ceil(run.source.height * scale);
    const int y = slices.empty() ? 0 : height + 2;
    if (w > 16384 || y + h > 16384) return false;
    slices.push_back({0, y, static_cast<int>(w), static_cast<int>(h)});
    width = std::max(width, slices.back().width);
    height = y + slices.back().height;
  }
  return true;
}

bool placeRun(const GraphicsRunLayout& layout, size_t index, const GraphicsMotionState& state,
    std::optional<double> crawlDistance, GraphicsRunPlacement& out) {
  if (!layout.drawing || index >= layout.runs.size() || index >= layout.slices.size()) return false;
  const auto& run = layout.runs[index];
  const auto& slice = layout.slices[index];
  if (slice.width <= 0 || slice.height <= 0 || run.first >= layout.drawing->primitives.size()) return false;
  const auto sample = sampleRun(*layout.drawing, run.unit, state);
  if (sample.opacity <= 0) return false;
  auto source = run.source;
  auto mask = sample.clip;
  const auto& item = layout.drawing->primitives[run.first];
  if (item.crawlStart && layout.drawing->crawl) {
    const auto& crawl = *layout.drawing->crawl;
    if (crawlDistance && *crawlDistance >= crawl.passWidth) return false;
    source.x = crawl.window.x + (crawlDistance ? crawlItemX(crawl.window.width, *item.crawlStart,
        source.width, *crawlDistance, crawl.rightward) : *item.crawlStart);
    const auto window = sample.transform.apply(crawl.clip);
    if (intersect(sample.transform.apply(source), window).width <= 0) return false;
    mask = mask ? intersect(*mask, window) : window;
  }
  out.destination = sample.transform.apply(source);
  if (out.destination.width <= 0 || out.destination.height <= 0) return false;
  out.mask = mask;
  out.opacity = sample.opacity;
  out.edge = sample.edge && sample.edge->width > 0 ? sample.edge : std::nullopt;
  return true;
}

void revealEdgeConstants(const std::optional<GraphicsRevealEdge>& edge, double layoutWidth, double layoutHeight,
    double rectX, double rectY, double rectWidth, double rectHeight, int targetWidth, int targetHeight, float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0.f;
  if (!edge || !(layoutWidth > 0) || !(layoutHeight > 0)) return;
  const double scale = edge->vertical ? rectHeight * targetHeight / layoutHeight : rectWidth * targetWidth / layoutWidth;
  const double origin = edge->vertical ? rectY * targetHeight : rectX * targetWidth;
  const double width = edge->width * scale;
  if (!(width > 0) || !std::isfinite(width)) return;
  const double lead = origin + edge->lead * scale;
  out[0] = edge->vertical ? 0.f : float(edge->sign);
  out[1] = edge->vertical ? float(edge->sign) : 0.f;
  out[2] = float(edge->sign * lead);
  out[3] = float(1 / width);
}

GraphicsMotionState previewMotion(const std::string& mode, double elapsedMs, double inMs, double outMs,
    const GraphicsTransition& graphicIn, const GraphicsTransition& graphicOut) {
  using Phase = GraphicsMotionPhase;
  const double loopMs = elapsedMs;
  const auto state = [&](Phase phase, double elapsed = 0) {
    return GraphicsMotionState{phase, elapsed, graphicIn, graphicOut, loopMs};
  };
  if (mode == "in") return elapsedMs < inMs ? state(Phase::In, elapsedMs) : state(Phase::Settled);
  if (mode == "cycle" || mode == "loop") {
    if (mode == "loop") elapsedMs = std::fmod(elapsedMs, inMs + 1000 + outMs + 500);
    if (elapsedMs < inMs) return state(Phase::In, elapsedMs);
    elapsedMs -= inMs + 1000;
    if (elapsedMs < 0) return state(Phase::Settled);
  } else if (mode != "out") {
    return state(Phase::Settled);
  }
  if (elapsedMs < outMs) return state(Phase::Out, elapsedMs);
  return state(elapsedMs < outMs + 500 ? Phase::Hidden : Phase::Settled);
}
void GraphicsLayerClock::configure(double inMs, double outMs) {
  inMs_ = inMs;
  outMs_ = outMs;
}

double GraphicsLayerClock::position(double nowMs) const {
  return std::clamp(anchorPos_ + direction_ * (nowMs - anchorMs_), 0.,
      mode_ == Mode::In ? inMs_ : outMs_);
}

void GraphicsLayerClock::anchor(Mode mode, int direction, double nowMs, double position) {
  mode_ = mode;
  direction_ = direction;
  anchorMs_ = nowMs;
  anchorPos_ = position;
}

bool GraphicsLayerClock::settle(double nowMs) {
  const double elapsed = position(nowMs);
  if (mode_ == Mode::In && (direction_ > 0 ? elapsed >= inMs_ : elapsed <= 0)) {
    mode_ = direction_ > 0 ? Mode::OnAir : Mode::Hidden;
    return true;
  }
  if (mode_ == Mode::Out && (direction_ > 0 ? elapsed >= outMs_ : elapsed <= 0)) {
    mode_ = direction_ > 0 ? Mode::Hidden : Mode::OnAir;
    return true;
  }
  return false;
}

void GraphicsLayerClock::show(double nowMs) {
  settle(nowMs);
  if (mode_ == Mode::Hidden) { anchor(Mode::In, 1, nowMs, 0); loopAnchorMs_ = nowMs; }
  else if (mode_ == Mode::In && direction_ < 0) anchor(mode_, 1, nowMs, position(nowMs));
  else if (mode_ == Mode::Out && direction_ > 0) anchor(mode_, -1, nowMs, position(nowMs));
  settle(nowMs);
}

void GraphicsLayerClock::hide(double nowMs) {
  settle(nowMs);
  if (mode_ == Mode::OnAir) anchor(Mode::Out, 1, nowMs, 0);
  else if (mode_ == Mode::In && direction_ > 0) anchor(mode_, -1, nowMs, position(nowMs));
  else if (mode_ == Mode::Out && direction_ < 0) anchor(mode_, 1, nowMs, position(nowMs));
  settle(nowMs);
}

void GraphicsLayerClock::clear() { mode_ = Mode::Hidden; }

GraphicsMotionState GraphicsLayerClock::state(double nowMs) const {
  auto current = *this;
  current.settle(nowMs);
  using Phase = GraphicsMotionPhase;
  const double loopMs = std::max(0., nowMs - loopAnchorMs_);
  switch (current.mode_) {
    case Mode::In: return {Phase::In, position(nowMs), {}, {}, loopMs};
    case Mode::Out: return {Phase::Out, position(nowMs), {}, {}, loopMs};
    case Mode::OnAir: return {Phase::Settled, 0, {}, {}, loopMs};
    case Mode::Hidden: return {Phase::Hidden, 0, {}, {}};
  }
  return {};
}

std::string GraphicsLayerClock::visibility(double nowMs) const {
  const auto phase = state(nowMs).phase;
  if (phase == GraphicsMotionPhase::Hidden) return "hidden";
  if (phase == GraphicsMotionPhase::Settled) return "on-air";
  return (phase == GraphicsMotionPhase::In) == (direction_ > 0) ? "entering" : "exiting";
}
} // namespace corevideo::graphics

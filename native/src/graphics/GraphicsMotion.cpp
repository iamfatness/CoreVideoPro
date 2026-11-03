#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsTemplate.h"

namespace corevideo::graphics {
double motionEase(const std::string& easing, double t) {
  if (easing == "ease-in") return t * t;
  if (easing == "ease-out") return t * (2 - t);
  if (easing == "ease-in-out") return t * t * (3 - 2 * t);
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
  if (state.phase == GraphicsMotionPhase::Settled) return result;
  if (state.phase == GraphicsMotionPhase::Hidden) {
    result.opacity = 0;
    return result;
  }
  const bool entering = state.phase == GraphicsMotionPhase::In;
  const auto& graphic = entering ? state.graphicIn : state.graphicOut;
  const GraphicsBox whole{0, 0, drawing.width, drawing.height};
  const auto apply = [&](const GraphicsLayerMotion& motion, const GraphicsBox& box,
                         const std::optional<GraphicsBox>& mask) {
    const auto sample = sampleUnit(motion, box, mask, state.phase, state.elapsedMs);
    if (sample.clip) {
      const auto clip = result.transform.apply(*sample.clip);
      result.clip = result.clip ? intersect(*result.clip, clip) : clip;
    }
    result.transform = result.transform.then(sample.transform);
    result.opacity *= sample.opacity;
  };
  const auto follow = [&] { apply(graphicMotion(graphic, whole.width, whole.height), whole, {}); };
  if (unit < 0) {
    follow();
    return result;
  }
  std::vector<int> chain;
  for (int index = unit; index >= 0; index = drawing.units[index].parent) chain.push_back(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const auto& layer = drawing.units[*it];
    const auto& own = entering ? layer.in : layer.out;
    if (own) apply(*own, layer.box, layer.mask);
    else if (layer.root) follow();
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
    if (runs.empty() || runs.back().unit != primitive.unit) runs.push_back({primitive.unit, i, 0, {}});
    auto& run = runs.back();
    ++run.count;
    const auto& clip = primitive.clip;
    if (clip.width <= 0 || clip.height <= 0) continue;
    if (run.source.width == 0 || run.source.height == 0) {
      run.source = clip;
      continue;
    }
    const double right = std::max(run.source.x + run.source.width, clip.x + clip.width);
    const double bottom = std::max(run.source.y + run.source.height, clip.y + clip.height);
    run.source.x = std::min(run.source.x, clip.x);
    run.source.y = std::min(run.source.y, clip.y);
    run.source.width = right - run.source.x;
    run.source.height = bottom - run.source.y;
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

GraphicsMotionState previewMotion(const std::string& mode, double elapsedMs, double inMs, double outMs,
    const GraphicsTransition& graphicIn, const GraphicsTransition& graphicOut) {
  using Phase = GraphicsMotionPhase;
  const auto state = [&](Phase phase, double elapsed = 0) {
    return GraphicsMotionState{phase, elapsed, graphicIn, graphicOut};
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
  if (mode_ == Mode::Hidden) anchor(Mode::In, 1, nowMs, 0);
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
  switch (current.mode_) {
    case Mode::In: return {Phase::In, position(nowMs), {}, {}};
    case Mode::Out: return {Phase::Out, position(nowMs), {}, {}};
    case Mode::OnAir: return {};
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

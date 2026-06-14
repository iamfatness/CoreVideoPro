export type TickerItemPriority = "normal" | "breaking" | "sponsored";

export type TickerItem = {
  id: string;
  text: string;
  priority: TickerItemPriority;
  tag?: string;
  addedAtMs: number;
};

export type TickerState = {
  items: TickerItem[];
  running: boolean;
  speedMultiplier: number;
  loop: boolean;
  currentIndex: number;
  cycleCount: number;
};

let _counter = 0;
function nextId() {
  return `ticker-item-${++_counter}`;
}

export function createTicker(): TickerState {
  return {
    items: [],
    running: false,
    speedMultiplier: 1,
    loop: true,
    currentIndex: 0,
    cycleCount: 0
  };
}

export function addTickerItem(
  state: TickerState,
  text: string,
  priority: TickerItemPriority = "normal",
  tag?: string
): TickerState {
  const trimmed = text.trim();
  if (!trimmed) throw new Error("Ticker item text must not be empty");
  const item: TickerItem = { id: nextId(), text: trimmed, priority, tag, addedAtMs: Date.now() };
  return { ...state, items: [...state.items, item] };
}

export function removeTickerItem(state: TickerState, id: string): TickerState {
  const index = state.items.findIndex((item) => item.id === id);
  if (index === -1) return state;
  const items = state.items.filter((item) => item.id !== id);
  const currentIndex = items.length === 0 ? 0 : Math.min(state.currentIndex, items.length - 1);
  return { ...state, items, currentIndex };
}

export function reorderTickerItem(state: TickerState, id: string, toIndex: number): TickerState {
  const from = state.items.findIndex((item) => item.id === id);
  if (from === -1) return state;
  const clamped = Math.max(0, Math.min(toIndex, state.items.length - 1));
  const items = [...state.items];
  const [moved] = items.splice(from, 1);
  items.splice(clamped, 0, moved);
  return { ...state, items };
}

export function startTicker(state: TickerState): TickerState {
  return { ...state, running: true };
}

export function pauseTicker(state: TickerState): TickerState {
  return { ...state, running: false };
}

export function stopTicker(state: TickerState): TickerState {
  return { ...state, running: false, currentIndex: 0, cycleCount: 0 };
}

export function setTickerSpeed(state: TickerState, multiplier: number): TickerState {
  const clamped = Math.max(0.25, Math.min(4, multiplier));
  return { ...state, speedMultiplier: clamped };
}

export function setTickerLoop(state: TickerState, loop: boolean): TickerState {
  return { ...state, loop };
}

export function advanceTicker(state: TickerState): TickerState {
  if (state.items.length === 0) return state;
  const next = state.currentIndex + 1;
  if (next >= state.items.length) {
    if (!state.loop) return { ...state, running: false, currentIndex: state.items.length - 1 };
    return { ...state, currentIndex: 0, cycleCount: state.cycleCount + 1 };
  }
  return { ...state, currentIndex: next };
}

export function clearTicker(state: TickerState): TickerState {
  return { ...state, items: [], running: false, currentIndex: 0, cycleCount: 0 };
}

export function currentTickerItem(state: TickerState): TickerItem | undefined {
  return state.items[state.currentIndex];
}

export function priorityLabel(priority: TickerItemPriority): string {
  switch (priority) {
    case "breaking": return "BREAKING";
    case "sponsored": return "SPONSORED";
    case "normal": return "NEWS";
  }
}

export function summarizeTicker(state: TickerState): string {
  if (state.items.length === 0) return "Ticker empty";
  const status = state.running ? "Running" : "Paused";
  const current = currentTickerItem(state);
  const cycles = state.cycleCount > 0 ? ` · cycle ${state.cycleCount + 1}` : "";
  return `${status} · ${state.items.length} items${cycles}${current ? ` · ${current.text.slice(0, 40)}${current.text.length > 40 ? "…" : ""}` : ""}`;
}

export interface IConnectionMap {
  /** Node id -> ids of the nodes that point at it. */
  [nodeId: string]: string[];
}

function buildOutgoing(graph: IConnectionMap): Map<string, string[]> {
  const outgoing = new Map<string, string[]>();
  for (const nodeId of Object.keys(graph)) {
    outgoing.set(nodeId, []);
  }
  for (const [target, sources] of Object.entries(graph)) {
    for (const source of sources) {
      let targets = outgoing.get(source);
      if (targets === undefined) {
        targets = [];
        outgoing.set(source, targets);
      }
      targets.push(target);
    }
  }
  return outgoing;
}

/** Shortest directed path from start to goal, including both endpoints. */
function shortestPath(
  outgoing: Map<string, string[]>,
  start: string,
  goal: string,
): string[] | undefined {
  if (start === goal) {
    return [start];
  }

  const previous = new Map<string, string | undefined>([[start, undefined]]);
  const queue = [start];

  for (let head = 0; head < queue.length; ++head) {
    const current = queue[head];
    for (const next of outgoing.get(current) ?? []) {
      if (previous.has(next)) {
        continue;
      }
      previous.set(next, current);
      if (next !== goal) {
        queue.push(next);
        continue;
      }

      const path: string[] = [];
      let node: string | undefined = goal;
      while (node !== undefined) {
        path.push(node);
        node = previous.get(node);
      }
      return path.toReversed();
    }
  }

  return undefined;
}

/** Rotate a cycle so its smallest id comes first, keeping its direction. */
function canonicalize(cycle: string[]): string[] {
  let first = 0;
  for (let idx = 1; idx < cycle.length; ++idx) {
    if (cycle[idx] < cycle[first]) {
      first = idx;
    }
  }
  return [...cycle.slice(first), ...cycle.slice(0, first)];
}

function cycleKey(cycle: string[]): string {
  return cycle.join("\u0000");
}

/**
 * Find a shortest cycle through every edge that belongs to one.
 *
 * A graph can contain many more simple cycles than edges, so enumerating all of
 * them is not useful for a UI. Instead each cyclic edge contributes one cycle:
 * the edge itself plus the shortest path back from its target to its source.
 * Cycles found from different edges are deduplicated.
 */
export function findShortestCycles(graph: IConnectionMap): string[][] {
  const outgoing = buildOutgoing(graph);
  const cycles = new Map<string, string[]>();

  for (const [target, sources] of Object.entries(graph)) {
    for (const source of sources) {
      if (source === target) {
        const selfLoop = [source];
        cycles.set(cycleKey(selfLoop), selfLoop);
        continue;
      }

      const backPath = shortestPath(outgoing, target, source);
      if (backPath === undefined) {
        continue;
      }

      // backPath starts at target and ends at source; drop the duplicated source.
      const cycle = canonicalize([source, ...backPath.slice(0, -1)]);
      cycles.set(cycleKey(cycle), cycle);
    }
  }

  return [...cycles.values()].toSorted(
    (lhs, rhs) => lhs.length - rhs.length || cycleKey(lhs).localeCompare(cycleKey(rhs)),
  );
}

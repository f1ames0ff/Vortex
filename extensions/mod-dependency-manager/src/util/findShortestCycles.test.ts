import { describe, expect, it } from "vitest";

import { findShortestCycles } from "./findShortestCycles";

describe("findShortestCycles", () => {
  it("finds a two-mod cycle once", () => {
    expect(
      findShortestCycles({
        A: ["B"],
        B: ["A"],
      }),
    ).toEqual([["A", "B"]]);
  });

  it("finds a longer cycle once, regardless of which edge finds it", () => {
    expect(
      findShortestCycles({
        A: ["C"],
        B: ["A"],
        C: ["B"],
      }),
    ).toEqual([["A", "B", "C"]]);
  });

  it("returns the shortest cycles first", () => {
    expect(
      findShortestCycles({
        // A <-> B and A -> C -> D -> A
        A: ["B", "D"],
        B: ["A"],
        C: ["A"],
        D: ["C"],
      }),
    ).toEqual([
      ["A", "B"],
      ["A", "C", "D"],
    ]);
  });

  it("finds separate cycles that share a mod", () => {
    // A -> B -> C -> A and A -> D -> E -> A
    expect(
      findShortestCycles({
        A: ["C", "E"],
        B: ["A"],
        C: ["B"],
        D: ["A"],
        E: ["D"],
      }),
    ).toEqual([
      ["A", "B", "C"],
      ["A", "D", "E"],
    ]);
  });

  it("treats a self-loop as a cycle", () => {
    expect(
      findShortestCycles({
        A: ["A"],
        B: ["A"],
      }),
    ).toEqual([["A"]]);
  });

  it("returns no cycles for an acyclic graph", () => {
    expect(
      findShortestCycles({
        A: [],
        B: ["A"],
        C: ["A", "B"],
      }),
    ).toEqual([]);
  });
});

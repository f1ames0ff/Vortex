import { util } from "@nexusmods/vortex-api";
import cytoscape from "cytoscape";
import coseBilkent from "cytoscape-cose-bilkent";
import * as React from "react";

(cytoscape as any).use(coseBilkent);

const MAX_COLUMNS = 5;

export interface IGraphElement {
  title: string;
  class: string;
  connections: string[];
  readonly?: boolean;
}

export interface IGraphSelection {
  source?: string;
  target?: string;
  id?: string;
  readonly?: boolean;
}

export interface IGraphViewProps {
  elements: { [id: string]: IGraphElement };
  /** Node ids in display order; the closing edge is implied. */
  highlightedCycle?: string[];
  className: string;
  style?: any;
  visualStyle: cytoscape.StylesheetJson;
  onContext: (x: number, y: number, selection: IGraphSelection) => void;
}

function san(input: string): string {
  return input.replace(/[^a-zA-Z0-9_-]/g, (invalid) => `_${invalid.charCodeAt(0)}_`);
}

class GraphView extends React.Component<IGraphViewProps, {}> {
  private mGraph: cytoscape.Core;
  private mLayout: cytoscape.LayoutManipulation;
  private mMousePos: { x: number; y: number } = { x: 0, y: 0 };
  private mLastProps: IGraphViewProps;
  private mEdgeIds: Set<string> = new Set();

  constructor(props: IGraphViewProps) {
    super(props);

    this.mLastProps = props;
  }

  public UNSAFE_componentWillReceiveProps(newProps: IGraphViewProps) {
    const previousProps = this.mLastProps;
    const elementsChanged = newProps.elements !== previousProps.elements;
    const highlightChanged = newProps.highlightedCycle !== previousProps.highlightedCycle;
    // bit of a hack because we got componentWillReceiveProps trigger twice with the same
    // props which causes an error further down the line as we try to update the graph
    this.mLastProps = newProps;

    if (this.mGraph === undefined || (!elementsChanged && !highlightChanged)) {
      return;
    }

    if (elementsChanged) {
      const changed = util.objDiff(previousProps.elements, newProps.elements);

      const newConnections = [];

      Object.keys(changed).forEach((id) => {
        if (id[0] === "+") {
          // node added
          this.mGraph.add({
            data: {
              id: san(id.slice(1)),
              title: changed[id].title,
              originalId: id.slice(1),
            },
            classes: changed[id].class,
            position: this.mMousePos,
          });
          const connections = changed[id].connections;
          Object.keys(connections || []).forEach((refId) => {
            const from = san(id.slice(1));
            const to = san(connections[refId]);
            newConnections.push({
              data: {
                id: `${to}-to-${from}`,
                source: to,
                sourceOrig: connections[refId],
                target: from,
                targetOrig: id.slice(1),
              },
              classes:
                newProps.elements[id] !== undefined ? newProps.elements[id].class : undefined,
            });
          });
        } else if (id[0] === "-") {
          // node removed
          this.mGraph.remove("#" + san(id.slice(1)));
        } else {
          // updated classes
          const nodeId = san(id);
          if (previousProps.elements[id].class !== newProps.elements[id].class) {
            this.mGraph
              .$(`node#${nodeId}, edge[target = "${nodeId}"]`)
              .removeClass(previousProps.elements[id].class)
              .addClass(newProps.elements[id].class);
          }
          // node content changed
          Object.keys(changed[id].connections || [])
            .sort((lhs, rhs) => {
              if (lhs[0] !== rhs[0]) {
                return lhs[0] === "-" ? -1 : 1;
              } else {
                return lhs.localeCompare(rhs);
              }
            })
            .forEach((refId) => {
              const from = san(id);
              const to = san(changed[id].connections[refId]);
              const connId = `${to}-to-${from}`;
              if (refId[0] === "-") {
                this.mEdgeIds.delete(connId);
                this.mGraph.remove("#" + connId);
              } else if (refId[0] === "+") {
                newConnections.push({
                  data: {
                    id: connId,
                    source: to,
                    sourceOrig: changed[id].connections[refId],
                    target: from,
                    targetOrig: id,
                  },
                  classes:
                    newProps.elements[id] !== undefined ? newProps.elements[id].class : undefined,
                });
              }
            });
        }
      });

      newConnections.forEach((conn) => {
        if (!this.mEdgeIds.has(conn.data.id)) {
          this.mEdgeIds.add(conn.data.id);
          this.mGraph.add(conn);
        }
      });
    }

    this.applyCycleHighlight(newProps.highlightedCycle);
  }

  public layout() {
    this.mLayout.run();
  }

  public render(): JSX.Element {
    const { className, style } = this.props;

    return <div ref={this.setRef} className={className} style={style} />;
  }

  private setRef = (ref: HTMLDivElement) => {
    const { elements, highlightedCycle, visualStyle } = this.props;
    if (ref === null) {
      (this.mGraph as any).off("cxttap", this.handleContext);
      this.mGraph = undefined;
      return;
    }
    this.mGraph = cytoscape({
      container: ref,
      style: visualStyle,
      minZoom: 0.33,
      maxZoom: 2,
      wheelSensitivity: 0.1,
      boxSelectionEnabled: false,
    });
    this.addElements(elements || {});
    this.applyCycleHighlight(highlightedCycle);
    this.mGraph.resize();
    this.mGraph.center();
    this.mLayout = this.mGraph.layout({
      name: "cose-bilkent",
      nodeDimensionsIncludeLabels: true,
      randomize: false,
      nodeRepulsion: 9000,
      idealEdgeLength: 500,
    } as any);
    this.mLayout.run();
    (this.mGraph as any).on("cxttap", this.handleContext);
  };

  private handleContext = (evt: cytoscape.EventObject) => {
    let selection;
    if (evt.target.data !== undefined) {
      const data = evt.target.data();
      if (data.title === undefined && data.source === undefined) {
        // an item was hit, but neither a node nor an edge. Probably the edge handle
        return;
      }
      selection =
        data.source !== undefined
          ? {
              source: data.sourceOrig,
              target: data.targetOrig,
              readonly: data.readonly,
            }
          : { id: data.originalId, readonly: data.readonly };
    }
    this.mMousePos = evt.position;
    this.props.onContext(evt.renderedPosition.x, evt.renderedPosition.y, selection);
  };

  private addElements(elements: { [id: string]: IGraphElement }) {
    const width = MAX_COLUMNS;
    const distance = (this.mGraph.width() / width) * 2;
    this.mGraph.add(
      Object.keys(elements).reduce((prev, id: string, idx: number) => {
        const ele = elements[id];
        const row = Math.floor(idx / width);
        const pos = row % 2 === 0 ? idx % width : width - (idx % width);
        prev.push({
          data: {
            id: san(id),
            title: ele.title,
            originalId: id,
            readonly: ele.readonly,
          },
          classes: ele.class,
          position: {
            x: pos * distance,
            y: row * distance + (pos % 2) * (distance / 2),
          },
        });
        (ele.connections || []).forEach((conn) => {
          prev.push({
            data: {
              id: san(`${san(conn)}-to-${san(id)}`),
              target: san(id),
              source: san(conn),
              targetOrig: id,
              sourceOrig: conn,
              readonly: ele.readonly,
            },
            classes: ele.class,
          });
        });
        return prev;
      }, []),
    );
  }

  private applyCycleHighlight(cycle: string[] | undefined) {
    this.mGraph
      .edges()
      .removeClass("cycle-highlight")
      .removeClass("cycle-conflict")
      .addClass("cycle-hidden");
    this.mGraph.nodes().removeClass("cycle-current").removeClass("cycle-conflict");

    if (cycle === undefined || cycle.length === 0) {
      this.mGraph.edges().removeClass("cycle-hidden");
      return;
    }

    cycle.forEach((modId) => {
      this.mGraph.$(`node#${san(modId)}`).addClass("cycle-current");
    });

    for (let idx = 0; idx < cycle.length; ++idx) {
      const source = san(cycle[idx]);
      const target = san(cycle[(idx + 1) % cycle.length]);
      this.mGraph
        .$(`edge#${source}-to-${target}`)
        .removeClass("cycle-hidden")
        .addClass("cycle-highlight");
    }
  }
}

export default GraphView;

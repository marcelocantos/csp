#!/usr/bin/env python3
"""Generate SVG diagrams from ASCII-art DSL embedded in markdown files.

Scans .md files under docs/ for <!-- csp-flow ... --> (and other csp-* tags)
comment blocks, parses the ASCII-art DSL, and generates SVG files.

The output path is derived from the ![alt](path.svg) image reference
immediately following each comment block.
"""

import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class Token:
    kind: str        # 'endpoint', 'processor', 'conceptual', 'arrow',
                     # 'arrow_dashed', 'arrow_labeled', 'arrow_dashed_labeled',
                     # 'arrow_reverse', 'pipe', 'label'
    text: str        # display text (for nodes/labels) or arrow symbol
    row: int
    col_start: int
    col_end: int
    label: str = ''  # for labeled arrows

    @property
    def col_center(self):
        return (self.col_start + self.col_end) / 2


@dataclass
class Node:
    id: int
    text: str
    kind: str        # 'endpoint', 'processor', 'conceptual'
    row: int         # row in the ASCII art
    col_center: float
    col_start: int
    col_end: int
    # Layout (filled in later)
    grid_row: int = 0
    grid_col: int = 0
    x: float = 0
    y: float = 0
    w: float = 0
    h: float = 0
    fill: str = ''


@dataclass
class Edge:
    src: int         # node id
    dst: int         # node id
    label: str = ''
    style: str = 'solid'      # 'solid' or 'dashed'
    direction: str = 'horizontal'  # 'horizontal' or 'vertical'


@dataclass
class FlowGraph:
    nodes: List[Node] = field(default_factory=list)
    edges: List[Edge] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CHAR_WIDTH = 7.2          # approx width of a monospace char at 12px
NODE_PAD_X = 16           # horizontal padding inside node box
NODE_HEIGHT = 30
NODE_SMALL_HEIGHT = 26
CORNER_RADIUS = 4
ARROW_GAP = 40            # gap between node edges for arrows
ROW_GAP = 40              # vertical gap between rows
PADDING = 5               # viewBox padding
FONT_SIZE = 12
LABEL_FONT_SIZE = 10

# Colors
COLOR_ENDPOINT = '#e8e8e8'
COLOR_PROCESSOR = '#f5d6a8'
COLOR_CONCEPTUAL = '#f0f0f0'
COLOR_CONTROL = '#d4edda'
COLOR_OBSERVATIONAL = '#d4e6f1'
COLOR_STROKE = '#888'
COLOR_ARROW = '#333'
COLOR_ARROW_GRAY = '#888'
COLOR_LABEL = '#666'


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

# Combined regex for all token types.  Order matters: labeled arrows before
# plain arrows, delimited nodes before bare text (handled in gaps).
# NOTE: arrows use -> (not -->) to avoid HTML comment closing conflict.
TOKEN_RE = re.compile(r"""
    (?P<arrow_dashed_labeled>  \.\."(?P<dal>[^"]*)"\.\.\>  )
  | (?P<arrow_labeled>         -"(?P<al>[^"]*)"->          )
  | (?P<arrow_reverse>         <-"(?P<rl>[^"]*)"(?:-|$)    )
  | (?P<arrow>                 ->                           )
  | (?P<arrow_dashed>          \.\.\>                       )
  | (?P<processor>             \{(?P<pt>[^}]+)\}            )
  | (?P<conceptual>            \((?P<ct>[^)]+)\)            )
  | (?P<pipe>                  \|                           )
  | (?P<label>                 "(?P<lt>[^"]*)"              )
""", re.VERBOSE)


def tokenize_line(line: str, row: int) -> List[Token]:
    """Tokenize a single line of csp-flow ASCII art."""
    tokens = []
    occupied: Set[int] = set()

    for m in TOKEN_RE.finditer(line):
        s, e = m.start(), m.end()
        occupied.update(range(s, e))

        if m.group('arrow_dashed_labeled'):
            tokens.append(Token('arrow_dashed_labeled', '..>', row, s, e,
                                label=m.group('dal')))
        elif m.group('arrow_labeled'):
            tokens.append(Token('arrow_labeled', '->', row, s, e,
                                label=m.group('al')))
        elif m.group('arrow_reverse'):
            tokens.append(Token('arrow_reverse', '<-', row, s, e,
                                label=m.group('rl')))
        elif m.group('arrow'):
            tokens.append(Token('arrow', '-->', row, s, e))
        elif m.group('arrow_dashed'):
            tokens.append(Token('arrow_dashed', '..>', row, s, e))
        elif m.group('processor'):
            tokens.append(Token('processor', m.group('pt'), row, s, e))
        elif m.group('conceptual'):
            tokens.append(Token('conceptual', m.group('ct'), row, s, e))
        elif m.group('pipe'):
            tokens.append(Token('pipe', '|', row, s, e))
        elif m.group('label'):
            tokens.append(Token('label', m.group('lt'), row, s, e))

    # Find bare text in gaps between tokens.
    _extract_bare_text(line, row, occupied, tokens)

    tokens.sort(key=lambda t: t.col_start)
    return tokens


def _extract_bare_text(line: str, row: int, occupied: Set[int],
                       tokens: List[Token]):
    """Extract bare text nodes from unoccupied regions of a line."""
    i = 0
    n = len(line)
    while i < n:
        if i in occupied or line[i].isspace():
            i += 1
            continue
        # Start of potential bare text.
        j = i
        while j < n and j not in occupied:
            j += 1
        text = line[i:j].strip()
        if text:
            # Trim: find actual start/end of non-whitespace
            actual_start = i + (len(line[i:j]) - len(line[i:j].lstrip()))
            actual_end = j - (len(line[i:j]) - len(line[i:j].rstrip()))
            tokens.append(Token('endpoint', text, row, actual_start,
                                actual_end))
        i = j


def tokenize(text: str) -> List[List[Token]]:
    """Tokenize the full csp-flow block into per-row token lists."""
    lines = text.split('\n')
    result = []
    for row, line in enumerate(lines):
        result.append(tokenize_line(line, row))
    return result


# ---------------------------------------------------------------------------
# Graph builder
# ---------------------------------------------------------------------------

def _is_arrow(tok: Token) -> bool:
    return tok.kind in ('arrow', 'arrow_dashed', 'arrow_labeled',
                        'arrow_dashed_labeled', 'arrow_reverse')


def _is_node(tok: Token) -> bool:
    return tok.kind in ('endpoint', 'processor', 'conceptual')


def _arrow_style(tok: Token) -> str:
    if tok.kind in ('arrow_dashed', 'arrow_dashed_labeled'):
        return 'dashed'
    return 'solid'


def _arrow_label(tok: Token) -> str:
    return tok.label


def build_flow_graph(token_rows: List[List[Token]]) -> FlowGraph:
    """Build a FlowGraph from tokenized lines."""
    graph = FlowGraph()
    node_id = 0
    all_nodes: List[Node] = []
    node_by_id: Dict[int, Node] = {}

    # Collect all node tokens.
    for row_tokens in token_rows:
        for tok in row_tokens:
            if _is_node(tok):
                n = Node(id=node_id, text=tok.text, kind=tok.kind,
                         row=tok.row, col_center=tok.col_center,
                         col_start=tok.col_start, col_end=tok.col_end)
                all_nodes.append(n)
                node_by_id[node_id] = n
                node_id += 1

    graph.nodes = all_nodes

    # Build horizontal edges.
    for row_tokens in token_rows:
        nodes_in_row = [t for t in row_tokens if _is_node(t)]
        arrows_in_row = [t for t in row_tokens if _is_arrow(t)]

        for arrow in arrows_in_row:
            is_reverse = arrow.kind == 'arrow_reverse'
            # Find nearest node to the left and right of this arrow.
            left_node = _find_nearest_node_left(arrow, nodes_in_row)
            right_node = _find_nearest_node_right(arrow, nodes_in_row)

            if left_node is None:
                # Dangling left: find across all rows.
                left_node = _find_nearest_node_left_any(arrow, all_nodes)
            if right_node is None:
                # Dangling right: find across all rows.
                right_node = _find_nearest_node_right_any(arrow, all_nodes)

            if left_node is not None and right_node is not None:
                src_id = _node_id(left_node, all_nodes)
                dst_id = _node_id(right_node, all_nodes)
                if is_reverse:
                    src_id, dst_id = dst_id, src_id
                graph.edges.append(Edge(
                    src=src_id, dst=dst_id,
                    label=_arrow_label(arrow),
                    style=_arrow_style(arrow),
                    direction='horizontal'))

    # Build vertical edges from pipe tokens.
    _build_vertical_edges(token_rows, all_nodes, graph)

    return graph


def _node_id(tok_or_node, all_nodes: List[Node]) -> int:
    """Find the node ID matching a token (by row and col_start)."""
    if isinstance(tok_or_node, Node):
        return tok_or_node.id
    for n in all_nodes:
        if n.row == tok_or_node.row and n.col_start == tok_or_node.col_start:
            return n.id
    return -1


def _find_nearest_node_left(arrow: Token, nodes: list):
    """Find the nearest node token to the left of an arrow on the same row."""
    candidates = [n for n in nodes
                  if n.row == arrow.row and n.col_end <= arrow.col_start]
    if not candidates:
        return None
    return max(candidates, key=lambda n: n.col_end)


def _find_nearest_node_right(arrow: Token, nodes: list):
    """Find the nearest node token to the right of an arrow on the same row."""
    candidates = [n for n in nodes
                  if n.row == arrow.row and n.col_start >= arrow.col_end]
    if not candidates:
        return None
    return min(candidates, key=lambda n: n.col_start)


def _find_nearest_node_left_any(arrow: Token, all_nodes: List[Node]):
    """Find the nearest node to the left of an arrow across all rows."""
    # Look for nodes whose col_center is to the left of or overlapping
    # the arrow, preferring close columns.
    candidates = [n for n in all_nodes
                  if n.col_center < arrow.col_start + 2
                  and n.row != arrow.row]
    if not candidates:
        return None
    # Prefer closest column, then prefer rows closer to the arrow's row.
    return min(candidates, key=lambda n: (
        abs(n.col_center - arrow.col_start), abs(n.row - arrow.row)))


def _find_nearest_node_right_any(arrow: Token, all_nodes: List[Node]):
    """Find the nearest node to the right of an arrow across all rows."""
    candidates = [n for n in all_nodes
                  if n.col_center > arrow.col_end - 2
                  and n.row != arrow.row]
    if not candidates:
        return None
    return min(candidates, key=lambda n: (
        abs(n.col_center - arrow.col_end), abs(n.row - arrow.row)))


def _build_vertical_edges(token_rows: List[List[Token]],
                          all_nodes: List[Node],
                          graph: FlowGraph):
    """Build vertical edges from | pipe tokens."""
    # Collect all pipes grouped by approximate column.
    pipes = []
    for row_tokens in token_rows:
        for tok in row_tokens:
            if tok.kind == 'pipe':
                pipes.append(tok)

    # Collect all standalone labels (on lines without nodes or arrows).
    labels = []
    for row_tokens in token_rows:
        has_node = any(_is_node(t) for t in row_tokens)
        has_arrow = any(_is_arrow(t) for t in row_tokens)
        for tok in row_tokens:
            if tok.kind == 'label' and not has_node and not has_arrow:
                labels.append(tok)

    # For each column of pipes, find the node above and below.
    # Group pipes by column proximity (within 3 chars).
    pipe_groups = _group_by_column(pipes, threshold=3)

    for col, group_pipes in pipe_groups:
        rows_with_pipes = sorted(set(p.row for p in group_pipes))
        if not rows_with_pipes:
            continue

        min_pipe_row = min(rows_with_pipes)
        max_pipe_row = max(rows_with_pipes)

        # Find node above: nearest node above the topmost pipe in this column.
        above = _find_node_near_col(all_nodes, col, max_row=min_pipe_row - 1,
                                    direction='above')
        # Find node below: nearest node below the bottommost pipe.
        below = _find_node_near_col(all_nodes, col, min_row=max_pipe_row + 1,
                                    direction='below')

        if above is not None and below is not None:
            # Find label near this pipe group (include adjacent rows).
            label_text = ''
            for lab in labels:
                if (min_pipe_row - 1 <= lab.row <= max_pipe_row + 1
                        and abs(lab.col_center - col) < 20):
                    label_text = lab.text
                    break

            graph.edges.append(Edge(
                src=above.id, dst=below.id,
                label=label_text,
                style='dashed',
                direction='vertical'))


def _group_by_column(tokens: List[Token],
                     threshold: int) -> List[Tuple[float, List[Token]]]:
    """Group tokens by column proximity."""
    if not tokens:
        return []
    tokens = sorted(tokens, key=lambda t: t.col_center)
    groups = []
    current_col = tokens[0].col_center
    current_group = [tokens[0]]
    for tok in tokens[1:]:
        if abs(tok.col_center - current_col) <= threshold:
            current_group.append(tok)
        else:
            groups.append((current_col, current_group))
            current_col = tok.col_center
            current_group = [tok]
    groups.append((current_col, current_group))
    return groups


def _find_node_near_col(nodes: List[Node], col: float, *,
                        max_row: int = 9999, min_row: int = -1,
                        direction: str = 'above') -> Optional[Node]:
    """Find the nearest node to a column, in the given row range."""
    candidates = []
    for n in nodes:
        if direction == 'above' and n.row <= max_row:
            candidates.append(n)
        elif direction == 'below' and n.row >= min_row:
            candidates.append(n)
    if not candidates:
        return None
    # Prefer closest column overlap, then closest row.
    def score(n):
        col_dist = abs(n.col_center - col)
        row_dist = (abs(n.row - max_row) if direction == 'above'
                    else abs(n.row - min_row))
        return (col_dist, row_dist)
    return min(candidates, key=score)


# ---------------------------------------------------------------------------
# Layout engine
# ---------------------------------------------------------------------------

def layout_flow(graph: FlowGraph):
    """Compute SVG layout positions for a flow graph."""
    if not graph.nodes:
        return

    # Step 1: Size each node.
    for n in graph.nodes:
        text_w = len(n.text) * CHAR_WIDTH
        n.w = text_w + NODE_PAD_X
        n.h = NODE_HEIGHT

    # Step 2: Identify the main flow row (row with most nodes on arrow chains).
    main_row = _find_main_flow_row(graph)

    # Step 3: Assign grid positions.
    # Nodes on the main flow row get sequential grid columns.
    main_nodes = sorted([n for n in graph.nodes if n.row == main_row],
                        key=lambda n: n.col_start)
    for i, n in enumerate(main_nodes):
        n.grid_row = 0
        n.grid_col = i

    # Non-main nodes: assign grid column based on which main-flow node they
    # are closest to (by column overlap or edge connection), grid row based
    # on their relative ASCII row.
    _assign_satellite_positions(graph, main_row, main_nodes)

    # Step 4: Assign colors (before pixel layout, since colors affect height).
    _assign_colors(graph, main_row)

    # Step 5: Compute pixel positions from grid.
    _compute_pixel_positions(graph, main_nodes)


def _find_main_flow_row(graph: FlowGraph) -> int:
    """Find the row number with the longest horizontal arrow chain."""
    row_counts: Dict[int, int] = {}
    for e in graph.edges:
        if e.direction == 'horizontal':
            src = graph.nodes[e.src]
            row_counts[src.row] = row_counts.get(src.row, 0) + 1
    if not row_counts:
        # Fallback: row with most nodes.
        for n in graph.nodes:
            row_counts[n.row] = row_counts.get(n.row, 0) + 1
    return max(row_counts, key=row_counts.get)


def _assign_satellite_positions(graph: FlowGraph, main_row: int,
                                main_nodes: List[Node]):
    """Assign grid positions to nodes not on the main flow row."""
    # Build a map of ASCII column to grid column from main flow nodes.
    col_map = {}
    for n in main_nodes:
        col_map[n.id] = n.grid_col

    # For each edge connecting a satellite to a main node, inherit grid_col.
    # Determine which satellite nodes have vertical vs horizontal connections.
    vert_connected: Set[int] = set()
    for e in graph.edges:
        if e.direction == 'vertical':
            vert_connected.add(e.src)
            vert_connected.add(e.dst)

    # Build edge lookup for satellites.
    main_ids = {n.id for n in main_nodes}

    for n in graph.nodes:
        if n.row == main_row:
            continue  # Already positioned.

        # Determine if this satellite is a source→main (multi-input) or
        # main→satellite (fan-out) via horizontal edges.
        fan_in_target = None   # main node this satellite feeds INTO
        fan_out_source = None  # main node this satellite receives FROM
        for e in graph.edges:
            if e.direction != 'horizontal':
                continue
            if e.src == n.id and e.dst in main_ids:
                fan_in_target = graph.nodes[e.dst]
            elif e.dst == n.id and e.src in main_ids:
                fan_out_source = graph.nodes[e.src]

        if fan_in_target is not None:
            # Multi-input: place one column left of the target (or col 0).
            n.grid_col = max(0, fan_in_target.grid_col - 1)
        elif fan_out_source is not None:
            # Fan-out: place one column right of the source.
            n.grid_col = fan_out_source.grid_col + 1
        elif main_nodes:
            # Fallback: use ASCII column proximity.
            closest = min(main_nodes,
                          key=lambda m: abs(m.col_center - n.col_center))
            n.grid_col = closest.grid_col
        else:
            n.grid_col = 0

        # Grid row: only use separate layers for vertically connected nodes.
        # Horizontally connected satellites (multi-input/fan-out) stay at
        # grid_row 0 and get stacked within their column.
        if n.id in vert_connected:
            if n.row < main_row:
                n.grid_row = n.row - main_row  # negative (above)
            else:
                n.grid_row = n.row - main_row  # positive (below)
        else:
            n.grid_row = 0  # same row, stacked vertically

    # Handle multi-node satellite rows (e.g., multi-input or fan-out).
    # Group satellite nodes by ASCII row, sort by col, assign sequential
    # grid columns if they differ from the main flow assignment.
    _adjust_satellite_columns(graph, main_row, main_nodes)


def _find_connected_main_nodes(node: Node, graph: FlowGraph,
                               main_row: int) -> List[Node]:
    """Find main-flow nodes connected to this satellite node."""
    result = []
    for e in graph.edges:
        if e.src == node.id:
            other = graph.nodes[e.dst]
            if other.row == main_row:
                result.append(other)
        elif e.dst == node.id:
            other = graph.nodes[e.src]
            if other.row == main_row:
                result.append(other)
    return result


def _adjust_satellite_columns(graph: FlowGraph, main_row: int,
                              main_nodes: List[Node]):
    """Adjust grid columns for satellite rows with multiple nodes."""
    sat_rows: Dict[int, List[Node]] = {}
    for n in graph.nodes:
        if n.row != main_row:
            sat_rows.setdefault(n.row, []).append(n)

    for row, nodes in sat_rows.items():
        if len(nodes) <= 1:
            continue
        nodes.sort(key=lambda n: n.col_start)
        # If they all have the same grid_col, space them out across
        # the columns they should span.
        for i, n in enumerate(nodes):
            # Find the grid column of their connection target.
            connected = _find_connected_main_nodes(n, graph, main_row)
            if connected:
                n.grid_col = connected[0].grid_col
            else:
                n.grid_col = i


def _compute_pixel_positions(graph: FlowGraph, main_nodes: List[Node]):
    """Compute pixel (x, y, w, h) from grid positions."""
    if not graph.nodes:
        return

    # Determine column widths: max node width per grid column.
    col_widths: Dict[int, float] = {}
    for n in graph.nodes:
        cur = col_widths.get(n.grid_col, 0)
        col_widths[n.grid_col] = max(cur, n.w)

    # Compute column X centers.
    sorted_cols = sorted(col_widths.keys())
    col_x: Dict[int, float] = {}
    x = PADDING
    for col in sorted_cols:
        half_w = col_widths[col] / 2
        col_x[col] = x + half_w
        x += col_widths[col] + ARROW_GAP

    # Determine row Y centers.
    sorted_rows = sorted(set(n.grid_row for n in graph.nodes))
    row_y: Dict[int, float] = {}
    # Main flow row (grid_row=0) first, then offset others.
    row_y[0] = PADDING + NODE_HEIGHT / 2
    # Count rows above and below.
    rows_above = sorted([r for r in sorted_rows if r < 0])
    rows_below = sorted([r for r in sorted_rows if r > 0])

    # Position rows above (top to bottom, above main).
    if rows_above:
        y = row_y[0] - (ROW_GAP + NODE_HEIGHT) * len(rows_above)
        for r in rows_above:
            row_y[r] = y + NODE_HEIGHT / 2
            y += NODE_HEIGHT + ROW_GAP
        # Shift everything down so top row starts at PADDING.
        min_y = min(row_y.values()) - NODE_HEIGHT / 2
        if min_y < PADDING:
            shift = PADDING - min_y
            for r in row_y:
                row_y[r] += shift

    # Position rows below.
    y = row_y[0] + NODE_HEIGHT / 2 + ROW_GAP
    for r in rows_below:
        row_y[r] = y + NODE_HEIGHT / 2
        y += NODE_HEIGHT + ROW_GAP

    # Handle multiple satellite nodes in the same grid column by stacking.
    # Group by (grid_row, grid_col) and offset.
    from collections import Counter
    cell_count = Counter((n.grid_row, n.grid_col) for n in graph.nodes)
    cell_index: Dict[Tuple[int, int], int] = {}

    for n in sorted(graph.nodes, key=lambda n: (n.grid_row, n.row, n.col_start)):
        cell = (n.grid_row, n.grid_col)
        idx = cell_index.get(cell, 0)
        cell_index[cell] = idx + 1

        n.x = col_x.get(n.grid_col, PADDING)
        n.y = row_y.get(n.grid_row, PADDING + NODE_HEIGHT / 2)

        # Stack multiple nodes in same cell vertically.
        if cell_count[cell] > 1:
            total = cell_count[cell]
            offset = (idx - (total - 1) / 2) * (NODE_HEIGHT + 6)
            n.y += offset

    # Normalize: shift all positions so min coordinates are at PADDING.
    if graph.nodes:
        min_x = min(n.x - n.w / 2 for n in graph.nodes)
        min_y = min(n.y - n.h / 2 for n in graph.nodes)
        shift_x = PADDING - min_x
        shift_y = PADDING - min_y
        for n in graph.nodes:
            n.x += shift_x
            n.y += shift_y


def _assign_colors(graph: FlowGraph, main_row: int):
    """Assign fill colors to nodes based on kind and position."""
    # First pass: assign based on kind.
    for n in graph.nodes:
        if n.kind == 'processor':
            n.fill = COLOR_PROCESSOR
        elif n.kind == 'conceptual':
            n.fill = COLOR_CONCEPTUAL
        else:
            n.fill = COLOR_ENDPOINT

    # Second pass: override based on vertical edges.
    for e in graph.edges:
        if e.direction != 'vertical':
            continue
        src = graph.nodes[e.src]
        dst = graph.nodes[e.dst]

        # Determine which is above/below main flow.
        if src.grid_row < 0:
            src.fill = COLOR_CONTROL
            src.h = NODE_SMALL_HEIGHT
        elif src.grid_row > 0:
            src.fill = COLOR_OBSERVATIONAL
            src.h = NODE_SMALL_HEIGHT
        if dst.grid_row < 0:
            dst.fill = COLOR_CONTROL
            dst.h = NODE_SMALL_HEIGHT
        elif dst.grid_row > 0:
            dst.fill = COLOR_OBSERVATIONAL
            dst.h = NODE_SMALL_HEIGHT


# ---------------------------------------------------------------------------
# SVG emitter
# ---------------------------------------------------------------------------

def emit_flow_svg(graph: FlowGraph) -> str:
    """Generate SVG markup for a flow graph."""
    if not graph.nodes:
        return ''

    # Compute bounding box.
    min_x = min(n.x - n.w / 2 for n in graph.nodes) - PADDING
    max_x = max(n.x + n.w / 2 for n in graph.nodes) + PADDING
    min_y = min(n.y - n.h / 2 for n in graph.nodes) - PADDING
    max_y = max(n.y + n.h / 2 for n in graph.nodes) + PADDING

    # Extend for labels below/beside nodes.
    for e in graph.edges:
        if e.label:
            max_y += LABEL_FONT_SIZE + 4

    vb_w = max_x - min_x
    vb_h = max_y - min_y

    lines = []
    lines.append(f'<svg viewBox="{min_x:.0f} {min_y:.0f} {vb_w:.0f} '
                 f'{vb_h:.0f}" xmlns="http://www.w3.org/2000/svg" '
                 f'font-family="monospace" font-size="{FONT_SIZE}">')

    # Defs: arrowhead markers.
    lines.append('  <defs>')
    lines.append('    <marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" '
                 'markerWidth="7" markerHeight="7" orient="auto-start-reverse">')
    lines.append(f'      <path d="M 0 0 L 10 5 L 0 10 z" fill="{COLOR_ARROW}"/>')
    lines.append('    </marker>')
    lines.append('    <marker id="arr-gray" viewBox="0 0 10 10" refX="9" '
                 'refY="5" markerWidth="7" markerHeight="7" '
                 'orient="auto-start-reverse">')
    lines.append(f'      <path d="M 0 0 L 10 5 L 0 10 z" '
                 f'fill="{COLOR_ARROW_GRAY}"/>')
    lines.append('    </marker>')
    lines.append('  </defs>')

    # Draw edges first (behind nodes).
    for e in graph.edges:
        _emit_edge(lines, e, graph)

    # Draw nodes.
    for n in graph.nodes:
        _emit_node(lines, n)

    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


def _emit_node(lines: List[str], n: Node):
    """Emit SVG elements for a single node."""
    rx = n.x - n.w / 2
    ry = n.y - n.h / 2
    lines.append(f'  <rect x="{rx:.0f}" y="{ry:.0f}" width="{n.w:.0f}" '
                 f'height="{n.h:.0f}" rx="{CORNER_RADIUS}" '
                 f'fill="{n.fill}" stroke="{COLOR_STROKE}"/>')
    # Text centered in box.
    text_escaped = _xml_escape(n.text)
    lines.append(f'  <text x="{n.x:.0f}" y="{n.y + 4:.0f}" '
                 f'text-anchor="middle">{text_escaped}</text>')


def _emit_edge(lines: List[str], e: Edge, graph: FlowGraph):
    """Emit SVG elements for a single edge."""
    src = graph.nodes[e.src]
    dst = graph.nodes[e.dst]

    if e.direction == 'horizontal':
        is_dashed = e.style == 'dashed'
        color = COLOR_ARROW_GRAY if is_dashed else COLOR_ARROW
        marker = 'arr-gray' if is_dashed else 'arr'
        dash = ' stroke-dasharray="5,3"' if is_dashed else ''

        # Determine left-to-right or right-to-left.
        if src.x < dst.x:
            x1 = src.x + src.w / 2
            x2 = dst.x - dst.w / 2
        else:
            x1 = src.x - src.w / 2
            x2 = dst.x + dst.w / 2

        y1 = src.y
        y2 = dst.y

        lines.append(f'  <line x1="{x1:.0f}" y1="{y1:.0f}" '
                     f'x2="{x2:.0f}" y2="{y2:.0f}" '
                     f'stroke="{color}"{dash} '
                     f'marker-end="url(#{marker})"/>')

        if e.label:
            mid_x = (x1 + x2) / 2
            mid_y = (y1 + y2) / 2 - 6
            label_escaped = _xml_escape(e.label)
            lines.append(f'  <text x="{mid_x:.0f}" y="{mid_y:.0f}" '
                         f'text-anchor="middle" fill="{COLOR_LABEL}" '
                         f'font-size="{LABEL_FONT_SIZE}">'
                         f'{label_escaped}</text>')

    elif e.direction == 'vertical':
        # Vertical: dashed gray arrow.
        if src.y < dst.y:
            x1 = src.x
            y1 = src.y + src.h / 2
            x2 = dst.x
            y2 = dst.y - dst.h / 2
        else:
            x1 = src.x
            y1 = src.y - src.h / 2
            x2 = dst.x
            y2 = dst.y + dst.h / 2

        lines.append(f'  <line x1="{x1:.0f}" y1="{y1:.0f}" '
                     f'x2="{x2:.0f}" y2="{y2:.0f}" '
                     f'stroke="{COLOR_ARROW_GRAY}" '
                     f'stroke-dasharray="5,3" '
                     f'marker-end="url(#arr-gray)"/>')

        if e.label:
            mid_x = max(x1, x2) + 8
            mid_y = (y1 + y2) / 2 + 3
            label_escaped = _xml_escape(e.label)
            lines.append(f'  <text x="{mid_x:.0f}" y="{mid_y:.0f}" '
                         f'fill="{COLOR_LABEL}" '
                         f'font-size="{LABEL_FONT_SIZE}">'
                         f'{label_escaped}</text>')


def _xml_escape(text: str) -> str:
    """Escape text for XML content."""
    return (text.replace('&', '&amp;')
                .replace('<', '&lt;')
                .replace('>', '&gt;')
                .replace('"', '&quot;'))


# ---------------------------------------------------------------------------
# Markdown scanner
# ---------------------------------------------------------------------------

BLOCK_RE = re.compile(
    r'<!--\s*(csp-flow|csp-state|csp-seq|csp-chart|csp-layers)\s*\n'
    r'(.*?)\n\s*-->',
    re.DOTALL)

IMAGE_RE = re.compile(r'!\[([^\]]*)\]\(([^)]+\.svg)\)')


@dataclass
class DiagramBlock:
    kind: str         # 'csp-flow', etc.
    text: str         # the DSL source
    svg_path: str     # relative path to output SVG
    md_path: str      # source markdown file


def scan_markdown(md_path: str) -> List[DiagramBlock]:
    """Find all csp-* diagram blocks in a markdown file."""
    with open(md_path, 'r') as f:
        content = f.read()

    blocks = []
    for m in BLOCK_RE.finditer(content):
        kind = m.group(1)
        text = m.group(2)
        # Find the image reference after the comment block.
        rest = content[m.end():]
        img_match = IMAGE_RE.search(rest[:200])  # Look within 200 chars.
        if img_match:
            svg_rel = img_match.group(2)
            # Resolve relative to the markdown file's directory.
            md_dir = os.path.dirname(md_path)
            svg_path = os.path.normpath(os.path.join(md_dir, svg_rel))
            blocks.append(DiagramBlock(kind=kind, text=text,
                                       svg_path=svg_path, md_path=md_path))
        else:
            print(f"WARNING: {md_path}: {kind} block has no ![](*.svg) ref",
                  file=sys.stderr)
    return blocks


def scan_all(docs_dir: str) -> List[DiagramBlock]:
    """Scan all markdown files under a directory."""
    blocks = []
    for root, dirs, files in os.walk(docs_dir):
        for fn in files:
            if fn.endswith('.md'):
                path = os.path.join(root, fn)
                blocks.extend(scan_markdown(path))
    return blocks


# ---------------------------------------------------------------------------
# State diagram parser (csp-state)
# ---------------------------------------------------------------------------

@dataclass
class StateNode:
    name: str
    label: str        # display text (may differ from name)
    is_initial: bool = False
    is_terminal: bool = False
    x: float = 0
    y: float = 0
    w: float = 0
    h: float = 0


@dataclass
class StateEdge:
    src: str    # state name (or '[*]')
    dst: str
    label: str = ''


@dataclass
class StateGraph:
    nodes: List[StateNode] = field(default_factory=list)
    edges: List[StateEdge] = field(default_factory=list)


STATE_EDGE_RE = re.compile(
    r'^\s*(\[?\*?\]?[\w]+(?:\s*\])?)\s*-+>\s*(\[?\*?\]?[\w]+(?:\s*\])?)'
    r'\s*(?::\s*(.+))?$')

STATE_ALIAS_RE = re.compile(
    r'^\s*state\s+"([^"]+)"\s+as\s+(\w+)')


def parse_state_graph(text: str) -> StateGraph:
    """Parse csp-state edge-list DSL into a StateGraph."""
    graph = StateGraph()
    seen: Dict[str, StateNode] = {}
    aliases: Dict[str, str] = {}

    for line in text.strip().split('\n'):
        line = line.strip()
        if not line:
            continue

        # Alias: state "label" as name
        m = STATE_ALIAS_RE.match(line)
        if m:
            aliases[m.group(2)] = m.group(1)
            continue

        # Edge: A -> B : label  (or  A --> B : label)
        m = re.match(
            r'\s*(\[\*\]|\w+)\s*-+>\s*(\[\*\]|\w+)\s*(?::\s*(.+))?', line)
        if not m:
            continue

        src_name = m.group(1)
        dst_name = m.group(2)
        label = (m.group(3) or '').strip()

        for name in (src_name, dst_name):
            if name not in seen:
                is_star = name == '[*]'
                display = aliases.get(name, name)
                node = StateNode(
                    name=name,
                    label=display if not is_star else '',
                    is_initial=is_star,
                    is_terminal=is_star)
                seen[name] = node
                graph.nodes.append(node)

        graph.edges.append(StateEdge(src_name, dst_name, label))

    return graph


STATE_NODE_HEIGHT = 30
STATE_NODE_PAD = 20
STATE_COL_GAP = 60
STATE_ROW_GAP = 50
STATE_CIRCLE_R = 8
STATE_COLOR = '#d4e6f1'


def layout_state(graph: StateGraph):
    """Compute positions for state graph nodes using longest-path layering."""
    if not graph.nodes:
        return

    # Build adjacency.
    out_adj: Dict[str, List[str]] = {}
    for e in graph.edges:
        out_adj.setdefault(e.src, []).append(e.dst)

    # Assign layers via BFS from initial states ([*] sources).
    layers: Dict[str, int] = {}
    initial = [n.name for n in graph.nodes if n.is_initial]
    if not initial:
        initial = [graph.nodes[0].name]

    queue = list(initial)
    for name in queue:
        if name not in layers:
            layers[name] = 0

    visited = set()
    q = list(initial)
    while q:
        cur = q.pop(0)
        if cur in visited:
            continue
        visited.add(cur)
        for nxt in out_adj.get(cur, []):
            new_layer = layers[cur] + 1
            if nxt not in layers or new_layer > layers[nxt]:
                layers[nxt] = new_layer
            q.append(nxt)

    # For any unassigned, place after maximum.
    max_layer = max(layers.values()) if layers else 0
    for n in graph.nodes:
        if n.name not in layers:
            max_layer += 1
            layers[n.name] = max_layer

    # Group nodes by layer.
    layer_groups: Dict[int, List[StateNode]] = {}
    for n in graph.nodes:
        layer_groups.setdefault(layers[n.name], []).append(n)

    # Size each node.
    for n in graph.nodes:
        if n.is_initial or n.is_terminal:
            n.w = STATE_CIRCLE_R * 2
            n.h = STATE_CIRCLE_R * 2
        else:
            text_w = len(n.label) * CHAR_WIDTH
            n.w = text_w + STATE_NODE_PAD
            n.h = STATE_NODE_HEIGHT

    # Compute column widths.
    sorted_layers = sorted(layer_groups.keys())
    col_widths = {}
    for layer in sorted_layers:
        col_widths[layer] = max(n.w for n in layer_groups[layer])

    # X positions.
    x = PADDING
    col_x = {}
    for layer in sorted_layers:
        half = col_widths[layer] / 2
        col_x[layer] = x + half
        x += col_widths[layer] + STATE_COL_GAP

    # Y positions (stack within each column).
    for layer in sorted_layers:
        nodes = layer_groups[layer]
        total_h = sum(n.h for n in nodes) + STATE_ROW_GAP * (len(nodes) - 1)
        y = PADDING + total_h / 2
        # Center vertically relative to the tallest column
        start_y = PADDING
        for n in nodes:
            n.x = col_x[layer]
            n.y = start_y + n.h / 2
            start_y += n.h + STATE_ROW_GAP

    # Normalize to PADDING.
    if graph.nodes:
        min_x = min(n.x - n.w / 2 for n in graph.nodes)
        min_y = min(n.y - n.h / 2 for n in graph.nodes)
        for n in graph.nodes:
            n.x += PADDING - min_x
            n.y += PADDING - min_y


def emit_state_svg(graph: StateGraph) -> str:
    """Generate SVG for a state diagram."""
    if not graph.nodes:
        return ''

    # Bounding box.
    min_x = min(n.x - n.w / 2 for n in graph.nodes) - PADDING
    max_x = max(n.x + n.w / 2 for n in graph.nodes) + PADDING
    min_y = min(n.y - n.h / 2 for n in graph.nodes) - PADDING
    max_y = max(n.y + n.h / 2 for n in graph.nodes) + PADDING

    # Extend for edge labels.
    label_space = sum(1 for e in graph.edges if e.label) * 6
    max_y += label_space

    vb_w = max_x - min_x
    vb_h = max_y - min_y

    lines = []
    lines.append(f'<svg viewBox="{min_x:.0f} {min_y:.0f} {vb_w:.0f} '
                 f'{vb_h:.0f}" xmlns="http://www.w3.org/2000/svg" '
                 f'font-family="monospace" font-size="{FONT_SIZE}">')

    lines.append('  <defs>')
    lines.append('    <marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" '
                 'markerWidth="7" markerHeight="7" orient="auto-start-reverse">')
    lines.append(f'      <path d="M 0 0 L 10 5 L 0 10 z" fill="{COLOR_ARROW}"/>')
    lines.append('    </marker>')
    lines.append('  </defs>')

    node_by_name = {n.name: n for n in graph.nodes}

    # Draw edges.
    for e in graph.edges:
        src = node_by_name.get(e.src)
        dst = node_by_name.get(e.dst)
        if not src or not dst:
            continue

        # Compute connection points.
        if src.x < dst.x:
            x1 = src.x + src.w / 2
            x2 = dst.x - dst.w / 2
        elif src.x > dst.x:
            x1 = src.x - src.w / 2
            x2 = dst.x + dst.w / 2
        else:
            x1 = src.x
            x2 = dst.x

        y1 = src.y
        y2 = dst.y

        # Self-loops: draw as arc above the node.
        if e.src == e.dst:
            cx = src.x
            top = src.y - src.h / 2 - 20
            lines.append(
                f'  <path d="M {cx - 15:.0f} {src.y - src.h / 2:.0f} '
                f'C {cx - 25:.0f} {top:.0f} {cx + 25:.0f} {top:.0f} '
                f'{cx + 15:.0f} {src.y - src.h / 2:.0f}" '
                f'fill="none" stroke="{COLOR_ARROW}" '
                f'marker-end="url(#arr)"/>')
            if e.label:
                label_escaped = _xml_escape(e.label)
                lines.append(
                    f'  <text x="{cx:.0f}" y="{top - 4:.0f}" '
                    f'text-anchor="middle" fill="{COLOR_LABEL}" '
                    f'font-size="{LABEL_FONT_SIZE}">{label_escaped}</text>')
            continue

        lines.append(
            f'  <line x1="{x1:.0f}" y1="{y1:.0f}" '
            f'x2="{x2:.0f}" y2="{y2:.0f}" '
            f'stroke="{COLOR_ARROW}" marker-end="url(#arr)"/>')

        if e.label:
            mid_x = (x1 + x2) / 2
            mid_y = (y1 + y2) / 2 - 6
            label_escaped = _xml_escape(e.label)
            lines.append(
                f'  <text x="{mid_x:.0f}" y="{mid_y:.0f}" '
                f'text-anchor="middle" fill="{COLOR_LABEL}" '
                f'font-size="{LABEL_FONT_SIZE}">{label_escaped}</text>')

    # Draw nodes.
    for n in graph.nodes:
        if n.is_initial or n.is_terminal:
            # Filled circle for [*]
            lines.append(
                f'  <circle cx="{n.x:.0f}" cy="{n.y:.0f}" r="{STATE_CIRCLE_R}" '
                f'fill="{COLOR_ARROW}" stroke="{COLOR_ARROW}"/>')
        else:
            rx = n.x - n.w / 2
            ry = n.y - n.h / 2
            lines.append(
                f'  <rect x="{rx:.0f}" y="{ry:.0f}" width="{n.w:.0f}" '
                f'height="{n.h:.0f}" rx="12" '
                f'fill="{STATE_COLOR}" stroke="{COLOR_STROKE}"/>')
            label_escaped = _xml_escape(n.label)
            lines.append(
                f'  <text x="{n.x:.0f}" y="{n.y + 4:.0f}" '
                f'text-anchor="middle">{label_escaped}</text>')

    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------
# Sequence diagram parser (csp-seq)
# ---------------------------------------------------------------------------

@dataclass
class SeqParticipant:
    id: str
    label: str
    x: float = 0


@dataclass
class SeqEvent:
    kind: str       # 'arrow', 'arrow_dashed', 'note'
    src: str        # participant id (or '' for notes)
    dst: str
    label: str = ''
    y: float = 0


@dataclass
class SeqDiagram:
    participants: List[SeqParticipant] = field(default_factory=list)
    events: List[SeqEvent] = field(default_factory=list)


SEQ_ROW_HEIGHT = 30
SEQ_PAD_X = 40


def parse_seq_diagram(text: str) -> SeqDiagram:
    """Parse csp-seq DSL into a SeqDiagram."""
    diagram = SeqDiagram()

    for line in text.strip().split('\n'):
        line = line.strip()
        if not line:
            continue

        # Participant line: A "Label" | B "Label" | ...
        if '|' in line and not line.startswith('note'):
            parts = [p.strip() for p in line.split('|')]
            for p in parts:
                m = re.match(r'(\w+)\s+"([^"]+)"', p)
                if m:
                    diagram.participants.append(
                        SeqParticipant(id=m.group(1), label=m.group(2)))
                elif re.match(r'\w+', p):
                    diagram.participants.append(
                        SeqParticipant(id=p.strip(), label=p.strip()))
            continue

        # Arrow: A ->> B : label  (solid)
        m = re.match(r'(\w+)\s+->>?\s+(\w+)\s*:\s*(.*)', line)
        if m:
            diagram.events.append(SeqEvent(
                'arrow', m.group(1), m.group(2), m.group(3).strip()))
            continue

        # Dashed arrow: A -->> B : label
        m = re.match(r'(\w+)\s+-->>?\s+(\w+)\s*:\s*(.*)', line)
        if m:
            diagram.events.append(SeqEvent(
                'arrow_dashed', m.group(1), m.group(2), m.group(3).strip()))
            continue

        # Note: note A : text   or   note A,B : text
        m = re.match(r'note\s+([\w,]+)\s*:\s*(.*)', line)
        if m:
            ids = m.group(1)
            diagram.events.append(SeqEvent(
                'note', ids, '', m.group(2).strip()))
            continue

    return diagram


def layout_seq(diagram: SeqDiagram):
    """Compute positions for sequence diagram."""
    if not diagram.participants:
        return

    # X positions for participants.
    x = PADDING + SEQ_PAD_X
    for p in diagram.participants:
        text_w = len(p.label) * CHAR_WIDTH + 20
        p.x = x + text_w / 2
        x += text_w + SEQ_PAD_X

    # Y positions for events.
    y = PADDING + NODE_HEIGHT + 15
    for ev in diagram.events:
        y += SEQ_ROW_HEIGHT
        ev.y = y


def emit_seq_svg(diagram: SeqDiagram) -> str:
    """Generate SVG for a sequence diagram."""
    if not diagram.participants or not diagram.events:
        return ''

    p_by_id = {p.id: p for p in diagram.participants}
    max_y = max(ev.y for ev in diagram.events) + SEQ_ROW_HEIGHT
    min_x = min(p.x - 50 for p in diagram.participants)
    max_x = max(p.x + 50 for p in diagram.participants)

    vb_w = max_x - min_x + PADDING * 2
    vb_h = max_y + PADDING

    lines = []
    lines.append(f'<svg viewBox="0 0 {vb_w:.0f} {vb_h:.0f}" '
                 f'xmlns="http://www.w3.org/2000/svg" '
                 f'font-family="monospace" font-size="{FONT_SIZE}">')

    lines.append('  <defs>')
    lines.append('    <marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" '
                 'markerWidth="7" markerHeight="7" orient="auto-start-reverse">')
    lines.append(f'      <path d="M 0 0 L 10 5 L 0 10 z" fill="{COLOR_ARROW}"/>')
    lines.append('    </marker>')
    lines.append('    <marker id="arr-gray" viewBox="0 0 10 10" refX="9" '
                 'refY="5" markerWidth="7" markerHeight="7" '
                 'orient="auto-start-reverse">')
    lines.append(f'      <path d="M 0 0 L 10 5 L 0 10 z" '
                 f'fill="{COLOR_ARROW_GRAY}"/>')
    lines.append('    </marker>')
    lines.append('  </defs>')

    # Lifelines.
    for p in diagram.participants:
        px = p.x - min_x + PADDING
        # Header box.
        text_w = len(p.label) * CHAR_WIDTH + 20
        lines.append(
            f'  <rect x="{px - text_w / 2:.0f}" y="{PADDING}" '
            f'width="{text_w:.0f}" height="{NODE_HEIGHT}" rx="{CORNER_RADIUS}" '
            f'fill="{COLOR_ENDPOINT}" stroke="{COLOR_STROKE}"/>')
        label_escaped = _xml_escape(p.label)
        lines.append(
            f'  <text x="{px:.0f}" y="{PADDING + NODE_HEIGHT / 2 + 4:.0f}" '
            f'text-anchor="middle">{label_escaped}</text>')
        # Lifeline.
        lines.append(
            f'  <line x1="{px:.0f}" y1="{PADDING + NODE_HEIGHT:.0f}" '
            f'x2="{px:.0f}" y2="{max_y:.0f}" '
            f'stroke="{COLOR_STROKE}" stroke-dasharray="5,3"/>')

    # Events.
    for ev in diagram.events:
        if ev.kind in ('arrow', 'arrow_dashed'):
            src_p = p_by_id.get(ev.src)
            dst_p = p_by_id.get(ev.dst)
            if not src_p or not dst_p:
                continue
            sx = src_p.x - min_x + PADDING
            dx = dst_p.x - min_x + PADDING
            dashed = ev.kind == 'arrow_dashed'
            color = COLOR_ARROW_GRAY if dashed else COLOR_ARROW
            marker = 'arr-gray' if dashed else 'arr'
            dash = ' stroke-dasharray="5,3"' if dashed else ''
            lines.append(
                f'  <line x1="{sx:.0f}" y1="{ev.y:.0f}" '
                f'x2="{dx:.0f}" y2="{ev.y:.0f}" '
                f'stroke="{color}"{dash} marker-end="url(#{marker})"/>')
            if ev.label:
                mid = (sx + dx) / 2
                label_escaped = _xml_escape(ev.label)
                lines.append(
                    f'  <text x="{mid:.0f}" y="{ev.y - 6:.0f}" '
                    f'text-anchor="middle" font-size="{LABEL_FONT_SIZE}">'
                    f'{label_escaped}</text>')

        elif ev.kind == 'note':
            ids = [i.strip() for i in ev.src.split(',')]
            xs = [p_by_id[i].x - min_x + PADDING for i in ids if i in p_by_id]
            if xs:
                cx = sum(xs) / len(xs)
                label_escaped = _xml_escape(ev.label)
                lines.append(
                    f'  <text x="{cx:.0f}" y="{ev.y:.0f}" '
                    f'text-anchor="middle" fill="{COLOR_LABEL}" '
                    f'font-style="italic" font-size="{LABEL_FONT_SIZE}">'
                    f'{label_escaped}</text>')

    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def generate(block: DiagramBlock) -> bool:
    """Generate SVG for a single diagram block.  Returns True on success."""
    svg = ''
    if block.kind == 'csp-flow':
        token_rows = tokenize(block.text)
        graph = build_flow_graph(token_rows)
        layout_flow(graph)
        svg = emit_flow_svg(graph)
    elif block.kind == 'csp-state':
        graph = parse_state_graph(block.text)
        layout_state(graph)
        svg = emit_state_svg(graph)
    elif block.kind == 'csp-seq':
        diagram = parse_seq_diagram(block.text)
        layout_seq(diagram)
        svg = emit_seq_svg(diagram)
    else:
        print(f"WARNING: {block.kind} not yet implemented ({block.md_path})",
              file=sys.stderr)
        return False

    if not svg:
        print(f"WARNING: empty SVG for {block.svg_path}", file=sys.stderr)
        return False
    os.makedirs(os.path.dirname(block.svg_path), exist_ok=True)
    with open(block.svg_path, 'w') as f:
        f.write(svg)
    return True


def main():
    repo_root = Path(__file__).resolve().parent.parent
    docs_dir = repo_root / 'docs'

    blocks = scan_all(str(docs_dir))
    if not blocks:
        print("No diagram blocks found.")
        return 0

    ok = 0
    fail = 0
    for b in blocks:
        if generate(b):
            ok += 1
        else:
            fail += 1

    print(f"Generated: {ok}  Failed: {fail}")
    return 1 if fail > 0 else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""One-time script: convert Mermaid diagrams in docs/reference/parts/*.md
to csp-flow ASCII art DSL comment blocks + ![](diagrams/*.svg) refs.

State diagrams (stateDiagram-v2) are skipped — handled later by csp-state.
"""

import re
import sys
from pathlib import Path
from collections import defaultdict
from typing import Dict, List, Tuple, Optional, Set

PARTS_DIR = Path(__file__).resolve().parent.parent / 'docs' / 'reference' / 'parts'

# ── Mermaid parser ──────────────────────────────────────────────────────────

def parse_mermaid(text: str):
    """Return (nodes, edges, styles) from a Mermaid graph LR block.
    nodes: {id: display_text}
    edges: [(src, dst, label, dashed)]
    styles: {id: {prop: val}}
    """
    nodes: Dict[str, str] = {}
    edges: List[Tuple[str, str, str, bool]] = []
    styles: Dict[str, Dict[str, str]] = {}

    for line in text.strip().split('\n'):
        line = line.strip()
        if not line or line.startswith('graph'):
            continue

        # style ID prop:val, ...
        m = re.match(r'style\s+(\w+)\s+(.*)', line)
        if m:
            props = {}
            for p in m.group(2).split(','):
                if ':' in p:
                    k, v = p.split(':', 1)
                    props[k.strip()] = v.strip()
            styles[m.group(1)] = props
            continue

        # Extract all inline node definitions: ID["text"] or ID[text]
        for m in re.finditer(r'(\w+)\s*\[(?:"([^"]+)"|([^\]]+))\]', line):
            nid = m.group(1)
            ntext = m.group(2) if m.group(2) is not None else m.group(3)
            ntext = re.sub(r'<br\s*/?>', ' ', ntext)
            # Unescape HTML entities
            ntext = ntext.replace('&lt;', '<').replace('&gt;', '>')
            ntext = ntext.replace('&amp;', '&')
            if nid not in nodes:
                nodes[nid] = ntext

        # Strip node definitions [text] so edge regexes see bare IDs:
        #   A[reader<T>] --> B["map(f)"] --> C[reader<T>]
        # becomes:
        #   A --> B --> C
        stripped = re.sub(r'\s*\[(?:"[^"]*"|[^\]]*)\]', '', line)

        # ── Extract edges on stripped line ──

        # A -->|"label"| B  or  A -->|label| B
        for m in re.finditer(r'(\w+)\s+-->\|"?([^"|]*)"?\|\s+(\w+)', stripped):
            edges.append((m.group(1), m.group(3), m.group(2), False))

        # A -- "label" --> B
        for m in re.finditer(r'(\w+)\s+--\s+"([^"]*)"\s+-->\s+(\w+)', stripped):
            edges.append((m.group(1), m.group(3), m.group(2), False))

        # A -.->|label| B  (dashed with pipe-delimited label)
        for m in re.finditer(r'(\w+)\s+-\.->(?:\|"?([^"|]*)"?\|)?\s+(\w+)', stripped):
            edges.append((m.group(1), m.group(3), m.group(2) or '', True))

        # A -. text .-> B  (dashed with inline label)
        for m in re.finditer(r'(\w+)\s+-\.\s+([^.]+?)\s+\.->(?:\|"?([^"|]*)"?\|)?\s+(\w+)', stripped):
            edges.append((m.group(1), m.group(4), m.group(2).strip(), True))

        # Simple solid: A --> B (handle chains by finding all pairs)
        # Remove already-matched labeled edges
        cleaned = stripped
        cleaned = re.sub(r'\w+\s+-->\|"?[^"|]*"?\|\s+\w+', 'X', cleaned)
        cleaned = re.sub(r'\w+\s+--\s+"[^"]*"\s+-->\s+\w+', 'X', cleaned)
        cleaned = re.sub(r'\w+\s+-\..*?\.->(?:\|[^|]*\|)?\s+\w+', 'X', cleaned)
        cleaned = re.sub(r'\w+\s+-\.->(?:\|[^|]*\|)?\s+\w+', 'X', cleaned)
        for m in re.finditer(r'(\w+)\s+-->\s+(\w+)', cleaned):
            edges.append((m.group(1), m.group(2), '', False))

    return nodes, edges, styles


# ── Topology analysis ───────────────────────────────────────────────────────

KNOWN_PROCS = {
    'merge', 'zip', 'unzip', 'interleave', 'chain', 'mux', 'demux',
    'fanout', 'share', 'gate', 'sample', 'pairwise', 'flatten',
    'blackhole', 'tee', 'deaf', 'mute', 'transpose', 'fallback',
    'first_wins', 'join', 'killswitch',
}


def is_processor(nid: str, text: str, styles: dict) -> bool:
    fill = styles.get(nid, {}).get('fill', '')
    if fill == '#f5d6a8':
        return True
    clean = re.sub(r'\s*\(.*', '', text)
    if clean.strip() in KNOWN_PROCS:
        return True
    if re.match(r'\w+\(', text.strip()):
        return True
    if ' imp' in text or ' MT' in text:
        return True
    return False


def is_control(nid: str, styles: dict) -> bool:
    return styles.get(nid, {}).get('fill') == '#d4edda'


def node_dsl(text: str, proc: bool) -> str:
    """Convert a Mermaid node's display text to DSL representation."""
    # Remove annotations like (data), (control), (source), (trigger), (sorted)
    # BUT keep (sorted) if it's informational
    text = re.sub(r'\s*\((data|control|source|trigger)\)', '', text)
    text = text.strip()
    # Remove " imp" / " MT" suffixes
    text = re.sub(r'\s+(imp|MT)$', '', text)
    if proc:
        return '{' + text + '}'
    return text


def find_main_chain(nodes, edges):
    """Find the longest chain of solid edges."""
    out = defaultdict(list)
    for src, dst, lab, dashed in edges:
        if not dashed:
            out[src].append(dst)
    # Find sources
    has_in = set()
    for _, dst, _, dashed in edges:
        if not dashed:
            has_in.add(dst)
    sources = [nid for nid in nodes if nid not in has_in]
    if not sources:
        sources = list(nodes.keys())

    def longest(start, visited=None):
        if visited is None:
            visited = set()
        visited.add(start)
        best = [start]
        for nxt in out.get(start, []):
            if nxt not in visited:
                cand = [start] + longest(nxt, visited.copy())
                if len(cand) > len(best):
                    best = cand
        return best

    best_chain = []
    for s in sources:
        c = longest(s)
        if len(c) > len(best_chain):
            best_chain = c
    return best_chain


# ── DSL generation ──────────────────────────────────────────────────────────

def generate_simple_lr(chain, nodes, styles):
    """input -> {proc} -> output   (or subset)."""
    parts = []
    for nid in chain:
        text = nodes[nid]
        proc = is_processor(nid, text, styles)
        parts.append(node_dsl(text, proc))
    return ' -> '.join(parts)


def generate_multi_input(chain, satellites_in, nodes, edges, styles):
    """
    sat1  ->
    sat2  -> {proc} -> output
    sat3  ->
    """
    # Find the merge point (first chain node that receives satellite input)
    merge_idx = 0
    chain_set = set(chain)
    for i, nid in enumerate(chain):
        if any(src in satellites_in for src, dst, _, dashed in edges
               if dst == nid and not dashed):
            merge_idx = i
            break

    # Include chain nodes before merge point as additional inputs
    all_inputs = set(satellites_in)
    for nid in chain[:merge_idx]:
        all_inputs.add(nid)

    # Build satellite list (sorted by original Mermaid order)
    sats = sorted(all_inputs, key=lambda s: list(nodes.keys()).index(s)
                  if s in nodes else 0)

    # Build main chain after merge point
    chain_after = chain[merge_idx:]
    chain_dsl = ' -> '.join(
        node_dsl(nodes[nid], is_processor(nid, nodes[nid], styles))
        for nid in chain_after)

    # Build lines
    sat_texts = [node_dsl(nodes[s], is_processor(s, nodes[s], styles)) for s in sats]
    max_len = max(len(t) for t in sat_texts) if sat_texts else 0

    lines = []
    mid = len(sat_texts) // 2
    for i, st in enumerate(sat_texts):
        pad = ' ' * (max_len - len(st))
        if i == mid:
            lines.append(f'{st}{pad} -> {chain_dsl}')
        else:
            lines.append(f'{st}{pad} ->')
    return '\n'.join(lines)


def generate_fan_out(chain, satellites_out, nodes, edges, styles):
    """
                        -> out1
    input -> {proc}  -> out2
                        -> out3
    """
    # Find the fork point
    fork_idx = len(chain) - 1
    for i, nid in enumerate(chain):
        if any(dst in satellites_out for _, dst, _, dashed in edges
               if _ == nid and not dashed):
            fork_idx = i
            break

    chain_before = chain[:fork_idx + 1]
    prefix = ' -> '.join(
        node_dsl(nodes[nid], is_processor(nid, nodes[nid], styles))
        for nid in chain_before)

    sats = sorted(satellites_out, key=lambda s: list(nodes.keys()).index(s)
                  if s in nodes else 0)
    sat_texts = [node_dsl(nodes[s], is_processor(s, nodes[s], styles)) for s in sats]

    # Check if edges to satellites have labels
    sat_labels = {}
    for src, dst, label, dashed in edges:
        if dst in satellites_out:
            sat_labels[dst] = (label, dashed)

    # Also include chain nodes after fork
    chain_after_texts = []
    for nid in chain[fork_idx + 1:]:
        chain_after_texts.append(
            node_dsl(nodes[nid], is_processor(nid, nodes[nid], styles)))

    # Build output list: chain continuation + satellites
    all_outputs = []
    # Chain continuation first (main output)
    for t in chain_after_texts:
        all_outputs.append((t, '', False))
    for s, st in zip(sats, sat_texts):
        label, dashed = sat_labels.get(s, ('', False))
        all_outputs.append((st, label, dashed))

    padding = ' ' * len(prefix) + ' '
    mid = len(all_outputs) // 2
    lines = []
    for i, (out_text, label, dashed) in enumerate(all_outputs):
        if dashed:
            arrow = f'.."{ label }"..>' if label else '..>'
        elif label:
            arrow = f'-"{label}"->'
        else:
            arrow = '->'
        if i == mid:
            lines.append(f'{prefix} {arrow} {out_text}')
        else:
            lines.append(f'{padding}{arrow} {out_text}')
    return '\n'.join(lines)


def generate_control_above(chain, ctrl_id, nodes, styles, ctrl_label=''):
    """
           ctrl_node
                |
    input -> {proc} -> output
    """
    # Build main chain DSL
    main_dsl = ' -> '.join(
        node_dsl(nodes[nid], is_processor(nid, nodes[nid], styles))
        for nid in chain)

    # Find where the control connects (find the chain node it feeds into)
    ctrl_text = node_dsl(nodes[ctrl_id], False)

    # Center the control above the processor (middle node in chain)
    # Find the processor position in the main_dsl string
    proc_idx = len(chain) // 2
    proc_nid = chain[proc_idx]
    proc_text = node_dsl(nodes[proc_nid], is_processor(proc_nid, nodes[proc_nid], styles))

    # Calculate position: find where proc_text starts in main_dsl
    proc_start = main_dsl.find(proc_text)
    proc_center = proc_start + len(proc_text) // 2

    # Position the | under the control node center
    pipe_col = proc_center
    ctrl_col = pipe_col - len(ctrl_text) // 2

    ctrl_pad = ' ' * max(0, ctrl_col)
    pipe_pad = ' ' * max(0, pipe_col)

    lines = [
        f'{ctrl_pad}{ctrl_text}',
        f'{pipe_pad}|',
    ]
    if ctrl_label:
        label_pad = ' ' * max(0, pipe_col - len(ctrl_label) // 2)
        lines.insert(1, f'{label_pad}"{ctrl_label}"')
        lines.insert(2, f'{pipe_pad}|')

    lines.append(main_dsl)
    return '\n'.join(lines)


def generate_observational_below(chain, obs_id, nodes, styles, obs_label=''):
    """
    input -> {proc} -> output
                 |
       obs_node
    """
    main_dsl = ' -> '.join(
        node_dsl(nodes[nid], is_processor(nid, nodes[nid], styles))
        for nid in chain)

    obs_text = node_dsl(nodes[obs_id], False)

    # Find the processor (connected to obs) in chain
    proc_idx = len(chain) // 2
    proc_nid = chain[proc_idx]
    proc_text = node_dsl(nodes[proc_nid], is_processor(proc_nid, nodes[proc_nid], styles))

    proc_start = main_dsl.find(proc_text)
    proc_center = proc_start + len(proc_text) // 2

    pipe_pad = ' ' * proc_center
    obs_pad = ' ' * max(0, proc_center - len(obs_text) // 2)

    lines = [main_dsl, f'{pipe_pad}|']
    if obs_label:
        label_pad = ' ' * max(0, proc_center - len(obs_label) // 2)
        lines.append(f'{label_pad}"{obs_label}"')
    lines.append(f'{obs_pad}{obs_text}')
    return '\n'.join(lines)


# ── Master converter ────────────────────────────────────────────────────────

def mermaid_to_dsl(mermaid_text: str) -> Optional[str]:
    """Convert a Mermaid graph LR block to csp-flow DSL.  Returns None on skip."""
    if 'stateDiagram' in mermaid_text:
        return None  # Skip state diagrams

    nodes, edges, styles = parse_mermaid(mermaid_text)
    if not nodes:
        return None

    chain = find_main_chain(nodes, edges)
    if not chain:
        return None

    chain_set = set(chain)

    # Categorize non-chain nodes
    out_adj = defaultdict(list)
    in_adj = defaultdict(list)
    for src, dst, label, dashed in edges:
        out_adj[src].append((dst, label, dashed))
        in_adj[dst].append((src, label, dashed))

    # Satellites feeding INTO chain nodes (multi-input)
    satellites_in = set()
    # Satellites receiving FROM chain nodes (fan-out)
    satellites_out = set()
    # Control nodes
    ctrl_nodes = set()
    # Observational nodes
    obs_nodes = set()

    for nid in nodes:
        if nid in chain_set:
            continue
        if is_control(nid, styles):
            ctrl_nodes.add(nid)
            continue
        # Check if it feeds into a chain node
        feeds_chain = any(dst in chain_set for dst, _, dashed in out_adj[nid] if not dashed)
        fed_by_chain = any(src in chain_set for src, _, dashed in in_adj[nid] if not dashed)
        dashed_from_chain = any(src in chain_set for src, _, dashed in in_adj[nid] if dashed)
        dashed_to_chain = any(dst in chain_set for dst, _, dashed in out_adj[nid] if dashed)

        if feeds_chain and not fed_by_chain:
            satellites_in.add(nid)
        elif fed_by_chain or dashed_from_chain:
            satellites_out.add(nid)
        elif dashed_to_chain:
            ctrl_nodes.add(nid)
        else:
            satellites_out.add(nid)

    # Determine which pattern to use
    has_ctrl = len(ctrl_nodes) > 0
    has_obs = len(obs_nodes) > 0
    has_fan_in = len(satellites_in) > 0
    has_fan_out = len(satellites_out) > 0

    if has_ctrl and not has_fan_in and not has_fan_out:
        ctrl_id = next(iter(ctrl_nodes))
        # Find label on the edge from ctrl to chain
        ctrl_label = ''
        for src, dst, label, dashed in edges:
            if src == ctrl_id and dst in chain_set:
                ctrl_label = label
                break
            if dst == ctrl_id and src in chain_set:
                ctrl_label = label
                break
        return generate_control_above(chain, ctrl_id, nodes, styles, ctrl_label)

    if has_fan_in and not has_fan_out and not has_ctrl:
        return generate_multi_input(chain, satellites_in, nodes, edges, styles)

    if has_fan_out and not has_fan_in and not has_ctrl:
        return generate_fan_out(chain, satellites_out, nodes, edges, styles)

    if not has_ctrl and not has_fan_in and not has_fan_out:
        return generate_simple_lr(chain, nodes, styles)

    # Mixed: try to handle control + main chain
    if has_ctrl:
        ctrl_id = next(iter(ctrl_nodes))
        # If also has fan_out, generate control above with fan_out
        # For now, just do control above with the main chain
        ctrl_label = ''
        for src, dst, label, dashed in edges:
            if src == ctrl_id:
                ctrl_label = label
                break
        return generate_control_above(chain, ctrl_id, nodes, styles, ctrl_label)

    # Fallback: just do the chain
    return generate_simple_lr(chain, nodes, styles)


# ── File rewriting ──────────────────────────────────────────────────────────

MERMAID_RE = re.compile(r'```mermaid\s*\n(.*?)\n```', re.DOTALL)


def convert_file(md_path: Path, dry_run=False) -> int:
    """Convert Mermaid blocks in a single .md file.  Returns count of conversions."""
    content = md_path.read_text()
    stem = md_path.stem  # e.g. 'map'

    matches = list(MERMAID_RE.finditer(content))
    if not matches:
        return 0

    count = 0
    # Process from last to first to preserve offsets
    for i, m in enumerate(reversed(matches)):
        idx = len(matches) - 1 - i  # original index
        mermaid_text = m.group(1)

        # Skip state diagrams
        if 'stateDiagram' in mermaid_text:
            continue

        dsl = mermaid_to_dsl(mermaid_text)
        if dsl is None:
            print(f'  SKIP: {md_path.name} block {idx} (unsupported)', file=sys.stderr)
            continue

        # Determine SVG filename
        if len(matches) == 1 or (len(matches) == 2 and 'stateDiagram' in matches[1 - idx].group(1) if len(matches) == 2 else False):
            svg_name = f'{stem}.svg'
        else:
            # Multiple flow diagrams — need suffixes
            svg_name = f'{stem}_{idx}.svg' if idx > 0 else f'{stem}.svg'

        alt_text = f'{stem} topology'

        replacement = f'<!-- csp-flow\n{dsl}\n-->\n![{alt_text}](diagrams/{svg_name})'

        content = content[:m.start()] + replacement + content[m.end():]
        count += 1

    if count > 0 and not dry_run:
        md_path.write_text(content)

    return count


# ── Special-case overrides ──────────────────────────────────────────────────
# Some files need hand-crafted DSL that the auto-converter can't produce.

OVERRIDES: Dict[str, List[Tuple[int, str, str]]] = {
    # (block_index, dsl_text, svg_name)

    'slide.md': [(0, (
        '                  -> reader<T> window.in\n'
        'reader<T> -> {slide}\n'
        '                  -> reader<T> window.out'
    ), 'slide.svg')],

    'fanout.md': [(0, (
        'reader<writer<T>> -> {fanout} -> reader<writer<T>>\n'
        '                             ..> writer<T> #1\n'
        '                             ..> writer<T> #2\n'
        '                             ..> writer<T> ...N'
    ), 'fanout.svg')],

    'group_by.md': [(0, (
        'reader<T> -> {group_by(f)} -> reader<pair<K, reader<T>>>\n'
        '                           ..> reader<T> key=A\n'
        '                           ..> reader<T> key=B\n'
        '                           ..> reader<T> key=...'
    ), 'group_by.svg')],

    'share.md': [
        (0, (
            'reader<T> -> {share} -> reader<reader<T>>\n'
            '                     ..> reader<T> #1\n'
            '                     ..> reader<T> #2\n'
            '                     ..> reader<T> #3'
        ), 'share.svg'),
        (1, (
            '                  -> {latch 1} -> subscriber 1\n'
            'source -> {share}\n'
            '                  -> {latch 2} -> subscriber 2'
        ), 'share_internal.svg'),
    ],

    'flat_map.md': [(0, (
        '                   -> {sub 1} ->\n'
        'reader<A> -> {flat_map(f)}     -> reader<B>\n'
        '                   -> {sub N} ->'
    ), 'flat_map.svg')],

    'parallel_map.md': [
        (0, (
            '              -> {worker 1} ->\n'
            'reader<A> ->                  -> reader<B>\n'
            '              -> {worker N} ->'
        ), 'parallel_map.svg'),
        (1, (
            '                         -> {worker 1} ->\n'
            'reader<A> -> {dispatch}                   -> {collect} -> reader<B>\n'
            '                         -> {worker N} ->'
        ), 'parallel_map_ordered.svg'),
    ],

    'metrics.md': [(0, (
        'reader<T> -> {metrics} -> reader<T>\n'
        '                  |\n'
        '    reader<metrics_snapshot>'
    ), 'metrics.svg')],

    'tee.md': [(0, (
        'reader<T> -> {tee} -> reader<T>\n'
        '               |\n'
        '         writer<T> side'
    ), 'tee.svg')],

    'try_map.md': [(0, (
        'reader<A> -> {try_map(f, err)} -> reader<B>\n'
        '                    |\n'
        '       writer<exception_ptr>'
    ), 'try_map.svg')],


    'killswitch.md': [(0, (
        '         reader<>\n'
        '              |\n'
        '        "death signal"\n'
        'reader<A> -> {killswitch} -> reader<A>'
    ), 'killswitch.svg')],

    'timeout.md': [(0, (
        '       after(d)\n'
        '           |\n'
        '    "reset on value"\n'
        'reader<T> -> {timeout(d)} -> reader<T>'
    ), 'timeout.svg')],

    'timer.md': [(0, (
        '  reader<duration>\n'
        '         |\n'
        '   {timer} -> reader<time_point>'
    ), 'timer.svg')],

    'mute.md': [(0, (
        '{mute} ..> reader (blocked)'
    ), 'mute.svg')],

    'deaf.md': [(0, (
        'writer (blocked) ..> {deaf}'
    ), 'deaf.svg')],

    'latch.md': [(0, (
        'writer<T> -> {latch} -> reader<T>'
    ), 'latch.svg')],

    'rpc.md': [
        (0, (
            '          -"tuple<Args...>"->\n'
            '{client}                      {server}\n'
            '          <-"Rep"-'
        ), 'rpc_pair.svg'),
        (1, (
            '          -"pair<tuple<Args...>, writer<Rep>>"->\n'
            '{client}                                        {server}\n'
            '          <-"Rep"-'
        ), 'rpc_reply.svg'),
    ],

    'byte_reader.md': [(0, (
        'fd -> {byte_reader(fd)} -> reader<vector<uint8_t>>'
    ), 'byte_reader.svg')],

    'byte_writer.md': [(0, (
        'writer<vector<uint8_t>> -> {byte_writer(fd)} -> fd'
    ), 'byte_writer.svg')],

    'random.md': [
        (0, '{uniform_int(lo, hi)} -> reader<T>', 'uniform_int.svg'),
        (1, '{uniform_real(lo, hi)} -> reader<T>', 'uniform_real.svg'),
        (2, '{bernoulli(p)} -> reader<bool>', 'bernoulli.svg'),
        (3, '{normal(mean, stddev)} -> reader<T>', 'normal.svg'),
        (4, '{choice({a, b, c})} -> reader<T>', 'choice.svg'),
        (5, 'reader<T> -> {shuffle(n)} -> reader<T>', 'shuffle.svg'),
    ],

    'first_last.md': [
        (0, 'reader<T> -> {first(n)} -> reader<T>', 'first.svg'),
        (1, 'reader<T> -> {last(n)} -> reader<T>', 'last.svg'),
        (2, 'reader<T> -> {skip_first(n)} -> reader<T>', 'skip_first.svg'),
        (3, 'reader<T> -> {skip_last(n)} -> reader<T>', 'skip_last.svg'),
    ],

    'sink.md': [(0, (
        'reader<A> -> {sink(f)}'
    ), 'sink.svg')],

    'sample.md': [(0, (
        '       reader<Trigger>\n'
        '             |\n'
        'reader<T> -> {sample} -> reader<T>'
    ), 'sample.svg')],

    'gate.md': [(0, (
        '       reader<bool>\n'
        '             |\n'
        'reader<T> -> {gate} -> reader<T>'
    ), 'gate.svg')],

    'pace.md': [(0, (
        '       reader<Trigger>\n'
        '             |\n'
        'reader<T> -> {pace} -> reader<T>'
    ), 'pace.svg')],

    'quantify.md': [(0, (
        'reader<T> -> {any_of(pred) / all_of(pred)} -> reader<bool>'
    ), 'quantify.svg')],

    'partition.md': [(0, (
        '                              -> reader<T> 0\n'
        'reader<T> -> {partition(f)} -> reader<T> 1\n'
        '                              -> reader<T> 2'
    ), 'partition.svg')],

    'round_robin.md': [(0, (
        '                                -> reader<T> 0\n'
        'reader<T> -> {round_robin(n)} -> reader<T> 1\n'
        '                                -> reader<T> 2'
    ), 'round_robin.svg')],

    'unzip.md': [(0, (
        '                                   -> reader<A>\n'
        'reader<tuple<A, B, ...>> -> {unzip} -> reader<B>\n'
        '                                   -> reader<...>'
    ), 'unzip.svg')],

    'demux.md': [(0, (
        '                                  -> reader<A>\n'
        'reader<variant<A,B,...>> -> {demux} -> reader<B>\n'
        '                                  -> reader<...>'
    ), 'demux.svg')],

    'quantize.md': [(0, (
        'reader<T> source ->                -> writer<T> sink\n'
        '                    {quantize}\n'
        'reader<T> quanta ->                -> writer<T> residue'
    ), 'quantize.svg')],
}


def convert_file_with_overrides(md_path: Path, dry_run=False) -> int:
    """Convert using overrides where available, auto-convert otherwise."""
    stem = md_path.stem
    content = md_path.read_text()

    matches = list(MERMAID_RE.finditer(content))
    if not matches:
        return 0

    overrides = OVERRIDES.get(f'{stem}.md', [])
    override_map = {}
    for entry in overrides:
        if len(entry) == 3:
            idx, dsl, svg = entry
        else:
            idx, dsl, svg = entry[0], entry[1], entry[2]
        override_map[idx] = (dsl, svg)

    count = 0
    # Process from last to first
    for i, m in enumerate(reversed(matches)):
        idx = len(matches) - 1 - i
        mermaid_text = m.group(1)

        if 'stateDiagram' in mermaid_text:
            continue

        if idx in override_map:
            dsl, svg_name = override_map[idx]
        else:
            dsl = mermaid_to_dsl(mermaid_text)
            if dsl is None:
                print(f'  SKIP: {md_path.name} block {idx}', file=sys.stderr)
                continue
            # Default SVG name
            flow_blocks = [j for j, mm in enumerate(matches)
                           if 'stateDiagram' not in mm.group(1)]
            if len(flow_blocks) <= 1:
                svg_name = f'{stem}.svg'
            else:
                svg_name = f'{stem}_{idx}.svg' if idx > 0 else f'{stem}.svg'

        alt_text = f'{stem} topology'
        replacement = f'<!-- csp-flow\n{dsl}\n-->\n![{alt_text}](diagrams/{svg_name})'

        content = content[:m.start()] + replacement + content[m.end():]
        count += 1

    if count > 0 and not dry_run:
        md_path.write_text(content)

    return count


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    dry_run = '--dry-run' in sys.argv

    # Skip throttle.md — already converted
    skip = {'throttle.md'}

    total = 0
    for md_path in sorted(PARTS_DIR.glob('*.md')):
        if md_path.name in skip:
            continue
        if md_path.name == 'parts.md':
            continue  # catalog page, no diagrams

        n = convert_file_with_overrides(md_path, dry_run=dry_run)
        if n > 0:
            action = 'would convert' if dry_run else 'converted'
            print(f'  {action} {md_path.name}: {n} block(s)')
            total += n

    print(f'\nTotal: {total} blocks {"(dry run)" if dry_run else "converted"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

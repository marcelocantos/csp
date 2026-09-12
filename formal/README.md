# Formal checks and correspondence

`make check` runs the fixed TLA+ models. The `_Bug` companions demonstrate
the failures the models were written to prevent; they are excluded from that
command. Model checking and source correspondence answer different questions.

`make check-tla-tags` compares `TLA:Module.Action` anchors in the fixed models
and C++ sources against [the frozen inventory](../scripts/tla_tag_baseline.json).
It runs in default `make`, `make bullseye`, and the CI TLA job. It never rewrites
its own baseline. Each entry records every occurrence's file (including
duplicates); line numbers may move without a baseline change.

The initial inventory reproduces the ENT-004 audit: 35 tag names have anchors
on both sides, 171 model tags have no C++ anchor, and 14 C++ labels have no model
anchor. The missing C++ anchors span the lifecycle, channel, scheduler, reactor,
signal, timer and stack-pool models. The orphaned labels belong to `ParkGate`,
`PerWorkerWake` and `StealWork`. These entries preserve known correspondence
debt; they do not assert that an unpaired action is intentionally model-only or
that any pair is semantically equivalent. Resolving each gap requires review
of the corresponding implementation and model.

Changes in either direction fail, including improvements and deleting a pair
from both sides. When changing a modeled protocol:

1. Review the implementation and model ordering together.
2. Update the affected anchors and explicitly edit their inventory entries.
3. Run `make check-tla-tags` and the relevant model and product tests.
4. Commit the baseline change with the code/model change, explaining any
   remaining unpaired anchors. Never regenerate the baseline simply to silence
   a failure.

The check detects anchor drift, not untagged implementation changes, incorrect
action bodies, or incomplete model coverage. Code review and model/product
tests remain necessary.

# September audit consolidation and Lifeboat

Local integration completed on 2026-09-12. This follows the two-round fleet
handoff, rather than reopening the repository-wide audit. No remote push,
pull request or release was performed.

## What changed

The entropy, performance, documentation, hygiene and notice branches are
integrated locally. Their useful changes now coexist with the fixes below:

- CMake's HTTP/1.1 protocol subset is explicit. Both the Makefile lint and
  direct CMake configure reject undeclared source additions. Native CMake
  builds and symbol inspection confirm the declared subset; an unlisted
  source mutation is rejected. This does not add Windows protocol support.
- Source and distribution test inventories are compared as complete,
  configuration-matched multisets, including TLS-disabled sanitizers.
  White-box exclusions remain explicit. Missing cases and failed, empty or
  malformed listings fail closed.
- Cancellation reasons are written before the cancelled flag is published.
  The focused libstdc++ TSan regression rejects the old ordering and accepts
  the fix. `is_cancel_active()` still means that a cancellation scope exists;
  explicitly supplied null reasons remain legal.
- A captured full-suite hang exposed stranded runnable work with a parked
  worker. Two wake gaps were repaired and independently reviewed. The
  [investigation and complete test matrix](../papers/36-worker-lost-wake.md)
  record the mechanisms, failing regressions and remaining uncertainty.
- Public versions, generated gateway freshness, vendored metadata and formal
  tag correspondence are enforced through the normal check paths. Hygiene
  validation holds every declared floor. Redundant documentation inventories
  were removed, and active documentation describes the current drop-ins.
- The allocation ratchet and compiler-flag dependency fix are integrated.
  Existing allocation baselines were preserved.

## A working visual consumer

[Lifeboat](../../demos/lifeboat/) is the first playable orbital dock: six live
CSP actors handle cargo through shared, buffered and rendezvous channels.
The browser exposes congestion, competing cranes, supervised recovery and
graceful evacuation. It is compiled from the distributed files.

The same executable passes live self-tests with one and four workers on
macOS and Ubuntu 24.04 ARM64 with GCC/libstdc++. A browser journey checks the
actual UI and backend on desktop and a 390-pixel viewport. It verifies
conservation, recovery, completed handoffs, empty evacuation and a new shift.
The journey is wired into CI; remote CI has not run for this local batch.

Independent review improved the observation boundary: handoffs are counted
only after sends succeed, late notifications cannot rewind cargo, full
control queues cannot block event drainage, and abandoned snapshot mailboxes
cannot stall the coordinator. Visual review caught canvas sizing feedback
that the first geometry test missed; the journey now checks both scene bounds
and canvas backing dimensions.

## What the evidence does not claim

The formal correspondence ratchet now has 45 paired tags, with the existing
171 missing C++ anchors and 14 orphaned C++ tags frozen. A matching tag is
not semantic equivalence. New cancellation and wake pairs have executable
fixed and broken models; the dock has a bounded routing abstraction, not a
proof of the full application.

Windows and Linux x86_64 were not rerun locally. Existing sanitizer exclusions
remain. The Windows VM gate still belongs to the explicit shipping workflow.
The dock is a six-actor demonstration, not yet a city, capacity benchmark or
deterministic replay system. Its visual appeal remains the owner's judgment.

## Useful lessons from this round

An audit finding needs an activated, fault-sensitive check, not just a file
named after a check. Running the full packaged consumer uncovered a scheduler
failure that narrow protocol runs had not exposed. A green rerun did not
explain the original hang; the retained stack/core did.

Atomic fast paths require the entire publish/register/wait handshake to be
rechecked. Failed compare-and-exchange attempts must remain separate model
steps when another actor can run between them. The previous atomic wake
abstraction hid exactly such a gap.

The next demo expansion worth discussing is a connected station with several
docks, power generation, fabrication recipes and habitat demand. Shared
resources would let a single failure create visible effects across districts.
Keep each new subsystem as an independent CSP actor and retain conservation
and shutdown journeys as the world grows. Replay should wait for a real
simulation checkpoint/clock design rather than a browser-only approximation.

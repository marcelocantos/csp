# Transition Rules Syntax

The CSP reference documentation uses a compact notation for specifying the
behaviour of each operation. This page explains how to read and interpret it.

## The basic form

Every rule follows this pattern:

```
subject.operation(args) ─┤guard├──➤ effects; result
```

| Element | Meaning |
|---------|---------|
| **subject** | The object or state the operation acts on (e.g., `live`, `active`, `sleep`). |
| **operation** | The function or operator being invoked (e.g., `operator<<(v)`, `spawn()`). |
| **guard** | A condition that must be true for this rule to apply. Enclosed in `─┤` and `├─`. |
| **effects** | What happens: state changes, data movement, scheduling actions. |
| **result** | The value returned to the caller, if any. |

The long horizontal line (`────`) is purely visual padding for alignment. It
carries no meaning.

## Rules without guards

When an operation has exactly one outcome regardless of state, the guard is
omitted:

```
live.copy()  ─────────────────➤ new writer sharing same channel; refcount++
```

Read this as: "calling `copy()` on a live endpoint always creates a new writer
sharing the same channel and increments the reference count."

## Guarded rules

When the outcome depends on the state of the system, the guard names the
condition:

```
live.operator<<(v)  ─┤reader ready├──➤ move(v, reader.dest); true
live.operator<<(v)  ─┤no readers├────➤ false
live.operator<<(v)  ─┤waiting├───────➤ suspend until reader ready v no readers
```

Multiple rules for the same operation are listed vertically. Each guard
identifies a distinct case. The set of guards covers all possible scenarios for
the operation.

Read the first rule as: "calling `<<` on a live writer, when a reader is already
waiting, moves the value to the reader's destination and returns true."

## Separators and connectives

| Symbol | Meaning |
|--------|---------|
| `;` | Sequential effects — the left side happens, then the right side. |
| `v` | Logical OR — either condition may trigger the transition. |
| `→` (thin arrow) | Produces or returns a value (used in continuations and results). |
| `──➤` (thick arrow) | Separates the trigger (left) from the outcome (right). |

Example with `;`:

```
live.~writer()  ─┤refcount = 1├──➤ write-end dies; unblock waiting readers → false
```

Read: "when the last writer is destroyed, the write-end dies, then any blocked
readers are unblocked and receive `false`."

Example with `v`:

```
live.operator<<(v)  ─┤waiting├──➤ suspend until reader ready v no readers
```

Read: "the writer suspends until a reader becomes ready, or until there are no
readers left."

## Multi-line continuations

When an operation's effects don't fit on one line, continuation lines are
indented and aligned with the effects column:

```
spawn(f) ────────────────────────➤ new imp M created; M becomes runnable;
                                   → reader<std::exception_ptr>
```

The `→` on the continuation line indicates the return value. Read: "`spawn`
creates a new imp that becomes runnable, and returns a
`reader<std::exception_ptr>`."

A longer example:

```
notify(sigs) ──────────────────➤ pipe created; handlers installed;
                                  producer MT spawned; sentinel MT spawned;
                                  → reader<int>
```

## Two-step rules

Some operations involve an initial action followed by a later transition. These
appear as a rule whose effects include a suspension, followed by an indented
continuation with its own guard:

```
sleep(d) ────────────────➤ suspend; deadline = clock::now() + d
         ─┤deadline passes├─➤ imp becomes runnable; return
```

Read: "`sleep(d)` suspends the imp with a deadline. When the deadline
passes, the imp becomes runnable and `sleep` returns."

## State transitions

Some rules describe an object moving between states rather than returning a
value. The `→` arrow denotes the state change:

```
active.disarm()  ─────────────────➤ active → inactive
```

Read: "calling `disarm()` on an active `chan_op` transitions it to the inactive
state."

## Subjects

The subject is often a **state name** from the object's lifecycle diagram. For
example, `writer<T>` has three states — null, live, dead — and its rules are
written as:

```
live.operator<<(v)  ─┤reader ready├──➤ ...
live.~writer()      ─┤refcount = 1├──➤ ...
null.~writer()      ─────────────────➤ (no-op)
```

When the subject is a function call with no receiver object (a free function),
the subject is the function itself:

```
schedule() ─┤imps exist├─➤ block calling thread; run scheduler loop
schedule() ─┤all MTs finished├───➤ return
```

## Reading a complete rule block

Here is a complete block from the `reader<T>` documentation, annotated:

```
live.operator>>(dest)  ─┤writer ready├──➤ move(writer.val, dest); true     ①
live.operator>>(dest)  ─┤no writers├────➤ false                            ②
live.operator>>(dest)  ─┤waiting├───────➤ suspend until writer ready       ③
                                           v no writers
live.read()            ─┤writer ready├──➤ move(writer.val, local);         ④
                                           return local
live.read()            ─┤no writers├────➤ throw "reader exhausted"         ⑤
live.operator~()       ─────────────────➤ chan_op that matches when         ⑥
                                           all writers die
live.copy()            ─────────────────➤ new reader; refcount++           ⑦
live.~reader()         ─┤refcount > 1├──➤ refcount--                       ⑧
live.~reader()         ─┤refcount = 1├──➤ read-end dies; unblock writers   ⑨
null.~reader()         ─────────────────➤ (no-op)                          ⑩
```

1. A writer is waiting: move its value into `dest`, return `true`.
2. No writers remain: return `false` (channel exhausted).
3. No writer ready yet: suspend until one arrives or the channel dies.
4. Like `>>`, but returns the value directly instead of writing to `dest`.
5. Like `>>`, but throws instead of returning `false`.
6. Creates a death-watch `chan_op` (no guard — always succeeds).
7. Creates a new reader sharing the same channel (no guard).
8. Destroying a reader when others remain just decrements the count.
9. Destroying the last reader kills the read-end and unblocks any writers.
10. Destroying a null reader is a no-op.

## Summary

| Pattern | Meaning |
|---------|---------|
| `subject.op(args) ──➤ effects` | Unconditional rule |
| `subject.op(args) ─┤guard├──➤ effects` | Conditional rule |
| `effects; more_effects` | Sequential effects |
| `condition_a v condition_b` | Either condition |
| `→ result` | Return value (often on a continuation line) |
| `state_a → state_b` | State transition |
| `(no-op)` | Operation has no effect |

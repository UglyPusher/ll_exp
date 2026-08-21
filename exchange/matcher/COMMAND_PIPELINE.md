# Command SPMC Pipeline

`CommandPipeline` is the bounded preallocated runtime transport for one Command
stream. It is separate from the physical WAL implementation and contains no
file I/O.

## Frontier Contract

The five absolute zero-based frontiers satisfy:

```text
tail <= reserve_checked <= risk_checked <= durable <= head
head - tail <= capacity
```

Exactly one role writes each frontier:

| Role | Reads | Writes |
|---|---|---|
| Ingress | `tail` | immutable `CommandEnvelope`, reset sidecars, `head` |
| Persistence | `head` | physical WAL outside the pipeline, then `durable` |
| Risk stage | `durable` | `RiskResult`, then `risk_checked` |
| Reserve stage | `risk_checked` | `ReserveResult`, then `reserve_checked` |
| Matcher | `reserve_checked` | copies complete slot, then `tail` |

Every frontier occupies its own 64-byte cache line. Publication uses a release
store; the next role acquires its upstream frontier before reading the slot.
No frontier uses sequential consistency.

## Storage And Reuse

`open()` allocates and value-initializes exactly `capacity` `CommandRingSlot`
objects and one aligned control block. Runtime stage operations do not allocate.
Position `p` maps to slot `p % capacity` and to command sequence
`first_sequence + p` without wraparound.

A slot cannot be reused until Matcher advances `tail`. A stalled downstream
stage therefore produces bounded backpressure instead of overwrite or loss.

## Persistence Boundary

Persistence copies immutable pending commands with
`copy_pending_for_persistence()`. After the selected physical WAL adapter has
successfully appended and physically synchronized a batch, the same role calls
`publish_durable(count)`. The pipeline does not infer or perform physical sync.

`fail_persistence()` is terminal for Ingress and Persistence. It prevents new
commands and further durable publication. Risk, Reserve, and Matcher may still
drain the prefix that was already below `durable`; commands in `[durable, head)`
remain inaccessible to business stages.

`RiskResult` and `ReserveResult` are runtime-only single-writer sidecars. A
`Pending` decision cannot publish a readiness frontier.

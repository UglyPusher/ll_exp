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
| Active producer | `tail` | immutable `CommandEnvelope`, reset sidecars, `head` |
| Persistence | `head` | live WAL I/O when enabled, then `durable` |
| Risk stage | `durable` | `RiskResult`, then `risk_checked` |
| Reserve stage | `risk_checked` | `ReserveResult`, then `reserve_checked` |
| Matcher | `reserve_checked` | copies complete slot, then `tail` |

Every frontier occupies its own 64-byte cache line. Publication uses a release
store; the next role acquires its upstream frontier before reading the slot.
No frontier uses sequential consistency.

## Storage And Reuse

`open()` allocates and value-initializes exactly `capacity` `CommandRingSlot`
objects and one aligned control block. Runtime stage operations do not allocate.
Position `p` maps to slot `p % capacity`. Ring position is a monotonic transport
coordinate and is independent of the `CommandSequence` stored in that slot.
Live publication uses a separate `next_live_sequence` cursor initialized from
`first_sequence`; replay publication preserves the physical sequence supplied
in its `CommandEnvelope`.

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

## Live And Replay Publication

Exactly one producer may own `head` at a time:

- live Ingress calls `try_publish(payload)`, which assigns and advances the
  live sequence cursor;
- `WalFileReader` calls `try_replay(envelope)` for a record already validated
  and decoded from Command WAL. The pipeline stores its physical sequence
  unchanged and does not advance the live cursor.

`try_replay()` rejects sequence zero. File identity, CRC, schema, and contiguous
WAL sequence validation remain the responsibility of the validated WAL reader.
All downstream stage results report the sequence stored in the slot; they never
derive command identity from ring position.

`restore_live_sequence()` restores the live cursor when an ordered
`LoadSnapshot` command is applied. It does not reset or move ring frontiers and
must not race with a live producer. Consequently a replay may move `head` and
`tail` arbitrarily far while the restored live command sequence remains
independent of those positions.

Replay control is in-band. `StartReplay`, `LoadSnapshot`, `StopReplay`, and the
final `LoadSnapshot` pass through every tract module in order; there is no
supervisor above the modules. In replay mode Persistence remains the sole
writer of `durable`, but advances it for commands supplied by `WalFileReader`
without append or sync. The exact control-message and snapshot orchestration
protocol is defined by the later deterministic-replay and snapshot items. All
commands share one `CommandEnvelope`; in live mode every command is persisted.
Schema version 2 adds canonical `StartReplay`/`StopReplay` records. In replay
mode Persistence advances `durable` without writing those replay-run commands
back into WAL.

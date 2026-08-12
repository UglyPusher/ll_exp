# Matcher TODO

This file tracks open design issues for the minimal `matcher` sample.

## CommandReader

- Graceful shutdown is now only an ordered `CommandType::Shutdown` command.
- `CommandReadStatus` is limited to `Ok`, `Empty`, and `Fatal`.
- `Empty` means polling: the matcher remains in `run()`, performs no state
  transition, and reads again. The current sample does no pause instruction,
  sleep, yield, idle callback, or batch drain.
- Define the concrete production idle strategy: pure spin, pause/backoff,
  metrics hook, or reader-owned wait policy.
- Decide whether `read_batch()` is useful. It may help amortize reader overhead
  or drain a ring/WAL page, but it is intentionally not part of the first
  contract.
- `CommandReadStatus::Fatal` returns `RunStatus::Fatal`. Fatal diagnostics are
  out-of-band runtime/executor responsibility, not matcher event-stream output.

## EventWriter

- `publish()` has only `Ok` and `Fatal`. Temporary capacity pressure is hidden
  inside the writer and may block until space is available.
- Define the exact meaning of `Ok` for each writer: accepted into an in-memory
  queue, written into mmap, made durable, or made visible to a downstream
  consumer.
- `Fatal` means the writer can no longer provide its publication contract. The
  failing event may be definitely not accepted or may have unknown publication
  status; recovery code must tolerate a valid prefix plus a possibly ambiguous
  tail.
- Confirm that a blocking `publish()` is acceptable in the synchronous matcher
  path.
- After `Fatal`, the matcher is terminal and state recovery is external via
  snapshot plus replay. Diagnostics after fatal still need a policy.

## Order identity and preflight

- `OrderId` is assigned upstream. Within one matcher epoch, new-order IDs must
  be strictly monotonically increasing: `order_id > last_order_id_`.
- `(EpochId, OrderId)` identifies an order globally. Epoch infrastructure is
  not part of the current sample.
- A stale, duplicate, or out-of-order new-order ID is a fatal stream invariant
  violation, not a business rejection.
- Stream/system invariant fatal reasons such as `NonMonotonicOrderId` publish a
  terminal `MatcherFatal` event while the event writer is still healthy.
- `EventWriterFatal` cannot publish a reliable fatal event through the failed
  writer; runtime/executor diagnostics remain out-of-band for that case.
- The matcher does not perform an active-book duplicate lookup for new orders;
  monotonicity is the hot-path duplicate/stale guard.
- In the current sample, if an order partially executes and then cannot rest its
  remaining quantity, the matcher treats that as fatal because trade events have
  already been published and there is no rollback.
- Business rejection does not roll back `last_order_id_`; the ID is consumed
  after the monotonicity check passes.

## Matching semantics

- Only limit orders are modeled. Market orders, IOC/FOK, post-only, reduce-only,
  pegged orders, and auction orders are not defined.
- Trade price is the resting maker price. This should be stated as a formal
  matching rule.
- Self-trade prevention is not implemented. Decide whether it belongs inside
  matcher policy or in an upstream validation/risk layer.
- Account state, risk checks, balances, positions, and credit limits are out of
  scope for this sample.
- Price range behavior for aggressive orders outside the book's configured
  range is not fully specified.

## Event stream

- Event ordering needs a formal contract. The sample emits accepted, then zero
  or more trades/maker done events, then taker done or rested/rejected.
- Partial maker updates currently have no event. Decide whether the event stream
  needs an explicit `OrderReduced`/`OrderUpdated` event.
- Rejection events are minimal. They do not yet expose the underlying
  `InsertStatus` from `OrderBook`.
- Command sequence numbers, timestamps, instrument IDs, and partition IDs are
  not modeled.

## Lifecycle and FSM

- The sample has no real market FSM beyond running/stopped/fatal.
- Auction, halt, resume, warmup, snapshot load, and controlled drain are open.
- `process()` is public for tests and simple harnesses. Decide whether the
  production API should expose it or keep it behind a test adapter.
- After fatal, all calls except diagnostics/destruction should be considered
  invalid. The sample returns fatal defensively.

## Allocation and performance

- The matcher itself is allocation-free after `OrderBook` construction. The
  sample test writer uses a fixed array and reports fatal on overflow.
- Compile-time reader/writer composition avoids virtual dispatch. If runtime
  polymorphism is later needed, measure the cost before changing the hot path.
- No cache-line layout, prefetching, batching, pause strategy, or NUMA-specific
  construction policy is defined yet.

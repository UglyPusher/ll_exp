# Matcher TODO

This file tracks open design issues for the minimal `matcher` sample.

## CommandReader

- Decide whether `CommandReadStatus::Shutdown` is enough, or whether shutdown
  should exist only as an ordered `CommandType::Shutdown` command.
- Define what `Empty` means for production readers. The matcher currently does
  no spin-wait policy, pause instruction, sleep, yield, idle callback, or batch
  drain.
- Decide whether `read_batch()` is useful. It may help amortize reader overhead
  or drain a ring/WAL page, but it is intentionally not part of the first
  contract.
- Specify whether `CommandReadStatus::Fatal` should publish a fatal event. The
  current sample attempts to publish `MatcherFatal`, but ignores the result.

## EventWriter

- `publish()` has only `Ok` and `Fatal`. Temporary capacity pressure is hidden
  inside the writer and may block until space is available.
- Define the exact meaning of `Ok` for each writer: accepted into an in-memory
  queue, written into mmap, made durable, or made visible to a downstream
  consumer.
- Confirm that a blocking `publish()` is acceptable in the synchronous matcher
  path.
- After `Fatal`, the matcher is terminal and state recovery is external via
  snapshot plus replay. Diagnostics after fatal still need a policy.

## Order identity and preflight

- `OrderBook` does not expose `contains(OrderId)`. The matcher cannot preflight
  duplicate active order IDs without attempting to insert a resting remainder.
- In the current sample, if an order partially executes and then cannot rest its
  remaining quantity, the matcher treats that as fatal because trade events have
  already been published and there is no rollback.
- Decide whether incoming taker order IDs must be globally unique even when the
  order fully executes and never rests.

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

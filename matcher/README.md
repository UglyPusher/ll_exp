# fexma::matcher

`fexma::matcher` is a minimal matching-component sketch built on top of
`fexma::order_book`.

It owns an `OrderBook`, consumes commands from a caller-provided reader, and
publishes events through a caller-provided writer. The matcher does not create
threads, configure CPU affinity, know about WAL files, or own transport.

The hot-path contract is intentionally small:

- one matcher instance has one mutable matching state;
- commands are processed strictly sequentially;
- `CommandReader::read_next()` may return `Ok`, `Empty`, `Shutdown`, or
  `Fatal`;
- `EventWriter::publish()` returns only `Ok` or `Fatal`;
- temporary writer capacity pressure is handled inside the writer and is not
  observable by the matcher;
- after `Fatal`, the matcher instance must not continue processing commands.

This is a base sample, not a complete exchange matching engine. Open design
items are tracked in [Matcher TODO.md](../Matcher%20TODO.md).

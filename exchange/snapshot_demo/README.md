# Simple Snapshot Demo

Application modules and static composition tests for Demo 006.

The current tract is:

```text
WalCore::head
  -> PersistenceSlider -> DurableF
  -> HashChainSlider -> HashF
  -> BitAccumulatorSlider -> BitF
  -> composition reclaimer -> WalCore::tail
```

`HashChainModule` maintains a deterministic 64-bit FNV-1a-style chain over the
previous digest, absolute position, physical sequence, payload size, and every
payload byte. It is an application consistency state, not a cryptographic
authenticator.

`BitAccumulatorModule` maintains the total number of set payload bits and an
order-sensitive rolling fold over record identity and payload. Both modules
require the next absolute position exactly, process synchronously without
allocation, and become fail-closed on a gap, duplicate, or reordered position.

Both sliders use `AvailableRangeAcquire` and `OnePositionPublish`. Only the
composition advances `tail`, and it follows `BitF`, the last mandatory stage.
Snapshot command interpretation and immutable capture generations are added in
the next implementation step.

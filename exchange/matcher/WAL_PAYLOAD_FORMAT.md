# Matcher WAL Payload Format

This document defines canonical matcher payload schema versions 1 and 2. The
generic WAL owns physical record headers, sequence, CRC, and alignment. These
payloads contain only logical Command or Event fields.

All integer fields are unsigned little-endian values. All reserved and unused
bytes are zero. Decoders reject unknown tags, invalid enum values, non-canonical
boolean values, and non-zero reserved or unused bytes before returning a domain
object. Native C++ layout, padding, pointers, and spans are never persisted.

Changing a field, offset, width, tag meaning, or canonical payload size requires
a new payload schema version.

## Current Schema Version 2

Version 2 retains the version-1 widths and byte layouts for every existing
command and event. It adds persisted `StartReplay` and `StopReplay` command and
event tags. A version-1 decoder rejects those tags; it never assigns them a new
meaning retroactively.

### Command Payload Version 2

Canonical size: 48 bytes. WAL `payload_schema_version` is `2` and
`payload_size` is `48`. The common header and all version-1 bodies below are
unchanged.

Additional command body layouts:

| Type | Body fields |
|---|---|
| `StartReplay` | `replay_id:u64 @16`, `live_snapshot_id:u64 @24`, `replay_snapshot_id:u64 @32`, `replay_through_command_sequence:u64 @40` |
| `StopReplay` | `replay_id:u64 @16`, zero `@24..47` |

### Event Payload Version 2

Canonical size: 64 bytes. WAL `payload_schema_version` is `2` and
`payload_size` is `64`. The common header and all version-1 bodies below are
unchanged.

Additional event body layouts:

| Type | Body fields |
|---|---|
| `StartReplay` | `replay_id:u64 @24`, `live_snapshot_id:u64 @32`, `replay_snapshot_id:u64 @40`, `replay_through_command_sequence:u64 @48`, zero `@56..63` |
| `StopReplay` | `replay_id:u64 @24`, zero `@32..63` |

## Legacy Command Payload Version 1

Canonical size: 48 bytes. WAL `payload_schema_version` is `1` and
`payload_size` is `48`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `client_id` |
| 8 | 1 | `CommandType` |
| 9 | 7 | reserved zero |
| 16 | 32 | command body and zero tail |

Command body layouts:

| Type | Body fields |
|---|---|
| `NewLimit` | `order_id:u64 @16`, `owner_id:u64 @24`, `side:u8 @32`, zero `@33..35`, `price:u32 @36`, `quantity:u32 @40`, zero `@44..47` |
| `SaveSnapshot` | `snapshot_id:u64 @16`, `snapshot_epoch_id:u64 @24`, zero `@32..47` |
| `LoadSnapshot` | `snapshot_id:u64 @16`, `snapshot_epoch_id:u64 @24`, zero `@32..47` |
| `Shutdown` | zero `@16..47` |

`None` is not a persisted command type. `CommandSequence` is the physical
Command WAL record sequence and is not repeated in the payload. Instrument and
epoch identity are physical WAL/manifest metadata.

## Legacy Event Payload Version 1

Canonical size: 64 bytes. WAL `payload_schema_version` is `1` and
`payload_size` is `64`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `client_id` |
| 8 | 8 | `caused_by_command_sequence` |
| 16 | 4 | `index_in_command` |
| 20 | 1 | `is_last_for_command`, exactly `0` or `1` |
| 21 | 1 | `EventType` |
| 22 | 2 | reserved zero |
| 24 | 40 | event body and zero tail |

Event body layouts:

| Type | Body fields |
|---|---|
| `OrderAccepted` | `order_id:u64 @24`, zero `@32..63` |
| `OrderRejected` | `order_id:u64 @24`, `reason:u8 @32`, zero `@33..63` |
| `Trade` | taker/maker order IDs `@24/@32`, taker/maker owner IDs `@40/@48`, `price:u32 @56`, `quantity:u32 @60` |
| `OrderRested` | `order_id:u64 @24`, `owner_id:u64 @32`, `side:u8 @40`, zero `@41..43`, `price:u32 @44`, `remaining:u32 @48`, zero `@52..63` |
| `OrderDone` | `order_id:u64 @24`, zero `@32..63` |
| `SaveSnapshot` | `snapshot_id:u64 @24`, `snapshot_epoch_id:u64 @32`, zero `@40..63` |
| `LoadSnapshot` | `snapshot_id:u64 @24`, `snapshot_epoch_id:u64 @32`, zero `@40..63` |
| `Shutdown` | zero `@24..63` |
| `MatcherFatal` | `reason:u8 @24`, zero `@25..31`, `offending_order_id:u64 @32`, `last_order_id:u64 @40`, zero `@48..63` |

`None` is not a persisted event type. `EventSequence` is the physical Event WAL
record sequence and is not repeated in the payload. Instrument and epoch
identity are physical WAL/manifest metadata.

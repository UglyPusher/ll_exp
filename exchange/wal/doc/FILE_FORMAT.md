# WAL File Format

## Layout

```text
canonical FileHeader
zero padding to records_offset
RecordHeader + payload + zero padding
RecordHeader + payload + zero padding
...
```

One logical payload corresponds to one physical record. A durability batch is
only an I/O grouping and does not add a batch header.

## File Header

The canonical format-version-3 file header is 64 bytes. All integer fields are
little-endian:

```text
u32 magic
u16 version
u16 header_size       == 64
u16 stream_kind       // Generic, Command, or Event
u16 flags             == 0
u32 payload_size
u32 payload_schema_version
u32 alignment
u32 records_offset
u64 stream_id
u64 epoch_id
u64 first_sequence
u64 manifest_id
u32 header_crc32
```

One physical WAL contains exactly one stream in exactly one epoch.
`stream_kind`, `stream_id`, and `epoch_id` identify that stream.
`manifest_id` binds a Command or Event WAL to the epoch manifest that owns its
configuration, schemas, and starting snapshot. Generic infrastructure WALs may
use zero identities; Command and Event WAL configuration requires non-zero
`stream_id`, `epoch_id`, and `manifest_id`.

`payload_schema_version` identifies the application-level schema used to encode
every payload in this file. It is file metadata and is not repeated in physical
record headers or payloads. Value `0` means that the generic WAL caller has not
declared an application schema.

`first_sequence` is the physical sequence of the first record and is non-zero.
The immutable file header does not store a changing next-sequence value.

`flags` is reserved and must be zero in format version 3. `capacity` is not
persisted because it belongs to the runtime ring, not to the durable stream.

`header_crc32` is CRC32 of the canonical 64 bytes with `header_crc32` encoded
as zero. `records_offset` is the first byte of the record area and is always a
multiple of `alignment`. Zero padding between the file header and
`records_offset` is part of the physical file but not part of the file-header
CRC.

## Record

The canonical record header is 24 bytes. All integer fields are little-endian:

```text
u32 magic
u16 version
u16 header_size       == 24
u64 sequence
u32 payload_crc32
u32 header_crc32
```

The payload follows immediately. Zero bytes pad the combined header and payload
to configured alignment. Padding is not part of payload CRC.

Physical sequences start at `first_sequence` and are contiguous in append
order.

The record stride is:

```text
align_up(24 + payload_size, alignment)
```

Every record starts at:

```text
records_offset + (sequence - first_sequence) * record_stride
```

Both `records_offset` and `record_stride` are multiples of `alignment`.

## Compatibility And Recovery

The physical format does not use native C++ object representation, structure
padding, or native endian layout. C++ structs may exist as logical field
carriers only; disk bytes are canonical serialized bytes.

Format version 2 is an experimental predecessor and is not compatible with
format version 3. The live writer currently creates only a new exclusive file.
The read-only validated reader rejects incompatible formats, identity mismatch,
corruption, sequence gaps, incomplete records, and non-zero padding. The scanner
reports the longest trusted prefix.

Explicit recovery may truncate only a scanner-classified incomplete trailing
record to the end offset of that trusted prefix. It physically synchronizes the
truncation and validates the retained file again. An incomplete or invalid file
header, a complete invalid record (including the final record), middle
corruption, a sequence error, or an identity mismatch is never truncated or
repaired. After crash, every complete record in the maximal contiguous
CRC-valid prefix is authoritative history and participates in replay. The
format contains no batch commit record or commit marker; batching is a
live-writer append-and-sync policy only. Writer reopen and automatic format
migration are not implemented.

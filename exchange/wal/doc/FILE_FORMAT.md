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

The canonical file header is 36 bytes. All integer fields are little-endian:

```text
u32 magic
u16 version
u16 header_size       == 36
u32 payload_size
u32 alignment
u64 next_sequence     == 1
u32 header_crc32
u32 records_offset
u32 payload_schema_version
```

`payload_schema_version` identifies the application-level schema used to encode
every payload in this file. It is file metadata and is not repeated in physical
record headers or payloads. Value `0` means that the generic WAL caller has not
declared an application schema.

`header_crc32` is CRC32 of the canonical 36 bytes with `header_crc32` encoded
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

Physical sequences start at `1` and are contiguous in append order.

The record stride is:

```text
align_up(24 + payload_size, alignment)
```

Every record starts at:

```text
records_offset + (sequence - 1) * record_stride
```

Both `records_offset` and `record_stride` are multiples of `alignment`.

## Compatibility And Recovery

The physical format does not use native C++ object representation, structure
padding, or native endian layout. C++ structs may exist as logical field
carriers only; disk bytes are canonical serialized bytes.

The implementation only creates a new exclusive file. It does not open,
validate, recover, truncate, or migrate an existing WAL.

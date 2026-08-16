# WAL File Format

## Layout

```text
FileHeader
RecordHeader + payload + zero padding
RecordHeader + payload + zero padding
...
```

One logical payload corresponds to one physical record. A durability batch is
only an I/O grouping and does not add a batch header.

## File Header

`FileHeader` stores:

- format magic and version;
- compiled file-header size;
- fixed payload size;
- physical record alignment;
- initial `next_sequence`, currently always `1` and not updated later;
- CRC32 of the complete header with `header_crc32` zeroed;
- reserved bytes with no current meaning.

## Record

`RecordHeader` stores:

- record magic and format version;
- compiled record-header size;
- physical sequence;
- CRC32 of exactly one fixed-size payload;
- CRC32 of the complete record header with `header_crc32` zeroed.

The payload follows immediately. Zero bytes pad the combined header and payload
to configured alignment. Padding is not part of payload CRC.

Physical sequences start at `1` and are contiguous in append order.

## Compatibility And Recovery

The current format uses native C++ standard-layout structs and therefore does
not yet define cross-compiler or cross-endian compatibility.

The implementation only creates a new truncated file. It does not open,
validate, recover, or truncate an existing WAL.

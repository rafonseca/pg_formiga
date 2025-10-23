# pg_formiga

A PostgreSQL extension for monitoring heap pruning activity through real-time WAL analysis.

## Overview

**pg_formiga** (Portuguese for "ant") continuously monitors PostgreSQL heap pruning events to provide insights into database maintenance patterns. It implements a background worker that process WAL records collecting page pruning data. Hopefully it will allow for alternative vacuum strategies.

## What it does

The extension tracks when PostgreSQL prunes dead tuples from heap pages by:

- Monitoring Write-Ahead Log (WAL) records in real-time
- Detecting `HEAP2_PRUNE` operations as they occur
- Storing pruning events with timing and location details
- Classifying events by type (access-triggered, vacuum scan, cleanup)

## Next Phase

- Run a continuous vacuum using information collected to guide the process

## Installation
It follows standard C extension pattern. Note that the extension is in `./contrib/pg_formiga`


## Usage

Once installed, the extension automatically begins monitoring. View captured events:

```sql
-- All events with details
SELECT relation_oid, block_number, prune_reason, event_timestamp 
FROM formiga.prune_events 
ORDER BY event_timestamp DESC;
```

## Data collected

The extension stores:
- **relation_oid**: Which table was pruned
- **block_number**: Specific page that was pruned
- **conflict_xid**: Transaction ID that triggered pruning
- **wal_lsn**: WAL position of the pruning event
- **prune_reason**: Type of pruning (ON_ACCESS, VACUUM_SCAN, VACUUM_CLEANUP)
- **event_timestamp**: When the event was recorded


## Current status

This is experimental software suitable for development and testing environments. Use in production should be carefully evaluated.

## Contributing

Contributions are welcome. Please test thoroughly and follow PostgreSQL extension development practices.

## License

This extension follows the same license as PostgreSQL.

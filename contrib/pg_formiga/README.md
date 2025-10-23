# pg_formiga

A PostgreSQL extension for monitoring heap pruning activity through real-time WAL analysis.

## Overview

**pg_formiga** (Portuguese for "ant") continuously monitors PostgreSQL heap pruning events to provide insights into database maintenance patterns. Like industrious ants, it works quietly in the background, collecting data for experimental approaches to continuous vacuum optimization.

## What it does

The extension tracks when PostgreSQL prunes dead tuples from heap pages by:

- Monitoring Write-Ahead Log (WAL) records in real-time
- Detecting `HEAP2_PRUNE` operations as they occur
- Storing pruning events with timing and location details
- Classifying events by type (access-triggered, vacuum scan, cleanup)

## Goal

The extension provides real-time data about heap maintenance activity to support research into more intelligent vacuum strategies. By understanding when and where pruning occurs, it aims to enable:

- Data-driven vacuum scheduling based on actual page modification patterns
- Reduced vacuum overhead through targeted maintenance
- Better understanding of workload-specific cleanup requirements
- Foundation for experimental continuous vacuum approaches


## Installation

1. Build and install the extension:
```bash
make && make install
```

2. Add to `postgresql.conf`:
```
shared_preload_libraries = 'pg_formiga'
```

3. Restart PostgreSQL and create the extension:
```sql
CREATE EXTENSION pg_formiga;
```

## Usage

Once installed, the extension automatically begins monitoring. View captured events:

```sql
-- Recent pruning activity
SELECT relation_oid::regclass AS table_name, 
       block_number, 
       prune_reason, 
       event_timestamp 
FROM formiga.prune_events 
WHERE event_timestamp >= NOW() - INTERVAL '1 hour'
ORDER BY event_timestamp DESC;

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

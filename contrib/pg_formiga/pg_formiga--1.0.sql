-- pg_formiga extension version 1.0
-- Continuous vacuum monitoring via WAL-based heap pruning detection

-- Create dedicated schema for extension objects
CREATE SCHEMA formiga;

-- Enum for PRUNE operation reasons
CREATE TYPE formiga.prune_reason AS ENUM (
    'UNKNOWN',
    'ON_ACCESS', 
    'VACUUM_SCAN',
    'VACUUM_CLEANUP'
);

-- Main tracking table for heap pruning events
-- ONE ROW PER (relation_oid, block_number) - stores latest event
CREATE TABLE formiga.prune_events (
    relation_oid OID NOT NULL,
    block_number BIGINT NOT NULL,
    conflict_xid BIGINT,
    event_timestamp TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    wal_lsn PG_LSN NOT NULL,
    prune_reason formiga.prune_reason,
    
    -- Constraints
    PRIMARY KEY (relation_oid, block_number)
);

-- Indexes optimized for timestamp ordering and common queries
CREATE INDEX idx_prune_events_timestamp_desc 
    ON formiga.prune_events (event_timestamp DESC);

CREATE INDEX idx_prune_events_relation_timestamp 
    ON formiga.prune_events (relation_oid, event_timestamp DESC);

CREATE INDEX idx_prune_events_wal_lsn 
    ON formiga.prune_events (wal_lsn);

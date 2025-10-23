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
CREATE TABLE formiga.prune_events (
    id BIGSERIAL PRIMARY KEY,
    relation_oid OID NOT NULL,
    block_number BIGINT NOT NULL,
    conflict_xid BIGINT,
    event_timestamp TIMESTAMPTZ DEFAULT NOW(),
    wal_lsn PG_LSN NOT NULL,
    prune_reason formiga.prune_reason,
    CONSTRAINT valid_block_number CHECK (block_number >= 0)
);

-- Indexes for efficient querying
CREATE INDEX idx_prune_events_relation_block 
    ON formiga.prune_events (relation_oid, block_number);
CREATE INDEX idx_prune_events_timestamp 
    ON formiga.prune_events (event_timestamp);
CREATE INDEX idx_prune_events_wal_lsn 
    ON formiga.prune_events (wal_lsn);
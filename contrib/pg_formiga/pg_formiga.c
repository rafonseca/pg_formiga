/*
 * pg_formiga.c
 *   PostgreSQL continuous vacuum monitoring extension
 *   Background worker for WAL-based heap pruning detection
 *   
 * "Formiga" means "ant" in Portuguese - like industrious ants,
 * this extension continuously monitors and will eventually enable
 * intelligent vacuum scheduling based on real-time heap activity.
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/xlogrecord.h"
#include "access/heapam_xlog.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "access/xlog.h"
#include "storage/relfilelocator.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "utils/snapmgr.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "access/heapam.h"
#include "storage/bufmgr.h"
#include "access/table.h"
#include "storage/read_stream.h"
#include "commands/vacuum.h"

PG_MODULE_MAGIC;

/* GUC variables for vacuum worker control */
static bool formiga_vacuum_worker_enabled = true;
static int formiga_vacuum_worker_delay = 30;       /* 30 seconds */
static int formiga_vacuum_relations_per_cycle = 10; /* number of relations to process per cycle */

/* Function declarations */
void _PG_init(void);
void _PG_fini(void);
PGDLLEXPORT void formiga_bgworker_main(Datum main_arg);
PGDLLEXPORT void formiga_vacuum_worker_main(Datum main_arg);

/* Hook variables */
static heap_vac_scan_callback_hook_type prev_heap_vac_scan_callback_hook = NULL;

/* Signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

static void formiga_sighup_handler(SIGNAL_ARGS);
static void formiga_sigterm_handler(SIGNAL_ARGS);
static void formiga_vacuum_relation(Oid relation_oid);
static BlockNumber formiga_vac_scan_callback(ReadStream *stream, void *callback_private_data, void *per_buffer_data);

/* Helper function to convert PRUNE info to string */
static const char *
prune_reason_to_string(uint8 info)
{
    switch (info)
    {
        case XLOG_HEAP2_PRUNE_ON_ACCESS:
            return "ON_ACCESS";
        case XLOG_HEAP2_PRUNE_VACUUM_SCAN:
            return "VACUUM_SCAN";
        case XLOG_HEAP2_PRUNE_VACUUM_CLEANUP:
            return "VACUUM_CLEANUP";
        default:
            return "UNKNOWN";
    }
}



/*
 * Signal handlers for background worker
 */
static void
formiga_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    (void) postgres_signal_arg; /* unused parameter */
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
formiga_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    (void) postgres_signal_arg; /* unused parameter */
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/*
 * Initialize XLogReader - copied from pg_walinspect InitXLogReaderState()
 * 
 * NOTE: This function is copied exactly from contrib/pg_walinspect/pg_walinspect.c
 * to avoid code duplication while maintaining the same WAL reading functionality.
 */
static XLogReaderState *
InitXLogReaderState(XLogRecPtr lsn)
{
	XLogReaderState *xlogreader;
	ReadLocalXLogPageNoWaitPrivate *private_data;
	XLogRecPtr	first_valid_record;

	/*
	 * Reading WAL below the first page of the first segments isn't allowed.
	 * This is a bootstrap WAL page and the page_read callback fails to read
	 * it.
	 */
	if (lsn < XLOG_BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not read WAL at LSN %X/%X",
						LSN_FORMAT_ARGS(lsn))));

	private_data = (ReadLocalXLogPageNoWaitPrivate *)
		palloc0(sizeof(ReadLocalXLogPageNoWaitPrivate));

	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &read_local_xlog_page_no_wait,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									private_data);

	if (xlogreader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/* first find a valid recptr to start from */
	first_valid_record = XLogFindNextRecord(xlogreader, lsn);

	if (XLogRecPtrIsInvalid(first_valid_record))
		ereport(ERROR,
				(errmsg("could not find a valid record after %X/%X",
						LSN_FORMAT_ARGS(lsn))));

	return xlogreader;
}

/*
 * Read next WAL record - copied from pg_walinspect ReadNextXLogRecord()
 *
 * NOTE: This function is copied exactly from contrib/pg_walinspect/pg_walinspect.c
 * to avoid code duplication while maintaining the same WAL reading functionality.
 *
 * By design, to be less intrusive in a running system, no slot is allocated
 * to reserve the WAL we're about to read. Therefore this function can
 * encounter read errors for historical WAL.
 *
 * We guard against ordinary errors trying to read WAL that hasn't been
 * written yet by limiting end_lsn to the flushed WAL, but that can also
 * encounter errors if the flush pointer falls in the middle of a record. In
 * that case we'll return NULL.
 */
static XLogRecord *
ReadNextXLogRecord(XLogReaderState *xlogreader)
{
	XLogRecord *record;
	char	   *errormsg;

	record = XLogReadRecord(xlogreader, &errormsg);

	if (record == NULL)
	{
		ReadLocalXLogPageNoWaitPrivate *private_data;

		/* return NULL, if end of WAL is reached */
		private_data = (ReadLocalXLogPageNoWaitPrivate *)
			xlogreader->private_data;

		if (private_data->end_of_wal)
			return NULL;

		if (errormsg)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read WAL at %X/%X: %s",
							LSN_FORMAT_ARGS(xlogreader->EndRecPtr), errormsg)));
	}

	return record;
}

/*
 * Parse HEAP2_PRUNE record and extract page information
 */
static void
parse_heap2_prune_record(XLogReaderState *xlogreader, XLogRecord *record, uint8 info)
{
    xl_heap_prune xlrec;
    RelFileLocator rlocator;
    BlockNumber blkno;
    TransactionId conflict_xid = InvalidTransactionId;
    char *maindataptr;
    int ret;
    StringInfoData query;
    
    (void) record; /* unused parameter - we use xlogreader instead */
    
    /* Extract basic record info */
    maindataptr = XLogRecGetData(xlogreader);
    memcpy(&xlrec, maindataptr, SizeOfHeapPrune);
    maindataptr += SizeOfHeapPrune;
    
    /* Get relation and block information from block reference 0 */
    if (!XLogRecHasBlockRef(xlogreader, 0))
    {
        elog(LOG, "pg_formiga: HEAP2_PRUNE record has no block reference");
        return;
    }
    
    XLogRecGetBlockTag(xlogreader, 0, &rlocator, NULL, &blkno);
    
    /* Extract conflict horizon XID if present */
    if (xlrec.flags & XLHP_HAS_CONFLICT_HORIZON)
    {
        memcpy(&conflict_xid, maindataptr, sizeof(TransactionId));
        maindataptr += sizeof(TransactionId);
    }
    
    elog(LOG, "pg_formiga: Parsed HEAP2_PRUNE - Relation: %u, Block: %u, Flags: 0x%02x, Conflict XID: %u",
         rlocator.relNumber, blkno, xlrec.flags, conflict_xid);
    
    /* Store in database using proper SPI pattern from worker_spi.c */
    PG_TRY();
    {
        /* Set statement start time as recommended */
        SetCurrentStatementStartTimestamp();
        
        /* Start transaction for database operations */
        StartTransactionCommand();
        
        /* Connect to SPI */
        if (SPI_connect() != SPI_OK_CONNECT)
        {
            elog(WARNING, "pg_formiga: SPI_connect failed");
            AbortCurrentTransaction();
            return;
        }
        
        /* Push active snapshot for MVCC */
        PushActiveSnapshot(GetTransactionSnapshot());
        
        /* UPSERT record into formiga.prune_events table */
        initStringInfo(&query);
        if (conflict_xid != InvalidTransactionId)
        {
            appendStringInfo(&query,
                "INSERT INTO formiga.prune_events "
                "(relation_oid, block_number, conflict_xid, event_timestamp, wal_lsn, prune_reason) "
                "VALUES (%u, %u, %u, NOW(), '%X/%X', '%s') "
                "ON CONFLICT (relation_oid, block_number) DO UPDATE SET "
                "conflict_xid = EXCLUDED.conflict_xid, "
                "event_timestamp = EXCLUDED.event_timestamp, "
                "wal_lsn = EXCLUDED.wal_lsn, "
                "prune_reason = EXCLUDED.prune_reason",
                rlocator.relNumber,
                blkno,
                conflict_xid,
                LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
                prune_reason_to_string(info));
        }
        else
        {
            appendStringInfo(&query,
                "INSERT INTO formiga.prune_events "
                "(relation_oid, block_number, conflict_xid, event_timestamp, wal_lsn, prune_reason) "
                "VALUES (%u, %u, NULL, NOW(), '%X/%X', '%s') "
                "ON CONFLICT (relation_oid, block_number) DO UPDATE SET "
                "conflict_xid = EXCLUDED.conflict_xid, "
                "event_timestamp = EXCLUDED.event_timestamp, "
                "wal_lsn = EXCLUDED.wal_lsn, "
                "prune_reason = EXCLUDED.prune_reason",
                rlocator.relNumber,
                blkno,
                LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
                prune_reason_to_string(info));
        }
            
        elog(DEBUG1, "pg_formiga: Executing SQL: %s", query.data);
        
        ret = SPI_execute(query.data, false, 0);
        
        if (ret != SPI_OK_INSERT)
        {
            elog(WARNING, "pg_formiga: Failed to insert record: %d", ret);
        }
        else
        {
            elog(LOG, "pg_formiga: Successfully stored HEAP2_PRUNE data for relation %u block %u, conflict_xid: %u", 
                 rlocator.relNumber, blkno, conflict_xid);
        }
        
        /* Cleanup SPI and transaction following the official pattern */
        pfree(query.data);
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        /* Cleanup on error following the official pattern */
        ErrorData *edata;
        
        edata = CopyErrorData();
        FlushErrorState();
        
        elog(WARNING, "pg_formiga: Database transaction error: %s", edata->message);
        FreeErrorData(edata);
        
        SPI_finish();
        PopActiveSnapshot();
        AbortCurrentTransaction();
    }
    PG_END_TRY();
}

/*
 * Background worker main function
 * Monitors WAL for HEAP2_PRUNE records in real-time
 */
PGDLLEXPORT void
formiga_bgworker_main(Datum main_arg)
{
    XLogReaderState *xlogreader = NULL;
    XLogRecord *record;
    XLogRecPtr start_lsn;
    int record_count = 0;
    int heap2_prune_count = 0;
    
    (void) main_arg; /* unused parameter */
    
    /* Setup signal handlers */
    pqsignal(SIGHUP, formiga_sighup_handler);
    pqsignal(SIGTERM, formiga_sigterm_handler);
    BackgroundWorkerUnblockSignals();
    
    elog(LOG, "pg_formiga: Background worker starting [PID=%d]", MyProcPid);
    
    /* Initialize database connection */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);
    
    /* Get starting WAL position for real-time processing */
    start_lsn = GetFlushRecPtr(NULL);
    elog(LOG, "pg_formiga: Background worker monitoring from LSN %X/%X", LSN_FORMAT_ARGS(start_lsn));
    
    /* Main monitoring loop - wait for WAL activity */
    while (!got_sigterm)
    {
        XLogRecPtr current_lsn;
        
        /* Check for configuration reload */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
            elog(LOG, "pg_formiga: Configuration reloaded");
        }
        
        /* Check current WAL position */
        current_lsn = GetFlushRecPtr(NULL);
        
        if (current_lsn > start_lsn)
        {
            /* New WAL activity detected, try to initialize reader */
            PG_TRY();
            {
                xlogreader = InitXLogReaderState(start_lsn);
                elog(LOG, "pg_formiga: Processing new WAL from %X/%X to %X/%X", 
                     LSN_FORMAT_ARGS(start_lsn), LSN_FORMAT_ARGS(current_lsn));
                
                /* Process available records */
                while (!got_sigterm)
                {
                    record = ReadNextXLogRecord(xlogreader);
                    
                    if (record == NULL)
                        break; /* No more records */
                    
                    record_count++;
                    
                    /* Check for HEAP2 PRUNE records */
                    if (record->xl_rmid == RM_HEAP2_ID)
                    {
                        uint8 info = record->xl_info & ~XLR_INFO_MASK;
                        
                        if (info == XLOG_HEAP2_PRUNE_ON_ACCESS ||
                            info == XLOG_HEAP2_PRUNE_VACUUM_SCAN ||
                            info == XLOG_HEAP2_PRUNE_VACUUM_CLEANUP)
                        {
                            heap2_prune_count++;
                            
                            elog(LOG, "pg_formiga: HEAP2_PRUNE detected! Type: %s, LSN: %X/%X, Count: %d", 
                                 prune_reason_to_string(info), LSN_FORMAT_ARGS(xlogreader->ReadRecPtr), heap2_prune_count);
                            
                            /* Parse and store the PRUNE record */
                            parse_heap2_prune_record(xlogreader, record, info);
                        }
                    }
                }
                
                /* Update start position for next iteration */
                start_lsn = xlogreader->EndRecPtr;
                
                /* Cleanup */
                if (xlogreader)
                {
                    if (xlogreader->private_data)
                        pfree(xlogreader->private_data);
                    XLogReaderFree(xlogreader);
                    xlogreader = NULL;
                }
            }
            PG_CATCH();
            {
                /* Log error but continue */
                ErrorData *edata;
                
                /* Cleanup on error */
                if (xlogreader)
                {
                    if (xlogreader->private_data)
                        pfree(xlogreader->private_data);
                    XLogReaderFree(xlogreader);
                    xlogreader = NULL;
                }
                
                edata = CopyErrorData();
                FlushErrorState();
                elog(LOG, "pg_formiga: WAL processing error: %s", edata->message);
                FreeErrorData(edata);
                
                /* Advance start position slightly to avoid getting stuck */
                start_lsn += XLOG_BLCKSZ;
            }
            PG_END_TRY();
        }
        else
        {
            /* No new WAL activity, log periodically */
            if (record_count % 10000 == 0)
            {
                elog(LOG, "pg_formiga: Monitoring WAL activity - processed %d records, %d HEAP2_PRUNE found", 
                     record_count, heap2_prune_count);
            }
        }
        
        /* Wait before next check */
        WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 2000L, 0);
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
    }
    
    elog(LOG, "pg_formiga: Background worker exiting. Processed %d records, found %d HEAP2_PRUNE records", 
         record_count, heap2_prune_count);
}

/*
 * Enable formiga vacuum hook for intelligent block selection
 */
static void
formiga_enable_vacuum_hook(void)
{
    prev_heap_vac_scan_callback_hook = heap_vac_scan_callback_hook;
    heap_vac_scan_callback_hook = formiga_vac_scan_callback;
}

/*
 * Disable formiga vacuum hook and restore previous hook
 */
static void
formiga_disable_vacuum_hook(void)
{
    heap_vac_scan_callback_hook = prev_heap_vac_scan_callback_hook;
}

/*
 * Vacuum a single relation with formiga block selection
 * Note: Assumes we're already in a transaction context
 */
static void
formiga_vacuum_relation(Oid relation_oid)
{
    VacuumParams params;
    BufferAccessStrategy bstrategy;
    Relation rel;
    
    PG_TRY();
    {
        /* Set up vacuum parameters for formiga vacuum */
        memset(&params, 0, sizeof(VacuumParams));
        params.options = VACOPT_VACUUM | VACOPT_VERBOSE;  /* Enable vacuum and verbose */
        params.freeze_min_age = -1;      /* Use default freeze age */
        params.freeze_table_age = -1;    /* Use default freeze table age */
        params.multixact_freeze_min_age = -1; /* Use default multixact freeze age */
        params.multixact_freeze_table_age = -1; /* Use default multixact freeze table age */
        params.is_wraparound = false;    /* Not a wraparound vacuum */
        params.index_cleanup = VACOPTVALUE_ENABLED; /* Enable index cleanup */
        params.truncate = VACOPTVALUE_ENABLED;   /* Enable truncation */
        params.nworkers = 0;             /* No parallel workers for now */
        params.log_min_duration = 0;    /* Log all vacuum activity */
        params.max_eager_freeze_failure_rate = 0.0; /* Use default */
        
        /* Use maintenance work mem buffer strategy */
        bstrategy = GetAccessStrategy(BAS_VACUUM);
        
        /* Enable formiga vacuum hook for intelligent block selection */
        formiga_enable_vacuum_hook();
        
        /* Open relation and call heap_vacuum_rel - this will use our hook for formiga block selection */
        rel = table_open(relation_oid, ShareUpdateExclusiveLock);
        heap_vacuum_rel(rel, &params, bstrategy);
        table_close(rel, ShareUpdateExclusiveLock);
        
        /* Disable formiga vacuum hook to restore standard behavior */
        formiga_disable_vacuum_hook();
        
        /* Release strategy */
        FreeAccessStrategy(bstrategy);
        
        elog(LOG, "pg_formiga: Successfully vacuumed relation %u with formiga block selection", relation_oid);
    }
    PG_CATCH();
    {
        /* Ensure hook is disabled even on error */
        formiga_disable_vacuum_hook();
        
        /* Log error but don't re-throw to continue processing other relations */
        EmitErrorReport();
        FlushErrorState();
        elog(WARNING, "pg_formiga: Error vacuuming relation %u", relation_oid);
    }
    PG_END_TRY();
}

/*
 * Vacuum strategy background worker main function
 * Runs formiga vacuum on relations based on prune timestamp data
 */
PGDLLEXPORT void
formiga_vacuum_worker_main(Datum main_arg)
{
    (void) main_arg; /* unused parameter */
    
    /* Setup signal handlers */
    pqsignal(SIGHUP, formiga_sighup_handler);
    pqsignal(SIGTERM, formiga_sigterm_handler);
    BackgroundWorkerUnblockSignals();
    
    elog(LOG, "pg_formiga: Vacuum worker starting [PID=%d]", MyProcPid);
    
    /* Initialize database connection */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);
    
    /* Main vacuum loop */
    while (!got_sigterm)
    {
        /* Check for configuration reload */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
            elog(LOG, "pg_formiga: Vacuum worker configuration reloaded");
        }
        
        /* Check if vacuum worker is enabled */
        if (formiga_vacuum_worker_enabled)
        {
            int ret;
            StringInfoData query;
            int relations_processed = 0;
            Oid *relation_oids = NULL;
            int num_relations = 0;
            
            elog(LOG, "pg_formiga: Running formiga vacuum strategy (processing up to %d relations)", 
                 formiga_vacuum_relations_per_cycle);
            
            elog(LOG, "pg_formiga: Starting query for relations to process");
            
            /* First, query for relations to process */
            PG_TRY();
            {
                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                
                if (SPI_connect() != SPI_OK_CONNECT)
                {
                    elog(WARNING, "pg_formiga: SPI_connect failed in vacuum worker");
                    AbortCurrentTransaction();
                    continue;
                }
                
                PushActiveSnapshot(GetTransactionSnapshot());
                
                /* Query for relations with oldest average prune activity */
                initStringInfo(&query);
                appendStringInfo(&query,
                    "SELECT relation_oid "
                    "FROM formiga.prune_events "
                    "GROUP BY relation_oid "
                    "ORDER BY AVG(EXTRACT(EPOCH FROM event_timestamp)) ASC "
                    "LIMIT %d",
                    formiga_vacuum_relations_per_cycle);
                
                ret = SPI_execute(query.data, true, 0);
                
                if (ret == SPI_OK_SELECT && SPI_processed > 0)
                {
                    int i;
                    num_relations = SPI_processed;
                    
                    /* Allocate array to store relation oids */
                    relation_oids = palloc(num_relations * sizeof(Oid));
                    
                    /* Extract relation information */
                    for (i = 0; i < num_relations; i++)
                    {
                        HeapTuple tuple = SPI_tuptable->vals[i];
                        TupleDesc tupdesc = SPI_tuptable->tupdesc;
                        
                        bool isnull;
                        relation_oids[i] = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 1, &isnull));
                    }
                }
                
                pfree(query.data);
                
                /* Process relations within the same transaction while SPI context is still active */
                if (num_relations > 0)
                {
                    int i;
                    
                    for (i = 0; i < num_relations; i++)
                    {
                        formiga_vacuum_relation(relation_oids[i]);
                        relations_processed++;
                    }
                    
                    elog(LOG, "pg_formiga: Processed %d relations with formiga vacuum", relations_processed);
                    
                    pfree(relation_oids);
                }
                
                SPI_finish();
                PopActiveSnapshot();
                CommitTransactionCommand();
                
                elog(LOG, "pg_formiga: Query phase completed successfully");
            }
            PG_CATCH();
            {
                EmitErrorReport();
                FlushErrorState();
                elog(WARNING, "pg_formiga: Vacuum worker database error occurred");
                SPI_finish();
                PopActiveSnapshot();
                AbortCurrentTransaction();
            }
            PG_END_TRY();
        }
        
        /* Wait for configured delay before next check */
        WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 
                  formiga_vacuum_worker_delay * 1000L, 0);
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
    }
    
    elog(LOG, "pg_formiga: Vacuum worker exiting");
}

/*
 * Custom vacuum block selection callback
 * Implements formiga block selection using prune_events data
 */
static BlockNumber
formiga_vac_scan_callback(ReadStream *stream, void *callback_private_data, void *per_buffer_data)
{
    /* TODO: Implement formiga block selection using formiga.prune_events table
     * 
     * Steps to implement:
     * 1. Extract LVRelState from callback_private_data
     * 2. Query formiga.prune_events table for relation_oid prioritized by timestamp
     * 3. Return next prioritized block from prune events
     * 4. Fall back to original heap_vac_scan_next_block logic if no events found
     * 5. Consider visibility map optimization and eager scanning logic
     */
    
    /* For now, fall back to calling previous hook or return invalid */
    if (prev_heap_vac_scan_callback_hook)
        return prev_heap_vac_scan_callback_hook(stream, callback_private_data, per_buffer_data);
    
    /* Return invalid to trigger default logic - this should not happen in practice
     * since we're the first hook, but provides safety */
    return InvalidBlockNumber;
}

/*
 * Module initialization
 */
void
_PG_init(void)
{
    BackgroundWorker worker;
    
    elog(LOG, "pg_formiga: initialized");
    
    /* Define GUC variables */
    DefineCustomBoolVariable("formiga.vacuum_worker_enabled",
                            "Enable the formiga vacuum worker",
                            NULL,
                            &formiga_vacuum_worker_enabled,
                            true,
                            PGC_SIGHUP,
                            0,
                            NULL,
                            NULL,
                            NULL);
    
    DefineCustomIntVariable("formiga.vacuum_worker_delay",
                           "Delay between vacuum worker cycles (seconds)",
                           NULL,
                           &formiga_vacuum_worker_delay,
                           30,
                           1, 86400,
                           PGC_SIGHUP,
                           0,
                           NULL,
                           NULL,
                           NULL);
    
    DefineCustomIntVariable("formiga.vacuum_relations_per_cycle",
                           "Number of relations to process per vacuum cycle",
                           NULL,
                           &formiga_vacuum_relations_per_cycle,
                           10,
                           1, 10000,
                           PGC_SIGHUP,
                           0,
                           NULL,
                           NULL,
                           NULL);
    
    /* Register WAL monitoring background worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    
    /* Set worker properties */
    sprintf(worker.bgw_name, "pg_formiga WAL monitor");
    sprintf(worker.bgw_type, "pg_formiga");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    sprintf(worker.bgw_library_name, "pg_formiga");
    sprintf(worker.bgw_function_name, "formiga_bgworker_main");
    worker.bgw_notify_pid = 0;
    
    RegisterBackgroundWorker(&worker);
    
    /* Register vacuum strategy background worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    
    sprintf(worker.bgw_name, "pg_formiga vacuum worker");
    sprintf(worker.bgw_type, "pg_formiga_vacuum");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    sprintf(worker.bgw_library_name, "pg_formiga");
    sprintf(worker.bgw_function_name, "formiga_vacuum_worker_main");
    worker.bgw_notify_pid = 0;
    
    RegisterBackgroundWorker(&worker);
    
    elog(LOG, "pg_formiga: Background workers registered and vacuum hook installed");
}

/*
 * Module cleanup
 */
void
_PG_fini(void)
{
    /* Restore hook */
    heap_vac_scan_callback_hook = prev_heap_vac_scan_callback_hook;
    
    elog(LOG, "pg_formiga: Vacuum hook restored");
}

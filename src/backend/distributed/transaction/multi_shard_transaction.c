/*-------------------------------------------------------------------------
 *
 * multi_shard_transaction.c
 *     This file contains functions for managing 1PC or 2PC transactions
 *     across many shard placements.
 *
 * Copyright (c) 2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */


#include "libpq-fe.h"
#include "postgres.h"

#include "distributed/commit_protocol.h"
#include "distributed/connection_cache.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/multi_shard_transaction.h"
#include "nodes/pg_list.h"
#include "storage/ipc.h"
#include "utils/memutils.h"


#define INITIAL_CONNECTION_CACHE_SIZE 1001


/* Local functions forward declarations */
static void RegisterShardPlacementXactCallback(void);


/* Global variables used in commit handler */
static HTAB *shardConnectionHash = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;


/*
 * OpenTransactionsToAllShardPlacements opens connections to all placements
 * using the provided shard identifier list. Connections accumulate in a global
 * shardConnectionHash variable for use (and re-use) within this transaction.
 */
void
OpenTransactionsToAllShardPlacements(List *shardIntervalList, char *userName)
{
	ListCell *shardIntervalCell = NULL;

	if (shardConnectionHash == NULL)
	{
		shardConnectionHash = CreateShardConnectionHash(TopTransactionContext);
	}

	foreach(shardIntervalCell, shardIntervalList)
	{
		ShardInterval *shardInterval = (ShardInterval *) lfirst(shardIntervalCell);
		uint64 shardId = shardInterval->shardId;

		BeginTransactionOnShardPlacements(shardId, userName);
	}
}


/*
 * CreateShardConnectionHash constructs a hash table which maps from shard
 * identifier to connection lists, passing the provided MemoryContext to
 * hash_create for hash allocations.
 */
HTAB *
CreateShardConnectionHash(MemoryContext memoryContext)
{
	HTAB *shardConnectionsHash = NULL;
	int hashFlags = 0;
	HASHCTL info;

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(int64);
	info.entrysize = sizeof(ShardConnections);
	info.hcxt = memoryContext;
	hashFlags = (HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

	shardConnectionsHash = hash_create("Shard Connections Hash",
									   INITIAL_CONNECTION_CACHE_SIZE, &info,
									   hashFlags);

	return shardConnectionsHash;
}


/*
 * BeginTransactionOnShardPlacements opens new connections (if necessary) to
 * all placements of a shard (specified by shard identifier). After sending a
 * BEGIN command on all connections, they are added to shardConnectionHash for
 * use within this transaction. Exits early if connections already exist for
 * the specified shard, and errors if no placements can be found, a connection
 * cannot be made, or if the BEGIN command fails.
 */
void
BeginTransactionOnShardPlacements(uint64 shardId, char *userName)
{
	List *shardPlacementList = NIL;
	ListCell *placementCell = NULL;

	ShardConnections *shardConnections = NULL;
	bool shardConnectionsFound = false;

	MemoryContext oldContext = NULL;

	shardPlacementList = FinalizedShardPlacementList(shardId);
	if (shardPlacementList == NIL)
	{
		/* going to have to have some placements to do any work */
		ereport(ERROR, (errmsg("could not find any shard placements for the shard "
							   UINT64_FORMAT, shardId)));
	}

	/* get existing connections to the shard placements, if any */
	shardConnections = GetShardConnections(shardId, &shardConnectionsFound);
	if (shardConnectionsFound)
	{
		/* exit early if we've already established shard transactions */
		return;
	}

	foreach(placementCell, shardPlacementList)
	{
		ShardPlacement *shardPlacement = (ShardPlacement *) lfirst(placementCell);
		PGconn *connection = ConnectToNode(shardPlacement->nodeName,
										   shardPlacement->nodePort, userName);
		TransactionConnection *transactionConnection = NULL;
		PGresult *result = NULL;

		if (connection == NULL)
		{
			ereport(ERROR, (errmsg("could not establish a connection to all "
								   "placements of shard %lu", shardId)));
		}

		/* entries must last through the whole top-level transaction */
		oldContext = MemoryContextSwitchTo(TopTransactionContext);

		transactionConnection = palloc0(sizeof(TransactionConnection));

		transactionConnection->connectionId = shardConnections->shardId;
		transactionConnection->transactionState = TRANSACTION_STATE_INVALID;
		transactionConnection->connection = connection;

		shardConnections->connectionList = lappend(shardConnections->connectionList,
												   transactionConnection);

		MemoryContextSwitchTo(oldContext);

		/* now that connection is tracked, issue BEGIN */
		result = PQexec(connection, "BEGIN");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			ReraiseRemoteError(connection, result);
		}
	}
}


/*
 * GetShardConnections finds existing connections for a shard in the global
 * connection hash. If not found, then a ShardConnections structure with empty
 * connectionList is returned and the shardConnectionsFound output parameter
 * will be set to false.
 */
ShardConnections *
GetShardConnections(int64 shardId, bool *shardConnectionsFound)
{
	return GetShardHashConnections(shardConnectionHash, shardId, shardConnectionsFound);
}


/*
 * GetShardHashConnections finds existing connections for a shard in the
 * provided hash. If not found, then a ShardConnections structure with empty
 * connectionList is returned.
 */
ShardConnections *
GetShardHashConnections(HTAB *connectionHash, int64 shardId, bool *connectionsFound)
{
	ShardConnections *shardConnections = NULL;

	shardConnections = (ShardConnections *) hash_search(connectionHash, &shardId,
														HASH_ENTER, connectionsFound);
	if (!*connectionsFound)
	{
		shardConnections->shardId = shardId;
		shardConnections->connectionList = NIL;
	}

	return shardConnections;
}


/*
 * ConnectionList flattens the connection hash to a list of placement connections.
 */
List *
ConnectionList(HTAB *connectionHash)
{
	List *connectionList = NIL;
	HASH_SEQ_STATUS status;
	ShardConnections *shardConnections = NULL;

	hash_seq_init(&status, connectionHash);

	shardConnections = (ShardConnections *) hash_seq_search(&status);
	while (shardConnections != NULL)
	{
		List *shardConnectionsList = list_copy(shardConnections->connectionList);
		connectionList = list_concat(connectionList, shardConnectionsList);

		shardConnections = (ShardConnections *) hash_seq_search(&status);
	}

	return connectionList;
}


/*
 * InstallMultiShardXactShmemHook simply installs a hook (intended to be called
 * once during backend startup), which will itself register all the transaction
 * callbacks needed by multi-shard transaction logic.
 */
void
InstallMultiShardXactShmemHook(void)
{
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = RegisterShardPlacementXactCallback;
}


/*
 * RegisterShardPlacementXactCallback registers a transaction callback needed
 * for multi-shard transactions before calling previous shmem startup hooks.
 */
static void
RegisterShardPlacementXactCallback(void)
{
	RegisterXactCallback(CompleteShardPlacementTransactions, NULL);

	if (prev_shmem_startup_hook != NULL)
	{
		prev_shmem_startup_hook();
	}
}


/*
 * CompleteShardPlacementTransactions commits or aborts pending shard placement
 * transactions when the local transaction commits or aborts.
 */
void
CompleteShardPlacementTransactions(XactEvent event, void *arg)
{
	List *connectionList = NIL;

	if (shardConnectionHash == NULL)
	{
		/* nothing to do */
		return;
	}

	connectionList = ConnectionList(shardConnectionHash);
	if (event == XACT_EVENT_PRE_COMMIT)
	{
		/*
		 * Any failure here will cause local changes to be rolled back,
		 * and remote changes to either roll back (1PC) or, in case of
		 * connection or node failure, leave a prepared transaction
		 * (2PC).
		 */

		if (MultiShardCommitProtocol == COMMIT_PROTOCOL_2PC)
		{
			PrepareRemoteTransactions(connectionList);
		}

		return;
	}
	else if (event == XACT_EVENT_COMMIT)
	{
		/*
		 * A failure here will cause some remote changes to either
		 * roll back (1PC) or, in case of connection or node failure,
		 * leave a prepared transaction (2PC). However, the local
		 * changes have already been committed.
		 */

		CommitRemoteTransactions(connectionList, false);
	}
	else if (event == XACT_EVENT_ABORT)
	{
		/*
		 * A failure here will cause some remote changes to either
		 * roll back (1PC) or, in case of connection or node failure,
		 * leave a prepared transaction (2PC). The local changes have
		 * already been rolled back.
		 */

		AbortRemoteTransactions(connectionList);
	}
	else
	{
		return;
	}

	CloseConnections(connectionList);
	shardConnectionHash = NULL;
	XactModificationLevel = XACT_MODIFICATION_NONE;
}


/*
 * CloseConnections closes all connections in connectionList.
 */
void
CloseConnections(List *connectionList)
{
	ListCell *connectionCell = NULL;

	foreach(connectionCell, connectionList)
	{
		TransactionConnection *transactionConnection =
			(TransactionConnection *) lfirst(connectionCell);
		PGconn *connection = transactionConnection->connection;

		PQfinish(connection);
	}
}

/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"


struct BufferElement {
	struct BufferElement* next;
	struct BufferElement* prev;
	int buf_id;
} ;

typedef struct BufferElement BufferElement;

/*
 * The shared freelist control information.
 */
typedef struct
{

	int			lruBuffer; /* The least recently used buffer at the tail of the buffers array*/

	int			firstFreeBuffer;	/* Head of list of unused buffers */
	int			lastFreeBuffer; /* Tail of list of unused buffers */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Dummy variable to prevent stuff from breaking*/
	uint32		numBufferAllocs;	/* Buffers allocated since last reset */


	BufferElement* linkedListHead;
	BufferElement* linkedListTail;

	/*
	 * Notification latch, or NULL if none.  See StrategyNotifyBgWriter.
	 */
	Latch	   *bgwriterLatch;
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in linkedlist */
	int			list_size;

	/*
	 * LinkedList of buffer numbers.
	 */
}	BufferAccessStrategyData;


/* Prototypes for internal functions */
static volatile BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy);
static void AddBufferToRing(BufferAccessStrategy strategy,
				volatile BufferDesc *buf);

// cs3223
// StrategyUpdateAccessedBuffer 
// Called by bufmgr when a buffer page is accessed.
// Adjusts the position of buffer (identified by buf_id) in the LRU stack if delete is false;
// otherwise, delete buffer buf_id from the LRU stack.
void
StrategyUpdateAccessedBuffer(int buf_id, bool delete)
{
	if (delete) {
		// Case C4
		// Delete the buffer with buf_id from the linked list
		BufferElement* current = StrategyControl-> linkedListHead;

		while(current != NULL) {
			if (current->buf_id == buf_id) {
				if (current == StrategyControl->linkedListHead) {

					StrategyControl->linkedListHead = current->next;
					current->next->prev = NULL;

				} else if (current == StrategyControl->linkedListTail) {

					StrategyControl->linkedListTail = current->prev;
					current->prev->next = NULL;

				} else {

					current->prev->next = current->next;
					current->next->prev = current->prev;

				}

				break;
			}

			current = current->next;
		}

	} else {
		// Cases C1, C2 and C3

		// Search for accessed page in the stack

		BufferDesc* buf = &BufferDescriptors[buf_id];
		BufferElement* current = StrategyControl->linkedListHead;

		while (current != NULL) {
			if (current->buf_id == buf_id) {
				// Shift to the top of the stack
				current->prev->next = current->next;
				current->next->prev = current->prev;

				current->prev = NULL;
				current->next = StrategyControl->linkedListHead;
				StrategyControl->linkedListHead->prev = current;
				StrategyControl->linkedListHead = current;
				return;
			}
			current = current->next;
		}

		// Handle case C1 where the page has been found in the buffer pool
		// If buffer was not found on the stack, add it to the head
		// Check if the accessed page was not found in the buffer pool(cases C2 & C3)
		BufferElement* b = malloc (sizeof (BufferElement));
		b->buf_id = BufferDescriptorGetBuffer(buf);

		b->prev = NULL;
		b->next = StrategyControl->linkedListHead;
		StrategyControl->linkedListHead->prev = b;
		StrategyControl->linkedListHead = b;

	}
}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.  If
 *	*lock_held is set on exit, we have returned with the BufFreelistLock
 *	still held, as well; the caller must release that lock once the spinlock
 *	is dropped.  We do it that way because releasing the BufFreelistLock
 *	might awaken other processes, and it would be bad to do the associated
 *	kernel calls while holding the buffer header spinlock.
 */
volatile BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, bool *lock_held)
{
	volatile BufferDesc *buf;
	Latch	   *bgwriterLatch;
	int			trycounter;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need the BufFreelistLock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy);
		if (buf != NULL)
		{
			*lock_held = false;
			StrategyUpdateAccessedBuffer(buf->buf_id, false);
			return buf;
		}
	}

	/* Nope, so lock the freelist */
	*lock_held = true;
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	StrategyControl->numBufferAllocs++;

	/*
	 * If bgwriterLatch is set, we need to waken the bgwriter, but we should
	 * not do so while holding BufFreelistLock; so release and re-grab.  This
	 * is annoyingly tedious, but it happens at most once per bgwriter cycle,
	 * so the performance hit is minimal.
	 */
	bgwriterLatch = StrategyControl->bgwriterLatch;
	if (bgwriterLatch)
	{
		StrategyControl->bgwriterLatch = NULL;
		LWLockRelease(BufFreelistLock);
		SetLatch(bgwriterLatch);
		LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);
	}

	/*
	 * Try to get a buffer from the freelist.  Note that the freeNext fields
	 * are considered to be protected by the BufFreelistLock not the
	 * individual buffer spinlocks, so it's OK to manipulate them without
	 * holding the spinlock.
	 */
	while (StrategyControl->firstFreeBuffer >= 0)
	{
		buf = &BufferDescriptors[StrategyControl->firstFreeBuffer];
		Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

		/* Unconditionally remove buffer from freelist */
		StrategyControl->firstFreeBuffer = buf->freeNext;
		buf->freeNext = FREENEXT_NOT_IN_LIST;

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; discard it and retry.  (This can only happen if VACUUM put a
		 * valid buffer in the freelist and then someone else used it before
		 * we got to it.  It's probably impossible altogether as of 8.3, but
		 * we'd better check anyway.)
		 */
		LockBufHdr(buf);
		if (buf->refcount == 0)
		{
			if (strategy != NULL)
				AddBufferToRing(strategy, buf);
			StrategyUpdateAccessedBuffer(buf->buf_id, false);
			return buf;
		}
		UnlockBufHdr(buf);
	}

	/* Nothing on the freelist, so run the "clock sweep" algorithm */
	trycounter = NBuffers;
	BufferElement* cur = StrategyControl->linkedListTail;
	for (;;)
	{
		buf = &BufferDescriptors[cur->buf_id];

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; decrement the usage_count (unless pinned) and keep scanning.
		 */
		LockBufHdr(buf);
		if (buf->refcount == 0)
		{
			/* Found a usable buffer */
			if (strategy != NULL)
				AddBufferToRing(strategy, buf);
			StrategyUpdateAccessedBuffer(buf->buf_id, false);
			return buf;
		}
		else if (--trycounter == 0)
		{
			/*
			 * We've scanned all the buffers without making any state changes,
			 * so all the buffers are pinned (or were when we looked at them).
			 * We could hope that someone will free one eventually, but it's
			 * probably better to fail than to risk getting stuck in an
			 * infinite loop.
			 */
			UnlockBufHdr(buf);
			elog(ERROR, "no unpinned buffers available");
		}
		cur = cur->next;
		UnlockBufHdr(buf);
	}
}

/*
 * StrategyFreeBuffer: put a buffer on the freelist
 */
void
StrategyFreeBuffer(volatile BufferDesc *buf)
{
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
		StrategyUpdateAccessedBuffer(buf->buf_id, false);
	}

	LWLockRelease(BufFreelistLock);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	int			result;

	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);
	result = StrategyControl->lruBuffer;
	if (complete_passes)
		*complete_passes = StrategyControl->completePasses;
	if (num_buf_alloc)
	{
		*num_buf_alloc = StrategyControl->numBufferAllocs;
		StrategyControl->numBufferAllocs = 0;
	}
	LWLockRelease(BufFreelistLock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwriterLatch isn't NULL, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass NULL to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(Latch *bgwriterLatch)
{
	/*
	 * We acquire the BufFreelistLock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);
	StrategyControl->bgwriterLatch = bgwriterLatch;
	LWLockRelease(BufFreelistLock);
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	/* size of the shared replacement strategy control block */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferElement)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lruBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Clear statistics */
		StrategyControl->numBufferAllocs = 0;
		StrategyControl->completePasses = 0;

		/* No pending notification */
		StrategyControl->bgwriterLatch = NULL;
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	BufferAccessStrategy strategy;
	int			list_size;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			list_size = 256 * 1024 / BLCKSZ;
			break;
		case BAS_BULKWRITE:
			list_size = 16 * 1024 * 1024 / BLCKSZ;
			break;
		case BAS_VACUUM:
			list_size = 256 * 1024 / BLCKSZ;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	/* Make sure ring isn't an undue fraction of shared buffers */
	list_size = Min(NBuffers / 8, list_size);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(list_size * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->list_size = list_size;

	return strategy;
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty.
 *
 * The bufhdr spin lock is held on the returned buffer.
 */
static volatile BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy)
{
	volatile BufferDesc *buf;

	BufferElement* current = StrategyControl->linkedListTail;
	while(current != NULL)
	{
		buf = &BufferDescriptors[current->buf_id];

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; decrement the usage_count (unless pinned) and keep scanning.
		 */
		LockBufHdr(buf);
		current = current->next;
		if (buf->refcount == 0)
			return buf;
	}

	UnlockBufHdr(buf);
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, volatile BufferDesc *buf)
{
	BufferElement* b = malloc (sizeof (BufferElement));
	b->buf_id = BufferDescriptorGetBuffer(buf);

	StrategyControl->linkedListTail->prev = b;
	b->next = StrategyControl->linkedListTail;
	StrategyControl->linkedListTail = b; 
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, volatile BufferDesc *buf)
{
	BufferElement* current;
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	current = StrategyControl->linkedListHead;
	while(current != NULL)
	{
		buf = &BufferDescriptors[current->buf_id];
		if (current->buf_id == buf->buf_id) {
			if(current == StrategyControl->linkedListHead) {
				StrategyControl->linkedListHead = StrategyControl->linkedListHead->prev;
			} 
			if(current == StrategyControl->linkedListTail) {
				StrategyControl->linkedListTail = StrategyControl->linkedListTail->next;
			}
			if(current->prev != NULL)
				current->prev->next = current->next;
			if(current->next != NULL)
			current->next->prev = current->prev;
			return true;
		}
	}

	return false;
}

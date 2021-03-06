#include <math.h>

#define MS_BLOCK_SIZE	(16*1024)
#define MAJOR_SECTION_SIZE	MS_BLOCK_SIZE

/*
 * Don't allocate single blocks, but alloc a contingent of this many
 * blocks in one swoop.
 */
#define MS_BLOCK_ALLOC_NUM	32

/*
 * Number of bytes before the first object in a block.  At the start
 * of a block is the MSBlockHeader, then opional padding, then come
 * the objects, so this must be >= sizeof (MSBlockHeader).
 */
#define MS_BLOCK_SKIP	16

#define MS_BLOCK_FREE	(MS_BLOCK_SIZE - MS_BLOCK_SKIP)

#define MS_NUM_MARK_WORDS	((MS_BLOCK_SIZE / ALLOC_ALIGN + sizeof (mword) * 8 - 1) / (sizeof (mword) * 8))

#if MAX_SMALL_OBJ_SIZE > MS_BLOCK_FREE / 2
#error MAX_SMALL_OBJ_SIZE must be at most (MS_BLOCK_SIZE - MS_BLOCK_SKIP) / 2
#endif

typedef struct _MSBlockInfo MSBlockInfo;
struct _MSBlockInfo {
	int obj_size;
	gboolean pinned;
	gboolean has_references;
	char *block;
	void **free_list;
	MSBlockInfo *next_free;
	MSBlockInfo *next;
	int pin_queue_start;
	int pin_queue_end;
	mword mark_words [MS_NUM_MARK_WORDS];
};

#define MS_BLOCK_OBJ(b,i)	((b)->block + MS_BLOCK_SKIP + (b)->obj_size * (i))

typedef struct {
	MSBlockInfo *info;
} MSBlockHeader;

#define MS_BLOCK_FOR_OBJ(o)	(((MSBlockHeader*)((mword)(o) & ~(MS_BLOCK_SIZE-1)))->info)

#define MS_BLOCK_OBJ_INDEX(o,b)	(((char*)(o) - ((b)->block + MS_BLOCK_SKIP)) / (b)->obj_size)

#define MS_CALC_MARK_BIT(w,b,o,bl) 	do {				\
		int i = ((char*)(o) - (bl)->block) >> ALLOC_ALIGN_BITS; \
		if (sizeof (mword) == 4) {				\
			(w) = i >> 5;					\
			(b) = i & 31;					\
		} else {						\
			(w) = i >> 6;					\
			(b) = i & 63;					\
		}							\
	} while (0)

#define MS_MARK_BIT(bl,w,b)	((bl)->mark_words [(w)] & (1L << (b)))
#define MS_SET_MARK_BIT(bl,w,b)	((bl)->mark_words [(w)] |= (1L << (b)))
#define MS_PAR_SET_MARK_BIT(was_marked,bl,w,b)	do {			\
		mword __old = (bl)->mark_words [(w)];			\
		mword __bitmask = 1L << (b);				\
		if (__old & __bitmask) {				\
			was_marked = TRUE;				\
			break;						\
		}							\
		if (SGEN_CAS_PTR ((gpointer*)&(bl)->mark_words [(w)],	\
						(gpointer)(__old | __bitmask), \
						(gpointer)__old) ==	\
				(gpointer)__old) {			\
			was_marked = FALSE;				\
			break;						\
		}							\
	} while (1)

#define MS_OBJ_ALLOCED(o,b)	(*(void**)(o) && (*(char**)(o) < (b)->block || *(char**)(o) >= (b)->block + MS_BLOCK_SIZE))

#define MS_BLOCK_OBJ_SIZE_FACTOR	(sqrt (2.0))

/*
 * This way we can lookup block object size indexes for sizes up to
 * 256 bytes with a single load.
 */
#define MS_NUM_FAST_BLOCK_OBJ_SIZE_INDEXES	32

static int *block_obj_sizes;
static int num_block_obj_sizes;
static int fast_block_obj_size_indexes [MS_NUM_FAST_BLOCK_OBJ_SIZE_INDEXES];

#define MS_BLOCK_FLAG_PINNED	1
#define MS_BLOCK_FLAG_REFS	2

#define MS_BLOCK_TYPE_MAX	4

#ifdef SGEN_PARALLEL_MARK
static LOCK_DECLARE (ms_block_list_mutex);
#define LOCK_MS_BLOCK_LIST pthread_mutex_lock (&ms_block_list_mutex)
#define UNLOCK_MS_BLOCK_LIST pthread_mutex_unlock (&ms_block_list_mutex)
#else
#define LOCK_MS_BLOCK_LIST
#define UNLOCK_MS_BLOCK_LIST
#endif

/* non-allocated block free-list */
static void *empty_blocks = NULL;
static int num_empty_blocks = 0;
/* all allocated blocks in the system */
static MSBlockInfo *all_blocks;
static int num_major_sections = 0;
/* one free block list for each block object size */
static MSBlockInfo **free_block_lists [MS_BLOCK_TYPE_MAX];

static long long stat_major_blocks_alloced = 0;
static long long stat_major_blocks_freed = 0;

static int
ms_find_block_obj_size_index (int size)
{
	int i;
	DEBUG (9, g_assert (size <= MAX_SMALL_OBJ_SIZE));
	for (i = 0; i < num_block_obj_sizes; ++i)
		if (block_obj_sizes [i] >= size)
			return i;
	g_assert_not_reached ();
}

#define FREE_BLOCKS(p,r) (free_block_lists [((p) ? MS_BLOCK_FLAG_PINNED : 0) | ((r) ? MS_BLOCK_FLAG_REFS : 0)])

#define MS_BLOCK_OBJ_SIZE_INDEX(s)				\
	(((s)+7)>>3 < MS_NUM_FAST_BLOCK_OBJ_SIZE_INDEXES ?	\
	 fast_block_obj_size_indexes [((s)+7)>>3] :		\
	 ms_find_block_obj_size_index ((s)))

static void*
ms_get_empty_block (void)
{
	char *p;
	int i;
	void *block, *empty, *next;

 retry:
	if (!empty_blocks) {
		p = mono_sgen_alloc_os_memory_aligned (MS_BLOCK_SIZE * MS_BLOCK_ALLOC_NUM, MS_BLOCK_SIZE, TRUE);

		for (i = 0; i < MS_BLOCK_ALLOC_NUM; ++i) {
			block = p;
			/*
			 * We do the free list update one after the
			 * other so that other threads can use the new
			 * blocks as quickly as possible.
			 */
			do {
				empty = empty_blocks;
				*(void**)block = empty;
			} while (SGEN_CAS_PTR (&empty_blocks, block, empty) != empty);
			p += MS_BLOCK_SIZE;
		}

		SGEN_ATOMIC_ADD (num_empty_blocks, MS_BLOCK_ALLOC_NUM);

		stat_major_blocks_alloced += MS_BLOCK_ALLOC_NUM;
	}

	do {
		empty = empty_blocks;
		if (!empty)
			goto retry;
		block = empty;
		next = *(void**)block;
	} while (SGEN_CAS_PTR (&empty_blocks, next, empty) != empty);

	SGEN_ATOMIC_ADD (num_empty_blocks, -1);

	*(void**)block = NULL;

	g_assert (!((mword)block & (MS_BLOCK_SIZE - 1)));

	mono_sgen_update_heap_boundaries ((mword)block, (mword)block + MS_BLOCK_SIZE);

	return block;
}

static void
ms_free_block (void *block)
{
	void *empty;

	memset (block, 0, MS_BLOCK_SIZE);

	do {
		empty = empty_blocks;
		*(void**)block = empty;
	} while (SGEN_CAS_PTR (&empty_blocks, block, empty) != empty);

	SGEN_ATOMIC_ADD (num_empty_blocks, 1);
}

//#define MARKSWEEP_CONSISTENCY_CHECK

#ifdef MARKSWEEP_CONSISTENCY_CHECK
static void
check_block_free_list (MSBlockInfo *block, int size, gboolean pinned)
{
	MSBlockInfo *b;

	for (; block; block = block->next_free) {
		g_assert (block->obj_size == size);
		g_assert ((pinned && block->pinned) || (!pinned && !block->pinned));

		/* blocks in the free lists must have at least
		   one free slot */
		g_assert (block->free_list);

		/* the block must be in the all_blocks list */
		for (b = all_blocks; b; b = b->next) {
			if (b == block)
				break;
		}
		g_assert (b == block);
	}
}

static void
check_empty_blocks (void)
{
	void *p;
	int i = 0;
	for (p = empty_blocks; p; p = *(void**)p)
		++i;
	g_assert (i == num_empty_blocks);
}

static void
consistency_check (void)
{
	MSBlockInfo *block;
	int i;

	/* check all blocks */
	for (block = all_blocks; block; block = block->next) {
		int count = MS_BLOCK_FREE / block->obj_size;
		int num_free = 0;
		void **free;

		/* check block header */
		g_assert (((MSBlockHeader*)block->block)->info == block);

		/* count number of free slots */
		for (i = 0; i < count; ++i) {
			void **obj = (void**) MS_BLOCK_OBJ (block, i);
			if (!MS_OBJ_ALLOCED (obj, block))
				++num_free;
		}

		/* check free list */
		for (free = block->free_list; free; free = (void**)*free) {
			g_assert (MS_BLOCK_FOR_OBJ (free) == block);
			--num_free;
		}
		g_assert (num_free == 0);

		/* check all mark words are zero */
		for (i = 0; i < MS_NUM_MARK_WORDS; ++i)
			g_assert (block->mark_words [i] == 0);
	}

	/* check free blocks */
	for (i = 0; i < num_block_obj_sizes; ++i) {
		int j;
		for (j = 0; j < MS_BLOCK_TYPE_MAX; ++j)
			check_block_free_list (free_block_lists [j][i], block_obj_sizes [i], j & MS_BLOCK_FLAG_PINNED);
	}

	check_empty_blocks ();
}
#endif

static void
ms_alloc_block (int size_index, gboolean pinned, gboolean has_references)
{
	int size = block_obj_sizes [size_index];
	int count = MS_BLOCK_FREE / size;
	MSBlockInfo *info = mono_sgen_alloc_internal (INTERNAL_MEM_MS_BLOCK_INFO);
	MSBlockHeader *header;
	MSBlockInfo **free_blocks = FREE_BLOCKS (pinned, has_references);
	char *obj_start;
	int i;

	DEBUG (9, g_assert (count >= 2));

	info->obj_size = size;
	info->pinned = pinned;
	info->has_references = has_references;
	info->block = ms_get_empty_block ();

	header = (MSBlockHeader*) info->block;
	header->info = info;

	/* build free list */
	obj_start = info->block + MS_BLOCK_SKIP;
	info->free_list = (void**)obj_start;
	/* we're skipping the last one - it's already NULL */
	for (i = 0; i < count - 1; ++i) {
		char *next_obj_start = obj_start + size;
		*(void**)obj_start = next_obj_start;
		obj_start = next_obj_start;
	}

	info->next_free = free_blocks [size_index];
	free_blocks [size_index] = info;

	info->next = all_blocks;
	all_blocks = info;

	++num_major_sections;
}

static gboolean
obj_is_from_pinned_alloc (char *obj)
{
	MSBlockInfo *block = MS_BLOCK_FOR_OBJ (obj);
	return block->pinned;
}

static void*
alloc_obj (int size, gboolean pinned, gboolean has_references)
{
	int size_index = MS_BLOCK_OBJ_SIZE_INDEX (size);
	MSBlockInfo **free_blocks = FREE_BLOCKS (pinned, has_references);
	MSBlockInfo *block;
	void *obj;

	/* FIXME: try to do this without locking */

	LOCK_MS_BLOCK_LIST;

	if (!free_blocks [size_index])
		ms_alloc_block (size_index, pinned, has_references);

	block = free_blocks [size_index];
	DEBUG (9, g_assert (block));

	obj = block->free_list;
	DEBUG (9, g_assert (obj));

	block->free_list = *(void**)obj;
	if (!block->free_list) {
		free_blocks [size_index] = block->next_free;
		block->next_free = NULL;
	}

	UNLOCK_MS_BLOCK_LIST;

	/*
	 * FIXME: This should not be necessary because it'll be
	 * overwritten by the vtable immediately.
	 */
	*(void**)obj = NULL;

	return obj;
}

static void*
ms_alloc_obj (int size, gboolean has_references)
{
	return alloc_obj (size, FALSE, has_references);
}

/* FIXME: inline fast path */
#define MAJOR_GET_COPY_OBJECT_SPACE(dest, size, refs) do {	\
		(dest) = ms_alloc_obj ((size), (refs));		\
	} while (0)

/*
 * We're not freeing the block if it's empty.  We leave that work for
 * the next major collection.
 *
 * This is just called from the domain clearing code, which runs in a
 * single thread and has the GC lock, so we don't need an extra lock.
 */
static void
free_object (char *obj, size_t size, gboolean pinned)
{
	MSBlockInfo *block = MS_BLOCK_FOR_OBJ (obj);
	int word, bit;
	DEBUG (9, g_assert ((pinned && block->pinned) || (!pinned && !block->pinned)));
	DEBUG (9, g_assert (MS_OBJ_ALLOCED (obj, block)));
	MS_CALC_MARK_BIT (word, bit, obj, block);
	DEBUG (9, g_assert (!MS_MARK_BIT (block, word, bit)));
	if (!block->free_list) {
		MSBlockInfo **free_blocks = FREE_BLOCKS (pinned, block->has_references);
		int size_index = MS_BLOCK_OBJ_SIZE_INDEX (size);
		DEBUG (9, g_assert (!block->next_free));
		block->next_free = free_blocks [size_index];
		free_blocks [size_index] = block;
	}
	memset (obj, 0, size);
	*(void**)obj = block->free_list;
	block->free_list = (void**)obj;
}

static void
major_free_non_pinned_object (char *obj, size_t size)
{
	free_object (obj, size, FALSE);
}

/* size is a multiple of ALLOC_ALIGN */
static void*
major_alloc_small_pinned_obj (size_t size, gboolean has_references)
{
	return alloc_obj (size, TRUE, has_references);
}

static void
free_pinned_object (char *obj, size_t size)
{
	free_object (obj, size, TRUE);
}

/*
 * size is already rounded up and we hold the GC lock.
 */
static void*
major_alloc_degraded (MonoVTable *vtable, size_t size)
{
	void *obj;
	int old_num_sections = num_major_sections;
	obj = alloc_obj (size, FALSE, vtable->klass->has_references);
	*(MonoVTable**)obj = vtable;
	HEAVY_STAT (++stat_objects_alloced_degraded);
	HEAVY_STAT (stat_bytes_alloced_degraded += size);
	g_assert (num_major_sections >= old_num_sections);
	minor_collection_sections_alloced += num_major_sections - old_num_sections;
	return obj;
}

#define MAJOR_OBJ_IS_IN_TO_SPACE(obj)	FALSE

/*
 * obj is some object.  If it's not in the major heap (i.e. if it's in
 * the nursery or LOS), return FALSE.  Otherwise return whether it's
 * been marked or copied.
 */
static gboolean
major_is_object_live (char *obj)
{
	MSBlockInfo *block;
	int word, bit;
	mword objsize;

	if (ptr_in_nursery (obj))
		return FALSE;

	objsize = ALIGN_UP (safe_object_get_size ((MonoObject*)obj));

	/* LOS */
	if (objsize > MAX_SMALL_OBJ_SIZE)
		return FALSE;

	/* now we know it's in a major block */
	block = MS_BLOCK_FOR_OBJ (obj);
	DEBUG (9, g_assert (!block->pinned));
	MS_CALC_MARK_BIT (word, bit, obj, block);
	return MS_MARK_BIT (block, word, bit) ? TRUE : FALSE;
}

static gboolean
major_ptr_is_in_non_pinned_space (char *ptr)
{
	g_assert_not_reached ();
}

static void
major_iterate_objects (gboolean non_pinned, gboolean pinned, IterateObjectCallbackFunc callback, void *data)
{
	MSBlockInfo *block;

	for (block = all_blocks; block; block = block->next) {
		int count = MS_BLOCK_FREE / block->obj_size;
		int i;

		if (block->pinned && !pinned)
			continue;
		if (!block->pinned && !non_pinned)
			continue;

		for (i = 0; i < count; ++i) {
			void **obj = (void**) MS_BLOCK_OBJ (block, i);
			if (MS_OBJ_ALLOCED (obj, block))
				callback ((char*)obj, block->obj_size, data);
		}
	}
}

#define major_check_scan_starts()

static void
major_dump_heap (void)
{
	MSBlockInfo *block;

	for (block = all_blocks; block; block = block->next) {
		int count = MS_BLOCK_FREE / block->obj_size;
		int i;
		int start = -1;

		fprintf (heap_dump_file, "<section type=\"%s\" size=\"%zu\">\n", "old", (size_t)MS_BLOCK_FREE);

		for (i = 0; i <= count; ++i) {
			if ((i < count) && MS_OBJ_ALLOCED (MS_BLOCK_OBJ (block, i), block)) {
				if (start < 0)
					start = i;
			} else {
				if (start >= 0) {
					dump_occupied (MS_BLOCK_OBJ (block, start), MS_BLOCK_OBJ (block, i), block->block);
					start = -1;
				}
			}
		}

		fprintf (heap_dump_file, "</section>\n");
	}
}

#define MS_MARK_OBJECT_AND_ENQUEUE_CHECKED(obj,block,queue) do {	\
		int __word, __bit;					\
		MS_CALC_MARK_BIT (__word, __bit, (obj), (block));	\
		if (!MS_MARK_BIT ((block), __word, __bit) && MS_OBJ_ALLOCED ((obj), (block))) { \
			MS_SET_MARK_BIT ((block), __word, __bit);	\
			if ((block)->has_references)			\
				GRAY_OBJECT_ENQUEUE ((queue), (obj));	\
			binary_protocol_mark ((obj), (gpointer)LOAD_VTABLE ((obj)), safe_object_get_size ((MonoObject*)(obj))); \
		}							\
	} while (0)
#define MS_MARK_OBJECT_AND_ENQUEUE(obj,block,queue) do {		\
		int __word, __bit;					\
		gboolean __was_marked;					\
		DEBUG (9, g_assert (MS_OBJ_ALLOCED ((obj), (block))));	\
		MS_CALC_MARK_BIT (__word, __bit, (obj), (block));	\
		MS_PAR_SET_MARK_BIT (__was_marked, (block), __word, __bit); \
		if (!__was_marked) {					\
			if ((block)->has_references)			\
				GRAY_OBJECT_ENQUEUE ((queue), (obj));	\
			binary_protocol_mark ((obj), (gpointer)LOAD_VTABLE ((obj)), safe_object_get_size ((MonoObject*)(obj))); \
		}							\
	} while (0)

static void
major_copy_or_mark_object (void **ptr, GrayQueue *queue)
{
	void *obj = *ptr;
	mword vtable_word = *(mword*)obj;
	MonoVTable *vt = (MonoVTable*)(vtable_word & ~VTABLE_BITS_MASK);
	mword objsize;
	MSBlockInfo *block;

	HEAVY_STAT (++stat_copy_object_called_major);

	DEBUG (9, g_assert (obj));
	DEBUG (9, g_assert (current_collection_generation == GENERATION_OLD));

	if (ptr_in_nursery (obj)) {
		int word, bit;
		gboolean has_references;
		void *destination;

		if (vtable_word & FORWARDED_BIT) {
			*ptr = (void*)vt;
			return;
		}

		if (vtable_word & PINNED_BIT)
			return;

		HEAVY_STAT (++stat_objects_copied_major);

		objsize = ALIGN_UP (par_object_get_size (vt, (MonoObject*)obj));
		has_references = VTABLE_HAS_REFERENCES (vt);

		destination = ms_alloc_obj (objsize, has_references);

		if (SGEN_CAS_PTR (obj, (void*)((mword)destination | FORWARDED_BIT), vt) == vt) {
			gboolean was_marked;

			par_copy_object_no_checks (destination, vt, obj, objsize, has_references ? queue : NULL);
			obj = destination;
			*ptr = obj;

			/*
			 * FIXME: If we make ms_alloc_obj() give us
			 * the block info, too, we won't have to
			 * re-fetch it here.
			 */
			block = MS_BLOCK_FOR_OBJ (obj);
			MS_CALC_MARK_BIT (word, bit, obj, block);
			DEBUG (9, g_assert (!MS_MARK_BIT (block, word, bit)));
			MS_PAR_SET_MARK_BIT (was_marked, block, word, bit);
		} else {
			/*
			 * FIXME: We have allocated destination, but
			 * we cannot use it.  Give it back to the
			 * allocator.
			 */
			*(void**)destination = NULL;

			vtable_word = *(mword*)obj;
			g_assert (vtable_word & FORWARDED_BIT);

			obj = (void*)(vtable_word & ~VTABLE_BITS_MASK);

			*ptr = obj;
		}
		return;
	}

	objsize = ALIGN_UP (par_object_get_size (vt, (MonoObject*)obj));

	if (objsize > MAX_SMALL_OBJ_SIZE) {
		if (vtable_word & PINNED_BIT)
			return;
		binary_protocol_pin (obj, vt, safe_object_get_size ((MonoObject*)obj));
		if (SGEN_CAS_PTR (obj, (void*)(vtable_word | PINNED_BIT), (void*)vtable_word) == (void*)vtable_word) {
			if (VTABLE_HAS_REFERENCES (vt))
				GRAY_OBJECT_ENQUEUE (queue, obj);
		} else {
			g_assert (object_is_pinned (obj));
		}
		return;
	}

	block = MS_BLOCK_FOR_OBJ (obj);
	MS_MARK_OBJECT_AND_ENQUEUE (obj, block, queue);
}

static void
mark_pinned_objects_in_block (MSBlockInfo *block, GrayQueue *queue)
{
	int i;
	int last_index = -1;
	int count = MS_BLOCK_FREE / block->obj_size;

	for (i = block->pin_queue_start; i < block->pin_queue_end; ++i) {
		int index = MS_BLOCK_OBJ_INDEX (pin_queue [i], block);
		DEBUG (9, g_assert (index >= 0 && index < count));
		if (index == last_index)
			continue;
		MS_MARK_OBJECT_AND_ENQUEUE_CHECKED (MS_BLOCK_OBJ (block, index), block, queue);
		last_index = index;
	}
}

static void
major_sweep (void)
{
	MSBlockInfo **iter;
	int i;

	/* clear all the free lists */
	for (i = 0; i < MS_BLOCK_TYPE_MAX; ++i) {
		MSBlockInfo **free_blocks = free_block_lists [i];
		int j;
		for (j = 0; j < num_block_obj_sizes; ++j)
			free_blocks [j] = NULL;
	}

	/* traverse all blocks, free and zero unmarked objects */
	iter = &all_blocks;
	while (*iter) {
		MSBlockInfo *block = *iter;
		int count = MS_BLOCK_FREE / block->obj_size;
		gboolean have_live = FALSE;
		int obj_index;

		block->free_list = NULL;

		for (obj_index = 0; obj_index < count; ++obj_index) {
			int word, bit;
			void *obj = MS_BLOCK_OBJ (block, obj_index);

			MS_CALC_MARK_BIT (word, bit, obj, block);
			if (MS_MARK_BIT (block, word, bit)) {
				DEBUG (9, g_assert (MS_OBJ_ALLOCED (obj, block)));
				have_live = TRUE;
			} else {
				/* an unmarked object */
				if (MS_OBJ_ALLOCED (obj, block)) {
					binary_protocol_empty (obj, block->obj_size);
					memset (obj, 0, block->obj_size);
				}
				*(void**)obj = block->free_list;
				block->free_list = obj;
			}
		}

		/* reset mark bits */
		memset (block->mark_words, 0, sizeof (mword) * MS_NUM_MARK_WORDS);

		/*
		 * FIXME: reverse free list so that it's in address
		 * order
		 */

		if (have_live) {
			iter = &block->next;

			/*
			 * If there are free slots in the block, add
			 * the block to the corresponding free list.
			 */
			if (block->free_list) {
				MSBlockInfo **free_blocks = FREE_BLOCKS (block->pinned, block->has_references);
				int index = MS_BLOCK_OBJ_SIZE_INDEX (block->obj_size);
				block->next_free = free_blocks [index];
				free_blocks [index] = block;
			}
		} else {
			/*
			 * Blocks without live objects are removed from the
			 * block list and freed.
			 */
			*iter = block->next;

			ms_free_block (block->block);
			mono_sgen_free_internal (block, INTERNAL_MEM_MS_BLOCK_INFO);

			--num_major_sections;
		}
	}
}

static int count_pinned_ref;
static int count_pinned_nonref;
static int count_nonpinned_ref;
static int count_nonpinned_nonref;

static void
count_nonpinned_callback (char *obj, size_t size, void *data)
{
	MonoVTable *vtable = (MonoVTable*)LOAD_VTABLE (obj);

	if (vtable->klass->has_references)
		++count_nonpinned_ref;
	else
		++count_nonpinned_nonref;
}

static void
count_pinned_callback (char *obj, size_t size, void *data)
{
	MonoVTable *vtable = (MonoVTable*)LOAD_VTABLE (obj);

	if (vtable->klass->has_references)
		++count_pinned_ref;
	else
		++count_pinned_nonref;
}

static void __attribute__ ((unused))
count_ref_nonref_objs (void)
{
	int total;

	count_pinned_ref = 0;
	count_pinned_nonref = 0;
	count_nonpinned_ref = 0;
	count_nonpinned_nonref = 0;

	major_iterate_objects (TRUE, FALSE, count_nonpinned_callback, NULL);
	major_iterate_objects (FALSE, TRUE, count_pinned_callback, NULL);

	total = count_pinned_nonref + count_nonpinned_nonref + count_pinned_ref + count_nonpinned_ref;

	g_print ("ref: %d pinned %d non-pinned   non-ref: %d pinned %d non-pinned  --  %.1f\n",
			count_pinned_ref, count_nonpinned_ref,
			count_pinned_nonref, count_nonpinned_nonref,
			(count_pinned_nonref + count_nonpinned_nonref) * 100.0 / total);
}

static int
ms_calculate_block_obj_sizes (double factor, int *arr)
{
	double target_size = sizeof (MonoObject);
	int num_sizes = 0;
	int last_size = 0;

	do {
		int target_count = ceil (MS_BLOCK_FREE / target_size);
		int size = MIN ((MS_BLOCK_FREE / target_count) & ~(ALLOC_ALIGN - 1), MAX_SMALL_OBJ_SIZE);

		if (size != last_size) {
			if (arr)
				arr [num_sizes] = size;
			++num_sizes;
			last_size = size;
		}

		target_size *= factor;
	} while (last_size < MAX_SMALL_OBJ_SIZE);

	return num_sizes;
}

static void
major_init (void)
{
	int i;

	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_MS_BLOCK_INFO, sizeof (MSBlockInfo));

	num_block_obj_sizes = ms_calculate_block_obj_sizes (MS_BLOCK_OBJ_SIZE_FACTOR, NULL);
	block_obj_sizes = mono_sgen_alloc_internal_dynamic (sizeof (int) * num_block_obj_sizes, INTERNAL_MEM_MS_TABLES);
	ms_calculate_block_obj_sizes (MS_BLOCK_OBJ_SIZE_FACTOR, block_obj_sizes);

	/*
	{
		int i;
		g_print ("block object sizes:\n");
		for (i = 0; i < num_block_obj_sizes; ++i)
			g_print ("%d\n", block_obj_sizes [i]);
	}
	*/

	for (i = 0; i < MS_BLOCK_TYPE_MAX; ++i)
		free_block_lists [i] = mono_sgen_alloc_internal_dynamic (sizeof (MSBlockInfo*) * num_block_obj_sizes, INTERNAL_MEM_MS_TABLES);

	for (i = 0; i < MS_NUM_FAST_BLOCK_OBJ_SIZE_INDEXES; ++i)
		fast_block_obj_size_indexes [i] = ms_find_block_obj_size_index (i * 8);
	for (i = 0; i < MS_NUM_FAST_BLOCK_OBJ_SIZE_INDEXES * 8; ++i)
		g_assert (MS_BLOCK_OBJ_SIZE_INDEX (i) == ms_find_block_obj_size_index (i));

	LOCK_INIT (ms_block_list_mutex);

	mono_counters_register ("# major blocks allocated", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_major_blocks_alloced);
	mono_counters_register ("# major blocks freed", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_major_blocks_freed);
}

/* only valid during minor collections */
static int old_num_major_sections;

static void
major_start_nursery_collection (void)
{
#ifdef MARKSWEEP_CONSISTENCY_CHECK
	consistency_check ();
#endif

	old_num_major_sections = num_major_sections;
}

static void
major_finish_nursery_collection (void)
{
	int sections_alloced;

#ifdef MARKSWEEP_CONSISTENCY_CHECK
	consistency_check ();
#endif

	sections_alloced = num_major_sections - old_num_major_sections;
	minor_collection_sections_alloced += sections_alloced;
}

static void
major_finish_major_collection (void)
{
	int section_reserve = minor_collection_allowance / MS_BLOCK_SIZE;

	/*
	 * FIXME: We don't free blocks on 32 bit platforms because it
	 * can lead to address space fragmentation, since we're
	 * allocating blocks in larger contingents.
	 */
	if (sizeof (mword) < 8)
		return;

	while (num_empty_blocks > section_reserve) {
		void *next = *(void**)empty_blocks;
		mono_sgen_free_os_memory (empty_blocks, MS_BLOCK_SIZE);
		empty_blocks = next;
		/*
		 * Needs not be atomic because this is running
		 * single-threaded.
		 */
		--num_empty_blocks;

		++stat_major_blocks_freed;
	}
}

static void
major_find_pin_queue_start_ends (GrayQueue *queue)
{
	MSBlockInfo *block;

	for (block = all_blocks; block; block = block->next) {
		find_optimized_pin_queue_area (block->block + MS_BLOCK_SKIP, block->block + MS_BLOCK_SIZE,
				&block->pin_queue_start, &block->pin_queue_end);
	}
}

static void
major_pin_objects (GrayQueue *queue)
{
	MSBlockInfo *block;

	for (block = all_blocks; block; block = block->next)
		mark_pinned_objects_in_block (block, queue);
}

static void
major_init_to_space (void)
{
}

static void
major_report_pinned_memory_usage (void)
{
	g_assert_not_reached ();
}

static gint64
major_get_used_size (void)
{
	gint64 size = 0;
	MSBlockInfo *block;

	for (block = all_blocks; block; block = block->next) {
		int count = MS_BLOCK_FREE / block->obj_size;
		void **iter;
		size += count * block->obj_size;
		for (iter = block->free_list; iter; iter = (void**)*iter)
			size -= block->obj_size;
	}

	return size;
}

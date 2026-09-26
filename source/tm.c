/*
    Jon Kuçi
    jon.kuci@epfl.ch
    EPFL, Masters in Computer Science, 2025
    CS-453 Project
    ---

	The algorithm presented bellow is based on TL2, originally introduced on:
    Dice, D., Shalev, O., and Shavit, N.
    "Transactional Locking II."
    Sun Microsystems Laboratories and Tel-Aviv University.
	https://people.csail.mit.edu/shanir/publications/Transactional_Locking.pdf

	Some of the ideas and pseudocode structures for the TL2 logic were inspired
    by Artem Khyzha's PhD thesis
	"Proving consistency of concurrent data structures and transactional
 	memory systems" from the Universidad Politecnica de Madrid.
	https://software.imdea.org/~gotsman/papers/khyzha-thesis.pdf

    For the hashtable, most of the code inspired by - Simple Hash Table in C
    https://benhoyt.com/writings/hash-table-in-c/


======================================================================================

	The memory is constructed as follows:

    Region:
    [[Reg1, Reg2, Reg3, ..., RegN] [Verlock1, Verlock2, Verlock3, ... VerlockN]]

    Segment:
    [[Reg1, Reg2, Reg3, ..., RegN] [Verlock1, Verlock2, Verlock3, ... VerlockN]]

    Verlock:
    [[ 31 bits: ---- version counter ---- ] [ 1 bit: lock ]]

    We use pointer tagging to know on which segment the pointer belongs to.
    Pointer - [ 16bit [ ----segment_position---- ] 48bit [ ---- actual_address--- ]]

    Returns a virtual address which is just the offset of: segment->start + offset = real_address

    For each thread we keep a thread version representing
    when that thread's current transaction started. When a segment is freed, we record the
    global clock version at that time. We only physically free the segment
    after all threads have moved past that version so no active transaction could
    possibly still have a pointer to it.

    The transaction object for Read only transactions is reused for each thread
    massively boosting performace.

=======================================================================================

Notes for the project:
    - It has been tested with valgrid on individual tests and grading and resulted with 0 leaks.
    - I put all the code in one file to be able to test it better and keep version files,
    I will clean the code after the semester and put it in github.

*/


#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <tm.h>
#include <time.h>
#include "macros.h"

#define READ_SET_INITIAL_CAPACITY 32
#define ALLOC_SET_INITIAL_CAPACITY 2
#define FREE_SET_INITIAL_CAPACITY 2
#define WRITE_SET_INITIAL_CAPACITY 32
#define MAX_SEGMENT_NUMBER 65536
#define MAX_THREADS 24

/*
    ===========================================================
        TYPE DEFINITIONS
    ===========================================================
*/

typedef struct segment_node {
    void* start;
    _Atomic int* verlock_base;
    uint16_t seg_id;
    size_t size;
    size_t alloc_size;
    void* mmap_base;
} segment_node_t;


typedef struct version_lock_register{
    _Atomic int version_lock;
}version_lock_register_t;

typedef struct freed_segment_node {
    _Atomic(segment_node_t*) segment;
    _Atomic uint64_t freed_at_version;
} freed_segment_node_t;

typedef struct freed_segment_set {
    freed_segment_node_t nodes[MAX_SEGMENT_NUMBER];
} freed_segment_set_t;


typedef struct {
    void* addr;
    _Atomic int* version_lock;
    size_t size;
} read_set_node_t;

typedef struct {
    void* addr;
    void* value_copy;
    _Atomic int* version_lock;
    size_t size;
} write_set_node_t;


typedef struct {
    read_set_node_t *nodes;
    size_t count;
    size_t capacity;
} read_set_t;

typedef struct {
    write_set_node_t* entries;
    size_t count;
    size_t capacity;
} write_hash_table_t;

typedef struct {
    void **segments;
    size_t count;
    size_t capacity;
} to_free_set_t;

typedef struct {
    segment_node_t** segments;
    size_t count;
    size_t capacity;
} alloced_set_t;


typedef struct transaction{
    bool is_ro;
    read_set_t read_set;
    write_hash_table_t write_hash;
	uint32_t thread_id;
	to_free_set_t to_free_set;
    alloced_set_t alloced_set;

    int read_version;
    int write_version;
}transaction_t;


typedef struct region{
    void* start;
    _Atomic int* verlock_base;
    _Atomic int global_clock;
    _Atomic(segment_node_t*) segment_map[MAX_SEGMENT_NUMBER];
    unsigned align_log2;
	size_t pointer_memcpy_parts;
    _Atomic uint64_t active_tx_versions[MAX_THREADS];
    transaction_t* ro_transactions[MAX_THREADS];
	_Atomic uint32_t next_thread_id;
    size_t size;
    size_t align;
    _Atomic uint16_t next_seg_id;
 	freed_segment_set_t freed_segment_set;
	void* mmap_base;
	pthread_mutex_t cleanup_lock;
    _Atomic bool freed_flag;
}region_t;
// ===========================================================


/*
    ===========================================================
        Helper function definitions
    ===========================================================
*/
bool read_set_add(read_set_t *set, void *addr, size_t size, _Atomic int* version_lock);
void unlock_version_lock_register(_Atomic int* version_lock);
bool lock_version_lock_register(_Atomic int* version_lock_register);
static inline bool get_real_pointer_and_version(region_t* region, const void* tagged_pointer, char** real_out, _Atomic int** lock_out);
static inline uintptr_t get_offset(const void* tagged_pointer);
static inline uint16_t get_segment_id(const void* tagged_pointer);
static inline void* make_tagged_pointer(uint16_t segment_id, uintptr_t offset);
static inline unsigned ilog2(size_t x);
static inline segment_node_t* get_segment_node(region_t* region, const void* tagged_pointer);
bool to_free_set_add(to_free_set_t* set, void* segment);
bool alloced_set_add(alloced_set_t* set, segment_node_t* segment);
write_set_node_t* ht_set(write_hash_table_t* table, void* addr);
static bool ht_expand(write_hash_table_t* table);
static write_set_node_t* ht_set_entry(write_set_node_t* entries, size_t capacity, void* addr, size_t* plength);
write_set_node_t* ht_get(write_hash_table_t* table, void* addr);
static inline uint64_t hash_ptr(void* p);
void ht_destroy(write_hash_table_t* table);
write_hash_table_t* ht_create(void);
static inline void pointer_memcpy(void* target, void* source, size_t parts);
static inline uint64_t get_min_active_version(region_t* region);
static void cleanup_freed_segments(region_t* region);


/**
 * This function allocates and initializes a TM region with a given size
 * and alignment. Initialized to 0 with mmap.
 *
 */
shared_t tm_create(size_t size, size_t align){
    region_t* region = (region_t*) malloc(sizeof(region_t));
    if (unlikely(!region)) {
        return invalid_shared;
    }
    memset(region, 0, sizeof(region_t));

    const size_t num_regs = size / align;
    const size_t total_size = size + num_regs * sizeof(_Atomic int);

   	size_t alloc_size = total_size + align;
	// mmap initializes memory with 0
    void* ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (unlikely(ptr == MAP_FAILED)) {
        free(region);
        return invalid_shared;
    }
	region->mmap_base = ptr;

    for (size_t i = 0; i < MAX_SEGMENT_NUMBER; i++) {
        atomic_init(&region->freed_segment_set.nodes[i].freed_at_version, UINT64_MAX);
    }

    atomic_init(&region->next_thread_id, 0);

    for (int i = 0; i < MAX_THREADS; i++) {
        atomic_init(&region->active_tx_versions[i], UINT64_MAX);

        region->ro_transactions[i] = malloc(sizeof(transaction_t));
        transaction_t* ro = region->ro_transactions[i];

        ro->is_ro = true;
        ro->thread_id = i;
        ro->read_version = 0;
        ro->write_version = 0;

    }


    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    region->start = (void*)aligned;

	pthread_mutex_init(&region->cleanup_lock, NULL);

    region->size = size;
    region->align = align;
    region->verlock_base = (_Atomic int*)((char*)region->start + size);

    // Stores log2(align) so we can replace division by fast bit shifts when indexing version locks
    region->align_log2 = ilog2(align);
	region->pointer_memcpy_parts = align >> 3;

    atomic_init(&region->next_seg_id, 1);
    atomic_init(&region->global_clock, 0);
    atomic_init(&region->freed_flag, false);
    return region;
}


/**
 * Get the logical start address of the transactional region.
 * The region's first address always starts with 0.
 * FFFF denotes the pointer tagging telling us that this pointer belongs to base region.
 */
void* tm_start(shared_t unused(shared)) {
	return (void*)0xFFFF000000000000ULL;
}
/** Return the region size in bytes. */
size_t tm_size(shared_t shared) {
	return ((region_t*) shared)->size;
}
/** Return the region alignment in bytes. */
size_t tm_align(shared_t shared) {
	return ((region_t*) shared)->align;
}

/**
 * Begin a new transaction.
 * Creates and initializes a transaction record, assigns a thread id
 *   to be able to track when its done for later segment cleaning.
 * captures the current global version for validation (TL2), and sets up
 * read and write tracking depending on the mode.
 *
 */
tx_t tm_begin(shared_t shared, bool is_ro){
    region_t* region = (region_t*) shared;

    // Keeps track for each thread which is active.
    static _Thread_local int thread_id = -1;
    if (unlikely(thread_id == -1)) {
        thread_id = atomic_fetch_add_explicit(&region->next_thread_id, 1, memory_order_relaxed);
    }

    transaction_t* transaction;

    if (is_ro) {
        // Use cached read-only transaction
        static _Thread_local transaction_t* cached_ro = NULL;
        if (unlikely(cached_ro == NULL)) {
            cached_ro = region->ro_transactions[thread_id];
        }
        transaction = cached_ro;
    } else {
        // Allocate RW
        transaction = malloc(sizeof(transaction_t));
        if (unlikely(!transaction)) {
            return invalid_tx;
        }

        transaction->is_ro = false;

        transaction->write_hash.entries = calloc(WRITE_SET_INITIAL_CAPACITY, sizeof(write_set_node_t));
        if (unlikely(!transaction->write_hash.entries)) {
            free(transaction);
            return invalid_tx;
        }
        transaction->write_hash.capacity = WRITE_SET_INITIAL_CAPACITY;
        transaction->write_hash.count = 0;

        // lazy allocation
        transaction->read_set.nodes = NULL;
        transaction->read_set.count = 0;
        transaction->read_set.capacity = 0;

        transaction->alloced_set.segments = NULL;
        transaction->alloced_set.count = 0;
        transaction->alloced_set.capacity = 0;

        transaction->to_free_set.segments = NULL;
        transaction->to_free_set.count = 0;
        transaction->to_free_set.capacity = 0;
    }

    transaction->thread_id = thread_id;
    transaction->read_version = atomic_load_explicit(&region->global_clock, memory_order_acquire);
    transaction->write_version = 0;

    atomic_store_explicit(&region->active_tx_versions[thread_id], transaction->read_version, memory_order_release);

    return (tx_t)transaction;
}



/**
* Transactional read.
* Aborts the transaction if a lock is held, a version changes during read,
* or if the target segment is no longer valid.
*/
bool tm_read(shared_t shared, tx_t tx, void const* source, size_t size, void* target){
    region_t* region = (region_t*) shared;
    transaction_t* transaction = (transaction_t*)(uintptr_t) tx;

    size_t align = region->align;
	size_t align_log2 = region->align_log2;
    char* target_ptr = (char*) target;

	size_t parts = region->pointer_memcpy_parts;
    char* real_address;
    _Atomic int* version_lock_base;

    if(!get_real_pointer_and_version(region, source, &real_address, &version_lock_base)){
        atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
        return false;
	}

    if(transaction->is_ro){
        //Read only code
        int rv = transaction->read_version;
        for(size_t i=0; i<size; i+=align){

            char* reg_val_address = real_address + i;
            _Atomic int* version_lock = version_lock_base + (i >> align_log2);
            char* target_address = target_ptr + i;

            int before = atomic_load_explicit(version_lock, memory_order_relaxed);
            int version = before >> 1;

            if ((before & 1) || (version > rv)){
                atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
				return false;
            }
			pointer_memcpy(target_address, reg_val_address, parts);

            int after = atomic_load_explicit(version_lock, memory_order_relaxed);
			if (unlikely(after != before)) {
                atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
				return false; // Lock acquired OR version changed
			}
        }
        return true;
    }

    // Write and read
    for (size_t i = 0; i < size; i += align){

        char* reg_val_address = real_address + i;
        char* target_address = target_ptr + i;

        //if in write set get from there
        write_set_node_t* entry = ht_get(&transaction->write_hash, reg_val_address);
        if (entry) {
           // memcpy(target_address, entry->value_copy, align);
			pointer_memcpy(target_address, entry->value_copy, parts);
            continue;
        }

        _Atomic int* version_lock = version_lock_base + (i >> align_log2);

        int before = atomic_load_explicit(version_lock, memory_order_relaxed);
        int version = before >> 1;

        if ((before & 1) || (version > transaction->read_version)) {
            goto abort;
        }
		// Pointer memcopy is faster than memcopy because in TL2 we have chunks
        pointer_memcpy(target_address, reg_val_address, parts);

        int after = atomic_load_explicit(version_lock, memory_order_relaxed);
		if (unlikely(after != before)){
    		goto abort; // Lock acquired OR version changed
		}
        if(unlikely(!read_set_add(&transaction->read_set, reg_val_address, align, version_lock))){
		    goto abort;
		}
    }
    return true;

    abort:
        if (transaction->alloced_set.segments) {
        for (size_t i = 0; i < transaction->alloced_set.count; i++) {
            segment_node_t* sn = transaction->alloced_set.segments[i];
            if (!sn) continue;
            atomic_store_explicit(&region->segment_map[sn->seg_id], NULL, memory_order_release);
            munmap(sn->mmap_base, sn->alloc_size);
            free(sn);
        }
         free(transaction->alloced_set.segments);
        }
        free(transaction->read_set.nodes);
        ht_destroy(&transaction->write_hash);
	    if(transaction->to_free_set.segments)free(transaction->to_free_set.segments);
        atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
        free(transaction);
        return false;
}

/**
 * Write operation for read write transactions.
 * Resolves the tagged target pointer, checks the write hash table,
 * then buffers each aligned chunk into the write set rather than
 * writing directly to shared memory. Actual writes happen at commit.
 */
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target) {
    region_t *region = (region_t *) shared;
    transaction_t *transaction = (transaction_t *) tx;
	size_t align_log2 = region->align_log2;
    size_t align = region->align;
    char* source_ptr = (char*) source;
	size_t parts = region->pointer_memcpy_parts;


    char* real_address;
    _Atomic int* version_lock_base;
    if(!get_real_pointer_and_version(region, target, &real_address, &version_lock_base)){
		goto abort;
	}

    for (size_t i = 0; i < size; i += align) {

        char* target_address = real_address + i;
        char* reg_val_address = source_ptr +i;
        _Atomic int* version_lock = version_lock_base + (i >> align_log2);

        write_set_node_t* entry = ht_set(&transaction->write_hash, target_address);
        if (!entry){
		 goto abort;
		}

         if (!entry->value_copy) {
            entry->value_copy = malloc(align);
            if (!entry->value_copy){
			    goto abort;
			}
            entry->version_lock = version_lock;
        }
		pointer_memcpy(entry->value_copy, reg_val_address, parts);
        entry->size = align;
    }
    return true;

    abort:
        if (transaction->alloced_set.segments) {
        for (size_t i = 0; i < transaction->alloced_set.count; i++) {
            segment_node_t* sn = transaction->alloced_set.segments[i];
            if (!sn) continue;
            atomic_store_explicit(&region->segment_map[sn->seg_id], NULL, memory_order_release);
            munmap(sn->mmap_base, sn->alloc_size);
            free(sn);
        }
         free(transaction->alloced_set.segments);
        }
        free(transaction->read_set.nodes);
        ht_destroy(&transaction->write_hash);
	    if(transaction->to_free_set.segments)free(transaction->to_free_set.segments);
        atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
        free(transaction);
        return false;
}




/*
  ===========================================================
   THE MAGIC - tm_end
  ===========================================================

 * Finalizes a transaction if it didn't abort before.
 * Read only transactions simply clear bookkeeping and return success.
 * Write transactions lock their write set, obtain a new version,
 * validate all earlier reads, apply all buffered writes,
 * release locks, schedule segment frees, and clean up all
 * per transaction allocations.
 */
bool tm_end(shared_t shared, tx_t tx) {
     region_t *region = (region_t*) shared;
     transaction_t* transaction = (transaction_t*) tx;

    if (transaction->is_ro){
       atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
       return true;
    }

    //Is Read - Write

    //1. Try to lock the write set
    for (size_t i = 0; i < transaction->write_hash.capacity; i++) {
        if (transaction->write_hash.entries[i].addr == NULL) continue;

        _Atomic int* version_lock = transaction->write_hash.entries[i].version_lock;

        if (!lock_version_lock_register(version_lock)) {
            // Failed to get that lock ; unlock everything we locked so far
            for (size_t j = 0; j < transaction->write_hash.capacity; j++) {
                if (transaction->write_hash.entries[j].addr == NULL) continue;
                if (j >= i) break;
                unlock_version_lock_register(transaction->write_hash.entries[j].version_lock);
            }
            goto abort;
        }
    }

    // 2. Get new write version (increment global clock)
    transaction->write_version = atomic_fetch_add_explicit(&region->global_clock, 1, memory_order_relaxed) + 1;

    // 3. Validate the read set (unless write_version == read_version + 1), also check if segment is gone
    if (transaction->write_version != transaction->read_version + 1) {
        for (size_t i = 0; i < transaction->read_set.count; i++) {
			read_set_node_t* read_node = &transaction->read_set.nodes[i];

            _Atomic int* version_lock = read_node->version_lock;

            int val = atomic_load(version_lock);
            int version = val >> 1; //Increment and make it 0

            if ((val & 1) || version > transaction->read_version) {
                for (size_t j = 0; j < transaction->write_hash.capacity; j++) {
                    if (transaction->write_hash.entries[j].addr == NULL) continue;
                    unlock_version_lock_register(transaction->write_hash.entries[j].version_lock);
                }
                goto abort;
            }
        }
    }

	size_t parts = region->pointer_memcpy_parts;

    // 4. Apply the writes and release locks (update version)
    for (size_t i = 0; i < transaction->write_hash.capacity; i++) {
        if (transaction->write_hash.entries[i].addr == NULL) continue;
        write_set_node_t* entry = &transaction->write_hash.entries[i];

        pointer_memcpy(entry->addr, entry->value_copy, parts);

        atomic_store(entry->version_lock, (transaction->write_version << 1));
    }


	// 5. Set up for freeing
	if (transaction->to_free_set.count > 0) {

		uint64_t freed_ver = atomic_load_explicit(&region->global_clock, memory_order_relaxed);
   	 	 for (size_t i = 0; i < transaction->to_free_set.count; i++) {
   	   		  void* target = transaction->to_free_set.segments[i];
   	     	  segment_node_t* sn = get_segment_node(region, target);
   	          if (!sn) continue;

   	   		  uint16_t seg_id = sn->seg_id;

    		 atomic_store_explicit(&region->segment_map[seg_id], NULL, memory_order_release); // Donnt delete it just removes from scope
   	 	     atomic_store_explicit(&region->freed_segment_set.nodes[seg_id].segment, sn, memory_order_release);

    		 atomic_store_explicit(&region->freed_segment_set.nodes[seg_id].freed_at_version,freed_ver, memory_order_release);
   	     }
	     atomic_store_explicit(&region->freed_flag, true, memory_order_relaxed);

	}

	// 6. Clean up things

    if(atomic_load_explicit(&region->freed_flag, memory_order_relaxed)){
	   if (pthread_mutex_trylock(&region->cleanup_lock) == 0) {
	        atomic_store_explicit(&region->freed_flag, false, memory_order_relaxed);
    	    cleanup_freed_segments(region);
   		    pthread_mutex_unlock(&region->cleanup_lock);
	    }
    }
    atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
      // 6. Cleanup
    free(transaction->read_set.nodes);
    if (transaction->alloced_set.segments) {
        free(transaction->alloced_set.segments);
    }
    ht_destroy(&transaction->write_hash);

    if(transaction->to_free_set.segments)free(transaction->to_free_set.segments);
    free(transaction);
    return true;


    abort:
        if (transaction->alloced_set.segments) {
            for (size_t i = 0; i < transaction->alloced_set.count; i++) {
                segment_node_t* sn = transaction->alloced_set.segments[i];
                if (!sn) continue;
                atomic_store_explicit(&region->segment_map[sn->seg_id], NULL, memory_order_release);
                munmap(sn->mmap_base, sn->alloc_size);
                free(sn);
            }
             free(transaction->alloced_set.segments);
        }
        free(transaction->read_set.nodes);
        ht_destroy(&transaction->write_hash);
    	if(transaction->to_free_set.segments)free(transaction->to_free_set.segments);
        atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
        free(transaction);
        return false;
}

/**
 * Allocates a new shared memory segment.
 * The segemnt is allocated but other transactions don't have a pointer to access it yet.
 * if later the transaction aborts, we store this into alloc_set so we can remove it.
 */
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target){
      region_t *region = (region_t *)shared;
      transaction_t* transaction = (transaction_t*) tx;

      const size_t align = region->align;
      const size_t num_regs = size / align;
      const size_t total_size= size + num_regs * sizeof(_Atomic int);

     segment_node_t *sn = malloc(sizeof(segment_node_t));
     if (unlikely(!sn)) {
         return nomem_alloc;
     }

    size_t alloc_size = total_size + align;
    void* ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (unlikely(ptr == MAP_FAILED)) {
        free(sn);
        return nomem_alloc;
    }

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    sn->start = (void*)aligned;


    sn->verlock_base = (_Atomic int*)((char*)sn->start + size);

    sn->size = size;
    sn->mmap_base = ptr;
	sn->alloc_size = alloc_size;
    sn->seg_id = atomic_fetch_add_explicit(&region->next_seg_id, 1, memory_order_acq_rel);
    atomic_store_explicit(&region->segment_map[sn->seg_id], sn, memory_order_release);

    if(unlikely(!alloced_set_add(&transaction->alloced_set, sn))){
		atomic_store_explicit(&region->segment_map[sn->seg_id], NULL,memory_order_release);
		munmap(sn->mmap_base, sn->alloc_size);
		free(sn);
        if (transaction->alloced_set.segments) {
            for (size_t i = 0; i < transaction->alloced_set.count; i++) {
                segment_node_t* sn = transaction->alloced_set.segments[i];
                if (!sn) continue;
                atomic_store_explicit(&region->segment_map[sn->seg_id], NULL, memory_order_release);
                munmap(sn->mmap_base, sn->alloc_size);
                free(sn);
            }
             free(transaction->alloced_set.segments);
        }
        free(transaction->read_set.nodes);
        ht_destroy(&transaction->write_hash);
	    if(transaction->to_free_set.segments)free(transaction->to_free_set.segments);
        atomic_store_explicit(&region->active_tx_versions[transaction->thread_id], UINT64_MAX, memory_order_release);
        free(transaction);
    return false;
		return abort_alloc;
	}

    *target = make_tagged_pointer(sn->seg_id, 0);
    return success_alloc;
}

/**
 * Marks a segment for deferred freeing.
 * Adds the tagged pointer to the transaction’s to free list so that
 * the segment will be reclaimed after commit once it is safe for
 * all active transactions.
*/
bool tm_free(shared_t unused(shared), tx_t tx, void *target){
    transaction_t* transaction = (transaction_t*) tx;
    if(!unlikely(to_free_set_add(&transaction->to_free_set, target))){
       return false;
    }
	return true;
}

/**
 * Destroys the entire transactional memory region.
 * Cleans up any pending freed segments, unmaps and frees all active
 * segments, unmaps the main region allocation and
 * frees the region structure itself.
 */
void tm_destroy(shared_t shared){
    region_t *region = (region_t *)shared;
    if (!region) return;
	pthread_mutex_destroy(&region->cleanup_lock);

    if (atomic_load_explicit(&region->freed_flag, memory_order_relaxed)) {
        cleanup_freed_segments(region);
    }

    uint16_t max_seg = atomic_load_explicit(&region->next_seg_id, memory_order_acquire);
    //  Cleanup any active segments still in segment_map
    for (uint16_t i = 0; i < max_seg; i++) {
        segment_node_t* sn = atomic_load_explicit(&region->segment_map[i], memory_order_acquire);
        if (sn) {
            munmap(sn->mmap_base, sn->alloc_size);
            free(sn);
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        if (region->ro_transactions[i]) {
            free(region->ro_transactions[i]);
        }

    }

    const size_t num_regs = region->size / region->align;
    const size_t total_size = region->size + num_regs * sizeof(_Atomic int);
    const size_t alloc_size = total_size + region->align;
    munmap(region->mmap_base, alloc_size);

    free(region);
}
// ===========================================================






/*
    ===========================================================
        Freeing Segments Functions
    ===========================================================
*/
/**
 * Finds the lowest active transaction version.
 * Used to decide when old freed segments can be safely removed.
 */
static inline uint64_t get_min_active_version(region_t* region) {
    uint64_t min = UINT64_MAX;
    for (size_t i = 0; i < MAX_THREADS; i++) {
        uint64_t ver = atomic_load_explicit(&region->active_tx_versions[i], memory_order_relaxed);
        if (ver < min) {
            min = ver;
        }
    }
    return min;
}

/**
 * Frees segments whose version is older than all active transactions.
 * Only segments no thread can still read from are actually released.
 */
static void cleanup_freed_segments(region_t* region) {
    uint64_t min_active = get_min_active_version(region);
    uint16_t max_seg = atomic_load_explicit(&region->next_seg_id, memory_order_relaxed);

    for (uint16_t seg_id = 0; seg_id < max_seg; seg_id++) {
        uint64_t freed_ver = atomic_load_explicit(&region->freed_segment_set.nodes[seg_id].freed_at_version, memory_order_relaxed);
        if (freed_ver == UINT64_MAX) continue;

        // Safe to free now. All transactions have started after the free.
        if (freed_ver < min_active) {
            segment_node_t* sn = atomic_load_explicit(&region->freed_segment_set.nodes[seg_id].segment, memory_order_relaxed);
        	if (sn) {

           		munmap(sn->mmap_base, sn->alloc_size);
           		free(sn);
				atomic_store_explicit(&region->freed_segment_set.nodes[seg_id].segment, NULL, memory_order_release);

            }
            //Dont check again -- like the wait-free consensus logic
            atomic_store_explicit( &region->freed_segment_set.nodes[seg_id].freed_at_version, UINT64_MAX,memory_order_release);
        }
    }
}
// ==========================================================








/*
    ===========================================================
        EXTRA FUNCTIONS
    ===========================================================
*/
/**
 * Adds a new entry to the read set.
 * Grows the buffer if needed.
 */

/**
 * Adds a new entry to the alloced set.
 * Grows the buffer if needed.
 */
bool read_set_add(read_set_t *set, void *addr, size_t size, _Atomic int* version_lock) {
    if (!set->nodes) {
        set->capacity = READ_SET_INITIAL_CAPACITY;
        set->nodes = malloc(set->capacity * sizeof(read_set_node_t));
        if (unlikely(!set->nodes)) return false;
    }else if (set->count == set->capacity) {
        set->capacity = (set->capacity * 3) / 2;
        void* new_nodes = realloc(set->nodes, set->capacity * sizeof(read_set_node_t));
        if (unlikely(!new_nodes)) return false;
        set->nodes = new_nodes;
    }
    set->nodes[set->count].addr = addr;
    set->nodes[set->count].size = size;
    set->nodes[set->count].version_lock = version_lock;
    set->count++;
    return true;
}

bool alloced_set_add(alloced_set_t* set, segment_node_t* segment){
    if (!set->segments) {
        set->capacity = ALLOC_SET_INITIAL_CAPACITY;
        set->segments = malloc(set->capacity * sizeof(segment_node_t*));
        if (unlikely(!set->segments)) return false;
    } else if (set->count == set->capacity) {
        set->capacity *= 2;
        void* new_segments= realloc(set->segments, set->capacity * sizeof(segment_node_t*));
        if (unlikely(!new_segments)) return false;
        set->segments = new_segments;
    }
    set->segments[set->count] = segment;
    set->count++;
    return true;
}
bool to_free_set_add(to_free_set_t* set, void* segment) {
    if (!set->segments) {
        set->capacity = FREE_SET_INITIAL_CAPACITY;
        set->segments = malloc(set->capacity * sizeof(void*));
        if (unlikely(!set->segments)) return false;
    } else if (set->count == set->capacity) {
        set->capacity *= 2;
        void* new_segments = realloc(set->segments, set->capacity * sizeof(void*));
        if (unlikely(!new_segments)) return false;
        set->segments = new_segments;
    }

    set->segments[set->count] = segment;
    set->count++;
    return true;
}

// ==================================================





/*
    ===========================================================
        LOCKS
    ===========================================================
*/

/**
 * Attempts to lock a version-lock register.
 * Fails if the lock bit is already set.
 * Succeeds by atomically setting the lock bit.
 */
bool lock_version_lock_register(_Atomic int* version_lock_register){
    int val = atomic_load(version_lock_register);

    if(val & 0x1){ // If the bit 0 is 1 (locked).
        return false;
    }
    // val OR 0x1 means leave as is just make the 0 bit 1
    return atomic_compare_exchange_strong(version_lock_register, &val, val | 0x1);
}
/**
 * Releases a version-lock by incrementing the counter.
 */
void unlock_version_lock_register(_Atomic int* version_lock) {
    // bump version counter and clear lock bit (LSB)
    atomic_fetch_add(version_lock, 1);
}





/*
===========================================================
        Pointer Tagging functions
===========================================================

*/
/**
 * Builds a tagged pointer combining a segment ID and a byte offset.
 * Upper 16 bits store the segment ID, lower 48 bits store the offset.
 */
static inline void* make_tagged_pointer(uint16_t segment_id, uintptr_t offset){
    return (void*)((((uintptr_t)segment_id) << 48) | (offset & 0x0000FFFFFFFFFFFFULL));
}

/**
 * Extracts the segment ID from a tagged pointer.
 * The ID is stored in the upper 16 bits.
 */
static inline uint16_t get_segment_id(const void* tagged_pointer){
    return ((uintptr_t) tagged_pointer >> 48) & 0xFFFF;
}

/**
 * Extracts the offset from a tagged pointer.
 * The offset is stored in the lower 48 bits.
 */
static inline uintptr_t get_offset(const void* tagged_pointer){
    return (uintptr_t)tagged_pointer & 0x0000FFFFFFFFFFFFULL;
}

/**
 * Decodes a tagged pointer into its real byte address and the associated
 * version lock pointer. Returns false if the tagged pointer references
 * an invalid or non existent segment.
 */
static inline bool get_real_pointer_and_version(region_t* region, const void* tagged_pointer, char** real_out, _Atomic int** lock_out){
    uint16_t seg_id = get_segment_id(tagged_pointer);
    uintptr_t offset = get_offset(tagged_pointer);
    size_t align_log2 = region->align_log2;
    //if the segment_id is max it is region, otherwise get from segment list
    if (seg_id == 0xFFFF) {
        *real_out = (char*)region->start + offset;
        size_t idx = offset >> align_log2;
        *lock_out = region->verlock_base + idx;
        return true;
    }
    segment_node_t* sn = atomic_load_explicit(&region->segment_map[seg_id], memory_order_acquire);
    if(unlikely(!sn)){
        *real_out = NULL;
        *lock_out = NULL;
        return false;
    }
    *real_out = (char*)sn->start + offset;
    size_t idx = offset >> align_log2;
    *lock_out = sn->verlock_base+idx;
    return true;
}

/**
 * Looks up the segment node corresponding to a tagged pointer.
 * The high 16 bits store the segment ID, which indexes segment_map.
 */
static inline segment_node_t* get_segment_node(region_t* region, const void* tagged_pointer){
    uint16_t id = ((uintptr_t)tagged_pointer >> 48) & 0xFFFF;
    return atomic_load_explicit(&region->segment_map[id], memory_order_acquire);
}

/* Efficient way to compute log2 for power-of-two alignment values using builtin trailing zero count */
static inline unsigned ilog2(size_t x){ return (unsigned)__builtin_ctzl(x); }

/* Copy aligned memory in chunks of 64 bits (8 byte)
 * It was faster and better than memcpy.
 */
static inline void pointer_memcpy(void* target, void* source, size_t parts){
    if (likely(parts == 1)){
		*(uint64_t*)target = *(uint64_t*)source;
	}else{
		for(size_t i =0; i < parts; i++){
			((uint64_t*)target)[i] = ((uint64_t*)source)[i];
		}
	}
}







/*
===========================================================
        Hash Functions

Most of the code inspired by - Simple Hash Table in C
https://benhoyt.com/writings/hash-table-in-c/

===========================================================
*/

/* Create a write set hash table
 * Allocates table struct and entry array
 * Uses zeroed entries for fast empty-slot detection
 */
write_hash_table_t* ht_create(void) {
    write_hash_table_t* table = malloc(sizeof(write_hash_table_t));
    if (table == NULL) {
        return NULL;
    }
    table->count = 0;
    table->capacity = WRITE_SET_INITIAL_CAPACITY;

    table->entries = calloc(table->capacity, sizeof(write_set_node_t));
    if (table->entries == NULL) {
        free(table);
        return NULL;
    }
    return table;
}

/* Destroy write set table
 * Frees all per-entry value_copy buffers and the entry array
 * Table struct itself is not freed (embedded in transaction)
 */
void ht_destroy(write_hash_table_t* table) {
    // Free all value_copy allocations
    for (size_t i = 0; i < table->capacity; i++) {
        if (table->entries[i].addr != NULL && table->entries[i].value_copy != NULL) {
            free(table->entries[i].value_copy);
        }
    }
    // Free the entries array
    free(table->entries);
    // DON'T free the table itself (it's embedded in transaction)
}

/* Pointer hashing (MurmurHash3 inspired)
 * Produces a 64 bit hash from a pointer value
 */
static inline uint64_t hash_ptr(void* p) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Lookup in write set
 * Uses linear probing until it finds either the key or an empty slot
 */
write_set_node_t* ht_get(write_hash_table_t* table, void* addr) {
    uint64_t hash = hash_ptr(addr);
    size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

    // Loop till we find an empty entry.
    while (table->entries[index].addr != NULL) {
        if (table->entries[index].addr == addr) {
            // Found it
            return &table->entries[index];
        }
        // Linear probing
        index++;
        if (index >= table->capacity) {
            index = 0;
        }
    }
    return NULL;
}

/* Insert or update entry in a raw entry array
 * No resizing here, used by both normal set and table expansion
 */
static write_set_node_t* ht_set_entry(write_set_node_t* entries, size_t capacity,
        void* addr, size_t* plength) {
    uint64_t hash = hash_ptr(addr);
    size_t index = (size_t)(hash & (uint64_t)(capacity - 1));

    // Loop till we find an empty entry.
    while (entries[index].addr != NULL) {
        if (entries[index].addr == addr) {
            // Found key (it already exists), return for update
            return &entries[index];
        }
        // Linear probing
        index++;
        if (index >= capacity) {
            index = 0;
        }
    }

    // Didn't find it, insert new entry
    if (plength != NULL) {
        (*plength)++;
    }
    entries[index].addr = addr;
    return &entries[index];
}

/* Expand table when load factor reaches seventy percent
 * Allocates new array and rehashes all existing entries into it
 */
static bool ht_expand(write_hash_table_t* table) {
    size_t new_capacity = table->capacity * 2;
    if (new_capacity < table->capacity) {
        return false;  // overflow
    }
    write_set_node_t* new_entries = calloc(new_capacity, sizeof(write_set_node_t));
    if (new_entries == NULL) {
        return false;
    }

    // Rehash all entries
    for (size_t i = 0; i < table->capacity; i++) {
        write_set_node_t entry = table->entries[i];
        if (entry.addr != NULL) {
            write_set_node_t* new_entry = ht_set_entry(new_entries, new_capacity, entry.addr, NULL);
            // Copy all fields
            *new_entry = entry;
        }
    }

    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
    return true;
}

/* Public insert into write set
 * Expands when needed, then inserts via ht_set_entry
*/
write_set_node_t* ht_set(write_hash_table_t* table, void* addr) {
    // Expand at 70% load factor
    if (table->count >= (table->capacity * 7) / 10) {
        if (!ht_expand(table)) {
            return NULL;
        }
    }

    // Set entry and update length
    return ht_set_entry(table->entries, table->capacity, addr, &table->count);
}



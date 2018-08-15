#ifndef QF_H
#define QF_H

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

	/* Can be 
		 0 (choose size at run-time), 
		 8, 16, 32, or 64 (for optimized versions),
		 or other integer <= 56 (for compile-time-optimized bit-shifting-based versions)
		 */
#define BITS_PER_SLOT 0

#define BITMASK(nbits) ((nbits) == 64 ? 0xffffffffffffffff : (1ULL << (nbits)) \
												- 1ULL)

	struct __attribute__ ((__packed__)) qfblock;
	typedef struct qfblock qfblock;

	enum lock {
		LOCK_NO_SPIN,
		LOCK_AND_SPIN,
		NO_LOCK
	};

	typedef struct {
		uint64_t total_time_single;
		uint64_t total_time_spinning;
		uint64_t locks_taken;
		uint64_t locks_acquired_single_attempt;
	} wait_time_data;

	typedef struct quotient_filter_mem {
		int fd;
		volatile int metadata_lock;
		volatile int *locks;
		wait_time_data *wait_times;
	} quotient_filter_mem;

	typedef quotient_filter_mem qfmem;

	typedef struct quotient_filter_metadata {
		uint64_t size;
		uint32_t seed;
		uint64_t nslots;
		uint64_t xnslots;
		uint64_t key_bits;
		uint64_t value_bits;
		uint64_t key_remainder_bits;
		uint64_t bits_per_slot;
		__uint128_t range;
		uint64_t nblocks;
		uint64_t nelts;
		uint64_t ndistinct_elts;
		uint64_t noccupied_slots;
		uint64_t num_locks;
	} quotient_filter_metadata;

	typedef quotient_filter_metadata qfmetadata;

	typedef struct quotient_filter {
		qfmem *mem;
		qfmetadata *metadata;
		qfblock *blocks;
	} quotient_filter;

	typedef quotient_filter QF;

	typedef struct {
		uint64_t start_index;
		uint16_t length;
	} cluster_data;

	typedef struct quotient_filter_iterator {
		const QF *qf;
		struct {
			uint64_t nslots;
			uint64_t xnslots;
			uint64_t bits_per_slot;
			uint64_t sizeof_qfblock_SLOTS_PER_BLOCK_times_bits_per_slot_div_8;
			uint64_t value_bits;
			uint64_t key_remainder_bits;
			uint64_t nblocks;
		} cache;
		uint64_t run;
		uint64_t current;
		uint64_t cur_start_index;
		uint16_t cur_length;
		uint32_t num_clusters;
		cluster_data *c_info;
	} quotient_filter_iterator;

	typedef quotient_filter_iterator QFi;

	/* Forward declaration for the macro. */
	void qf_dump_metadata(const QF *qf);

#define DEBUG_CQF(fmt, ...) \
	do { if (PRINT_DEBUG) fprintf(stderr, fmt, __VA_ARGS__); } while (0)

#define DEBUG_DUMP(qf) \
	do { if (PRINT_DEBUG) qf_dump_metadata(qf); } while (0)

	void qf_init(QF *qf, uint64_t nslots, uint64_t key_bits, uint64_t
							 value_bits, bool mem, const char *path, uint32_t seed);

	void qf_reset(QF *qf);

	void qf_destroy(QF *qf, bool mem);

	void qf_copy(QF *dest, QF *src);

	/* Increment the counter for this key/value pair by count. */
	bool qf_insert(QF *qf, uint64_t key, uint64_t value, uint64_t count,
								 enum lock flag);

	/* Remove count instances of this key/value combination. */
	void qf_remove(QF *qf, uint64_t key, uint64_t value, uint64_t count, enum
								 lock flag);

	/* Remove all instances of this key/value pair. */
	void qf_delete_key_value(QF *qf, uint64_t key, uint64_t value);

	/* Remove all instances of this key. */
	void qf_delete_key(QF *qf, uint64_t key);

	/* Replace the association (key, oldvalue, count) with the association
		 (key, newvalue, count). If there is already an association (key,
		 newvalue, count'), then the two associations will be merged and
		 their counters will be summed, resulting in association (key,
		 newvalue, count' + count). */
	void qf_replace(QF *qf, uint64_t key, uint64_t oldvalue, uint64_t newvalue);

	/* Lookup the value associated with key.  Returns the count of that
		 key/value pair in the QF.  If it returns 0, then, the key is not
		 present in the QF. Only returns the first value associated with key
		 in the QF.  If you want to see others, use an iterator. */
	uint64_t qf_query(const QF *qf, uint64_t key, uint64_t *value);

	/* Return the number of times key has been inserted, with any value,
		 into qf. */
	uint64_t qf_count_key(const QF *qf, uint64_t key);

	/* Return the number of times key has been inserted, with the given
		 value, into qf. */
	uint64_t qf_count_key_value(const QF *qf, uint64_t key, uint64_t value);

	uint64_t qf_key_value_index(const QF *qf, uint64_t key, uint64_t value, uint64_t* idx);
	/* Initialize an iterator */
	bool qf_iterator(const QF *qf, QFi *qfi, uint64_t position);

	/* Initialize an iterator and position it at the smallest index containing a
	 * hash value greater than or equal to "hash". */
	bool qf_iterator_hash(const QF *qf, QFi *qfi, uint64_t hash);

	/* Returns 0 if the iterator is still valid (i.e. has not reached the
		 end of the QF. */
	int qfi_get(const QFi *qfi, uint64_t *key, uint64_t *value, uint64_t *count);

	/* Advance to next entry.  Returns whether or not another entry is
		 found.  */
	int qfi_next(QFi *qfi);
	int qfi_nextx(QFi *qfi, uint64_t* read_offset);

	/* Check to see if the if the end of the QF */
	int qfi_end(const QFi *qfi); 

	/* For debugging */
	void qf_dump(const QF *);

	/* write data structure of to the disk */
	void qf_serialize(const QF *qf, const char *filename);

	/* read data structure off the disk */
	void qf_deserialize(QF *qf, const char *filename);

	/* mmap the QF from disk. */
	void qf_read(QF *qf, const char *path);

	/* merge two QFs into the third one. */
	void qf_merge(QF *qfa, QF *qfb, QF *qfc, enum lock flag);

	/* merge multiple QFs into the final QF one. */
	void qf_multi_merge(QF *qf_arr[], int nqf, QF *qfr, enum lock flag);

	/* find cosine similarity between two QFs. */
	uint64_t qf_inner_product(QF *qfa, QF *qfb);

	/* magnitude of a QF. */
	uint64_t qf_magnitude(QF *qf);

	/* use madvice to drop pages corresponding to blocks from start_idx to
	 * last_slot_idx. */
	void qf_drop_pages(const QF *qf, uint64_t start_idx, uint64_t end_idx);

	/* return the addr of the slot in qfblock */
	const unsigned char *qf_get_addr(const QF *qf, uint64_t idx);

#ifdef __cplusplus
}
#endif

#endif /* QF_H */

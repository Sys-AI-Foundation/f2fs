#ifndef F2FS_IFS_H
#define F2FS_IFS_H

#include <linux/fs.h>
#include <linux/bug.h>
#include <linux/f2fs_fs.h>
#include <linux/mm.h>
#include <linux/iomap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h> // For atomic_t and bitops
#include "f2fs.h"
#define F2FS_IFS_MAGIC 0xf2f5 
#define F2FS_IFS_PRIVATE_LONGS 2
/*
 * F2FS structure for folio private data, mimicking iomap_folio_state layout.
 * F2FS private flags/data are stored in extra space allocated at the end
 */
struct f2fs_iomap_folio_state {
	spinlock_t state_lock;
	unsigned int read_bytes_pending;
	atomic_t write_bytes_pending; 
	/*
	 * Flexible array member.
	 * Holds [0...iomap_longs-1] for iomap uptodate/dirty bits.
	 * Holds [iomap_longs] for F2FS private flags/data (unsigned long).
	 * Holds [iomap_longs+1] for dirty_bytes_pending
	 */
	unsigned long state[];
};
static inline bool f2fs_ifs_block_is_uptodate(struct f2fs_iomap_folio_state *ifs,
		unsigned int block)
{
	return test_bit(block, ifs->state);
}
static inline size_t f2fs_ifs_iomap_longs(struct folio *folio)
{
	struct inode* inode=folio->mapping->host;
	WARN_ON_ONCE(inode==NULL);
	unsigned int nr_blocks = i_blocks_per_folio(inode, folio);
	return BITS_TO_LONGS(2 * nr_blocks);
}
static inline size_t f2fs_ifs_total_longs(struct folio *folio)
{
	return f2fs_ifs_iomap_longs(folio) + F2FS_IFS_PRIVATE_LONGS;
}

static inline unsigned long *
f2fs_ifs_private_flags_ptr(struct f2fs_iomap_folio_state *fifs, struct folio *folio)
{
	return &fifs->state[f2fs_ifs_iomap_longs(folio)];
}
static inline atomic_t *
f2fs_ifs_cc_pending_bytes_ptr(struct f2fs_iomap_folio_state *fifs, struct folio *folio)
{
	// Treat the second private long as an atomic_t
	return (atomic_t *)&fifs->state[f2fs_ifs_iomap_longs(folio) + 1];
}

/*Have to set parameter ifs's type to void*
and have to interprest ifs as f2fs_ifs to acess it's fields because
we cannot see iomap_folio_state definition*/
static void ifs_to_f2fs_ifs(void *ifs, struct f2fs_iomap_folio_state *fifs, struct folio *folio)
{
	struct f2fs_iomap_folio_state *src_ifs = (struct f2fs_iomap_folio_state *)ifs;
	size_t iomap_longs = f2fs_ifs_iomap_longs(folio);
	fifs->read_bytes_pending = READ_ONCE(src_ifs->read_bytes_pending);
	atomic_set(&fifs->write_bytes_pending, atomic_read(&src_ifs->write_bytes_pending));
	memcpy(fifs->state, src_ifs->state, iomap_longs * sizeof(unsigned long));
}
struct f2fs_iomap_folio_state *f2fs_ifs_alloc(struct folio *folio, gfp_t gfp);
void f2fs_ifs_free(struct folio *folio);
struct f2fs_iomap_folio_state *f2fs_folio_get_private(struct folio *folio);
inline unsigned long f2fs_get_folio_private_data(struct folio *folio);
inline int f2fs_set_folio_private_data(struct folio *folio,unsigned long data);
inline void f2fs_clear_folio_private_data(struct folio *folio);
inline void f2fs_clear_folio_private_all(struct folio *folio);
/*0-order and fully dirty folio has no fifs
they store private flag directly in their folio->private field
as original f2fs page private behaviour*/
unsigned f2fs_iomap_find_dirty_range(struct folio *folio, u64 *range_start,u64 range_end);							
static inline bool is_f2fs_ifs(struct folio *folio)
{
    if (!folio_test_private(folio))
        return false;
        
    // 检查是否为标志位使用
    if (test_bit(PAGE_PRIVATE_NOT_POINTER, (unsigned long *)&folio->private))
        return false;
        
    struct f2fs_iomap_folio_state *fifs;
    fifs = (struct f2fs_iomap_folio_state *)folio->private;
    if (!fifs)
        return false;
    if (READ_ONCE(fifs->read_bytes_pending) == F2FS_IFS_MAGIC) {
        return true;
    }
    return false;
}
#define F2FS_FOLIO_PRIVATE_GET_FUNC(name, flagname)                                \
	static inline bool f2fs_folio_private_##name(struct folio *folio)          \
	{                                                                          \
		/* For order-0 folios, use folio->private directly */              \
		if (folio_order(folio) == 0) {                                     \
			return folio_test_private(folio) &&                        \
				test_bit(PAGE_PRIVATE_NOT_POINTER,                 \
					(unsigned long *)&folio->private) &&      \
				test_bit(PAGE_PRIVATE_##flagname,                  \
					(unsigned long *)&folio->private);        \
		}                                                                   \
                                                                                   \
		/* For higher-order folios, use iomap folio state */               \
		struct f2fs_iomap_folio_state *fifs =                              \
			(struct f2fs_iomap_folio_state *)folio->private;           \
		unsigned long *private_p;                                          \
		if (unlikely(!fifs || !folio->mapping))                            \
			return false;                                               \
		/* Check magic number before accessing private data */             \
		if (READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC)         \
			return false;                                               \
		private_p = f2fs_ifs_private_flags_ptr(fifs, folio);               \
		if (!private_p)                                                    \
			return false;                                               \
		/* Test bits directly on the 'private' slot */                     \
		return test_bit(PAGE_PRIVATE_##flagname, private_p);               \
	}

#define F2FS_FOLIO_PRIVATE_SET_FUNC(name, flagname)                               \
	static inline int f2fs_set_folio_private_##name(struct folio *folio)      \
	{                                                                         \
		/* For order-0 folios, use folio->private directly */             \
		if (folio_order(folio) == 0) {                                    \
			if (!folio_test_private(folio))                           \
				folio_attach_private(folio, (void *)0);           \
			set_bit(PAGE_PRIVATE_NOT_POINTER,                         \
				(unsigned long *)&folio->private);               \
			set_bit(PAGE_PRIVATE_##flagname,                          \
				(unsigned long *)&folio->private);               \
			return 0;                                                 \
		}                                                                 \
		/* For higher-order folios, use iomap folio state */             \
		if (unlikely(!folio->mapping))                                   \
			return -ENOENT;                                           \
		struct f2fs_iomap_folio_state *fifs =                            \
			f2fs_ifs_alloc(folio, GFP_NOFS);                         \
		if(unlikely(!fifs))                                              \
			return -ENOMEM;                                           \
		unsigned long *private_p;                                        \
		WRITE_ONCE(fifs->read_bytes_pending, F2FS_IFS_MAGIC);            \
		private_p = f2fs_ifs_private_flags_ptr(fifs, folio);             \
		if (!private_p)                                                  \
			return -EINVAL;                                           \
		/* Set the bit atomically */                                     \
		set_bit(PAGE_PRIVATE_##flagname, private_p);                     \
		/* Ensure NOT_POINTER bit is also set if any F2FS flag is set */ \
		if (PAGE_PRIVATE_##flagname != PAGE_PRIVATE_NOT_POINTER)         \
			set_bit(PAGE_PRIVATE_NOT_POINTER, private_p);            \
		return 0;                                                        \
	}

#define F2FS_FOLIO_PRIVATE_CLEAR_FUNC(name, flagname)                         \
	static inline void f2fs_clear_folio_private_##name(                   \
		struct folio *folio)                                          \
	{                                                                     \
		/* For order-0 folios, use folio->private directly */         \
		if (folio_order(folio) == 0) {                               \
			if (folio_test_private(folio) &&                      \
			    test_bit(PAGE_PRIVATE_NOT_POINTER,                \
				     (unsigned long *)&folio->private)) {     \
				clear_bit(PAGE_PRIVATE_##flagname,            \
					  (unsigned long *)&folio->private);  \
				if (folio->private ==                         \
				    BIT(PAGE_PRIVATE_NOT_POINTER))            \
					folio_detach_private(folio);          \
			}                                                     \
			return;                                               \
		}                                                             \
                                                                              \
		/* For higher-order folios, use iomap folio state */         \
		struct f2fs_iomap_folio_state *fifs =                         \
			(struct f2fs_iomap_folio_state *)folio->private;      \
		unsigned long *private_p;                                     \
		if (unlikely(!fifs || !folio->mapping))                       \
			return;                                               \
		/* Check magic number before clearing */                      \
		if (READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC)    \
			return; /* Not ours or state unclear */               \
		private_p = f2fs_ifs_private_flags_ptr(fifs, folio);          \
		if (!private_p)                                               \
			return;                                               \
		clear_bit(PAGE_PRIVATE_##flagname, private_p);                \
	}

// Generate the accessor functions using the macros
F2FS_FOLIO_PRIVATE_GET_FUNC(nonpointer, NOT_POINTER);
F2FS_FOLIO_PRIVATE_GET_FUNC(inline, INLINE_INODE);
F2FS_FOLIO_PRIVATE_GET_FUNC(gcing, ONGOING_MIGRATION);
F2FS_FOLIO_PRIVATE_GET_FUNC(atomic, ATOMIC_WRITE);
F2FS_FOLIO_PRIVATE_GET_FUNC(reference, REF_RESOURCE);
F2FS_FOLIO_PRIVATE_GET_FUNC(deferred_unlock, DEFERRED_UNLOCK);

F2FS_FOLIO_PRIVATE_SET_FUNC(reference, REF_RESOURCE);
F2FS_FOLIO_PRIVATE_SET_FUNC(inline, INLINE_INODE);
F2FS_FOLIO_PRIVATE_SET_FUNC(gcing, ONGOING_MIGRATION);
F2FS_FOLIO_PRIVATE_SET_FUNC(atomic, ATOMIC_WRITE);
F2FS_FOLIO_PRIVATE_SET_FUNC(deferred_unlock, DEFERRED_UNLOCK);

F2FS_FOLIO_PRIVATE_CLEAR_FUNC(reference, REF_RESOURCE);
F2FS_FOLIO_PRIVATE_CLEAR_FUNC(inline, INLINE_INODE);
F2FS_FOLIO_PRIVATE_CLEAR_FUNC(gcing, ONGOING_MIGRATION);
F2FS_FOLIO_PRIVATE_CLEAR_FUNC(atomic, ATOMIC_WRITE);
F2FS_FOLIO_PRIVATE_CLEAR_FUNC(deferred_unlock, DEFERRED_UNLOCK);
// --- Data access functions ---
#endif /* F2FS_IFS_H */


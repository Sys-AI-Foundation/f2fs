//f2fs_ifs.c
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/mm.h>
#include <linux/iomap.h>
#include <linux/slab.h>
#include <linux/sched.h> // For cond_resched()
#include "f2fs.h"
#include "f2fs_ifs.h" 
struct f2fs_iomap_folio_state *f2fs_ifs_alloc(struct folio *folio, gfp_t gfp)
{
	
	if (folio_order(folio) == 0) {
		WARN_ON_ONCE(1); 
		return NULL;
	}
	struct f2fs_iomap_folio_state *fifs;
	void *old_private;
	struct inode *inode = folio->mapping->host; 
	size_t iomap_longs;
	size_t total_longs;
	size_t alloc_size;

	WARN_ON_ONCE(!inode); // Should have an inode

	old_private = folio_get_private(folio);

	if (old_private) {
		// Check if it's already our type using the magic number directly
		if (READ_ONCE(((struct f2fs_iomap_folio_state *)old_private)->read_bytes_pending) == F2FS_IFS_MAGIC) {
			return (struct f2fs_iomap_folio_state *)old_private; // Already ours
		} else {
			// Non-NULL, not ours -> Allocate, Copy, Replace path
			total_longs = f2fs_ifs_total_longs(folio);
			alloc_size = sizeof(*fifs) + total_longs * sizeof(unsigned long);

			fifs = kmalloc(alloc_size, gfp); 
			if (!fifs)
				return NULL;

			spin_lock_init(&fifs->state_lock);
			*f2fs_ifs_private_flags_ptr(fifs, folio) = 0; 
			// Copy data from the presumed iomap_folio_state (old_private)
			ifs_to_f2fs_ifs(old_private, fifs, folio);
			WRITE_ONCE(fifs->read_bytes_pending, F2FS_IFS_MAGIC);
            atomic_set(f2fs_ifs_cc_pending_bytes_ptr(fifs, folio), 0);
			folio_change_private(folio, fifs);
			kfree(old_private); 
			return fifs;
		}
	} else {
		iomap_longs = f2fs_ifs_iomap_longs(folio);
		total_longs = iomap_longs + 1;
		alloc_size = sizeof(*fifs) + total_longs * sizeof(unsigned long);

		fifs = kzalloc(alloc_size, gfp);
		if (!fifs)
			return NULL;

		spin_lock_init(&fifs->state_lock);

		unsigned int nr_blocks = i_blocks_per_folio(inode, folio);
		if (nr_blocks > 0) { // Check nr_blocks directly
			if (folio_test_uptodate(folio))
				bitmap_set(fifs->state, 0, nr_blocks);
			if (folio_test_dirty(folio))
				bitmap_set(fifs->state, nr_blocks, nr_blocks);
		}

		WRITE_ONCE(fifs->read_bytes_pending, F2FS_IFS_MAGIC);
        atomic_set(&fifs->write_bytes_pending, 0); 
        atomic_set(f2fs_ifs_cc_pending_bytes_ptr(fifs, folio), 0);
		folio_attach_private(folio, fifs);
		return fifs;
	}
}
void f2fs_ifs_free(struct folio *folio)
{
	// Sanity check: order-0 folios should not use f2fs_ifs
	if (folio_order(folio) == 0) {
		// Just detach private if it exists, but warn about this situation
		if (folio_test_private(folio)) {
			WARN_ON_ONCE(is_f2fs_ifs(folio)); // Should never happen
			folio_detach_private(folio);
		}
		return;
	}
	struct f2fs_iomap_folio_state*fifs=folio_detach_private(folio);
	if (!fifs)
		return; 
	if(is_f2fs_ifs(folio))
	{
		WARN_ON_ONCE(READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC);
		WARN_ON_ONCE(atomic_read(&fifs->write_bytes_pending));
	}
	else
	{
		WARN_ON_ONCE(READ_ONCE(fifs->read_bytes_pending) != 0);
		WARN_ON_ONCE(atomic_read(&fifs->write_bytes_pending));
	}
	
	kfree(fifs);
}
struct f2fs_iomap_folio_state *f2fs_folio_get_private(struct folio *folio)
{
    if (!folio_test_private(folio))
        return NULL;
        
    // 检查是否为标志位使用
    if (test_bit(PAGE_PRIVATE_NOT_POINTER, (unsigned long *)&folio->private))
        return NULL;
        
    void *private_data = folio->private;
    if (!private_data)
        return NULL;
        
    // 安全地检查 magic
    struct f2fs_iomap_folio_state *fifs = private_data;
    if (READ_ONCE(fifs->read_bytes_pending) == F2FS_IFS_MAGIC)
        return fifs;
    
    return NULL;
}
unsigned f2fs_iomap_find_dirty_range(struct folio *folio, u64 *range_start,
		u64 range_end)
{
	struct inode* inode=folio->mapping->host;
	
	if(folio_order(folio) == 0) 
	 	return range_end-*range_start;
	if(f2fs_compressed_file(inode))
	{	
		/*clamp range end to a cluster's size*/
        
		range_end=min(range_end,(cluster_i_end_idx(inode,*range_start>>PAGE_SHIFT)+1) << PAGE_SHIFT);
	}
	return iomap_find_dirty_range(folio, range_start, range_end);
}
inline unsigned long f2fs_get_folio_private_data(struct folio *folio)
{
	/* For order-0 folios, use folio->private directly */
	if (folio_order(folio) == 0) {
		if (!folio_test_private(folio))
			return 0;
		unsigned long data = (unsigned long)folio->private;
		if (!test_bit(PAGE_PRIVATE_NOT_POINTER, &data))
			return 0;
		return data >> PAGE_PRIVATE_MAX;
	}

	/* For higher-order folios, use iomap folio state */
	struct f2fs_iomap_folio_state *fifs = 
		(struct f2fs_iomap_folio_state *)folio->private;
	unsigned long *private_p;
	unsigned long data_val;

	if (!fifs || !folio->mapping)
		return 0;
	if (READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC)
		return 0;

	private_p = f2fs_ifs_private_flags_ptr(fifs, folio);
	if (!private_p)
		return 0;

	data_val = READ_ONCE(*private_p); // Read atomically

	if (!test_bit(PAGE_PRIVATE_NOT_POINTER, &data_val))
		return 0; // Return 0 if NOT_POINTER isn't set

	return data_val >> PAGE_PRIVATE_MAX;
}

inline int f2fs_set_folio_private_data(struct folio *folio, unsigned long data)
{
	/* For order-0 folios, use folio->private directly */
	if (folio_order(folio) == 0) {
		if (!folio_test_private(folio))
			folio_attach_private(folio, (void *)0);
		
		unsigned long private_val = (unsigned long)folio->private;
		// Clear data bits and preserve flag bits
		private_val &= GENMASK(PAGE_PRIVATE_MAX - 1, 0);
		// Set new data bits
		private_val |= (data << PAGE_PRIVATE_MAX);
		// Ensure NOT_POINTER is set
		__set_bit(PAGE_PRIVATE_NOT_POINTER, &private_val);
		
		folio->private = (void *)private_val;
		return 0;
	}

	/* For higher-order folios, use iomap folio state */
	if (unlikely(!folio->mapping))
		return -ENOENT;
	
	struct f2fs_iomap_folio_state *fifs = f2fs_ifs_alloc(folio, GFP_NOFS);
	if (unlikely(!fifs))
		return -ENOMEM;
	
	unsigned long *private_p;
	unsigned long old_val, new_val;
	
	private_p = f2fs_ifs_private_flags_ptr(fifs, folio);
	if (!private_p)
		return -EINVAL;

	// Atomically set the data part and the NOT_POINTER bit using cmpxchg loop
	do {
		old_val = READ_ONCE(*private_p);
		new_val = old_val;
		// Clear old data bits (bits >= PAGE_PRIVATE_MAX)
		new_val &= GENMASK(PAGE_PRIVATE_MAX - 1, 0);
		// Set new data bits
		new_val |= (data << PAGE_PRIVATE_MAX);
		// Ensure NOT_POINTER is set
		__set_bit(PAGE_PRIVATE_NOT_POINTER, &new_val);
	} while (cmpxchg(private_p, old_val, new_val) != old_val);

	return 0;
}

inline void f2fs_clear_folio_private_data(struct folio *folio)
{
	/* For order-0 folios, use folio->private directly */
	if (folio_order(folio) == 0) {
		if (!folio_test_private(folio))
			return;
			
		unsigned long private_val = (unsigned long)folio->private;
		if (!test_bit(PAGE_PRIVATE_NOT_POINTER, &private_val))
			return;
			
		// If no data bits set, nothing to do
		if ((private_val >> PAGE_PRIVATE_MAX) == 0)
			return;
			
		// Clear data bits, preserve flag bits
		private_val &= GENMASK(PAGE_PRIVATE_MAX - 1, 0);
		folio->private = (void *)private_val;
		
		// If only NOT_POINTER flag is left, detach private
		if (private_val == BIT(PAGE_PRIVATE_NOT_POINTER))
			folio_detach_private(folio);
		return;
	}

	/* For higher-order folios, use iomap folio state */
	struct f2fs_iomap_folio_state *fifs = 
		(struct f2fs_iomap_folio_state *)folio->private;
	unsigned long *private_p;
	unsigned long old_val, new_val;

	if (!fifs || !folio->mapping)
		return;
	if (READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC)
		return;

	private_p = f2fs_ifs_private_flags_ptr(fifs, folio);
	if (!private_p)
		return;

	// Atomically clear the data part, leave flags untouched
	do {
		old_val = READ_ONCE(*private_p);
		// If already no data bits set, nothing to do
		if ((old_val >> PAGE_PRIVATE_MAX) == 0)
			break;
		new_val = old_val;
		// Clear data bits
		new_val &= GENMASK(PAGE_PRIVATE_MAX - 1, 0);
	} while (cmpxchg(private_p, old_val, new_val) != old_val);
}

inline void f2fs_clear_folio_private_all(struct folio *folio)
{
	/* First try direct folio->private access */
	if (folio_test_private(folio)) {
		if (folio_order(folio) == 0 || 
		    !is_f2fs_ifs(folio)) {
			folio_detach_private(folio);
			return;
		}
	}

	/* For higher-order folios with f2fs_ifs */
	struct f2fs_iomap_folio_state *fifs = 
		(struct f2fs_iomap_folio_state *)folio->private;
	unsigned long *private_p;

	if (unlikely(!fifs || !folio->mapping))
		return;
	if (READ_ONCE(fifs->read_bytes_pending) != F2FS_IFS_MAGIC)
		return;

	private_p = f2fs_ifs_private_flags_ptr(fifs, folio);
	if (!private_p)
		return;
		
	// Clear all private flags/data
	*private_p = 0;
}

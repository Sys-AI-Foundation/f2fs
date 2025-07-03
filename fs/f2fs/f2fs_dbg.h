#ifndef F2FS_DBG_H
#define F2FS_DBG_H

#include <linux/fs.h>
#include <linux/bug.h>
#include <linux/f2fs_fs.h>
#include <linux/mm.h>
#include <linux/iomap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h> // For atomic_t and bitops
#include <linux/delay.h>
#include "f2fs.h"
#include "f2fs_ifs.h"

#define FUNC(func, ...) \
    do { \
        printk(KERN_EMERG "In %s\n", __func__); \
        func(__VA_ARGS__); \
    } while (0)

void f2fs_is_folio_writeback(struct folio *folio)
{
    if (folio_test_writeback(folio))
        f2fs_err(F2FS_I_SB(folio->mapping->host),
            "folio %p,index %d,order %d host inode ino %d is already in writeback state", folio);
}
void f2fs_check_inode_folios_writeback(struct inode *inode)
{
    struct address_space *mapping = inode->i_mapping;
    struct folio *folio;
    pgoff_t start = 0;
    pgoff_t end = -1;
    struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
    
    XA_STATE(xas, &mapping->i_pages, start);
    
    rcu_read_lock();
    xas_for_each(&xas, folio, end) {
        if (!xa_is_value(folio)) {
            if (folio_test_writeback(folio)) {
                f2fs_err(sbi, 
                    "Error:inode(%lu): folio %p, index %lu, order %u is in writeback state",
                    inode->i_ino, folio, folio_index(folio), 
                    folio_order(folio));
                // 如果只需要找到第一个就退出，可以在这里加上 break;
            }
        }
    }
    rcu_read_unlock();
}
void print_wbp(struct folio*folio)
{
    struct f2fs_iomap_folio_state* fifs=folio->private;
    struct inode*inode=folio->mapping->host;
    if(fifs&&folio_order(folio)>0)
    {
    f2fs_err(F2FS_I_SB(folio->mapping->host),"folio index %lu, order %d, host inode ino %d,write_bytes_pending %u",folio_index(folio), folio_order(folio),inode->i_ino,atomic_read(&fifs->write_bytes_pending));
    }
}
#endif /*F2FS_DBG_H*/
void f2fs_list_folios_bio(struct bio *bio)
{
    struct folio *folio;
    struct f2fs_iomap_folio_state *fifs;
    struct folio_iter fi;
    int i = 0;

    if (!bio)
    {
        f2fs_err(F2FS_I_SB(folio->mapping->host),"bio is NULL,may not alloc");
        return;
    }
    bio_for_each_folio_all(fi, bio) {
        folio = fi.folio;
        fifs = folio->private;
        if (fifs&&folio_order(folio)>0) {
            f2fs_err(F2FS_I_SB(folio->mapping->host),
                "in bio %p,folio %p, index %lu, order %d, host inode ino %d, fi.length:%d,write_bytes_pending %u",
                bio,folio, folio_index(folio), folio_order(folio),
                folio->mapping->host->i_ino,fi.length,
                atomic_read(&fifs->write_bytes_pending));
        }
        else
        {
            f2fs_err(F2FS_I_SB(folio->mapping->host),
                "in bio %p,folio %p, index %lu, order %d, host inode ino %d, fi.length:%d",
                bio,folio, folio_index(folio), folio_order(folio),
                folio->mapping->host->i_ino,fi.length);
        }
    }
}
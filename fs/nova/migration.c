#include "nova.h"
#include "bdev.h"

/* 
 * [About Mini Buffer]
 * It is a temporary buffer solution for tiering file system.
 * Includes: (for each buffer page)
 *          down_read: mini-buffer in use
 *          down_write: under-migration
 *      tier: tier number
 *      blocknr: blocknr in this tier
 */

// Allocate a DRAM buffer in sbi
int init_dram_buffer(struct nova_sb_info *sbi) {
    unsigned int i = 0;

	sbi->bdev_buffer = kcalloc(BDEV_BUFFER_PAGES, IO_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->bdev_buffer) return -ENOMEM;

    sbi->bb_pages = kcalloc(BDEV_BUFFER_PAGES, sizeof(struct page *), GFP_KERNEL);
	if (!sbi->bb_pages) return -ENOMEM;

   
	mutex_init(&sbi->bb_mutex);

    spin_lock_init(&sbi->bal_lock);

	for (i = 0; i < BDEV_BUFFER_PAGES; i++) {
        sbi->bb_pages[i] = virt_to_page(sbi->bdev_buffer+i*IO_BLOCK_SIZE);
    }

    sbi->bal_head = kzalloc(sizeof(struct bio_async_list), GFP_KERNEL);
    
	INIT_LIST_HEAD(&sbi->bal_head->list);

    return 0;
}

void print_a_write_entry(struct nova_file_write_entry *entry, int n) {
    nova_info("#%3d [P]%p [B]%lu\n", n, entry, virt_to_block((unsigned long)entry));
    nova_info("     ||Type|Tier| num_pg | block  | pgoff  ||");
    nova_info("     ||%4u|%4u|%8u|%8llu|%8llu||", entry->entry_type, get_entry_tier(entry),
    entry->num_pages, entry->block  >> PAGE_SHIFT, entry->pgoff);
}

int print_file_write_entries(struct super_block *sb, struct nova_inode_info_header *sih) {
	void *addr;
    struct nova_inode_log_page *addrp;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
	struct nova_file_write_entry *entry;
    unsigned int inv, num;
    unsigned long epoch_id;
    int i = 0;
    // pi->log_head, pi->log_tail
	u64 curr_p = pi->log_head;
	size_t entry_size = sizeof(struct nova_file_write_entry);

    nova_info("Print [start]\n");

    nova_info("valid %u deleted %u link %u\n", pi->valid, pi->deleted, pi->i_links_count);
    addrp = nova_get_block(sb, curr_p);
    inv = addrp->page_tail.invalid_entries;
    num = addrp->page_tail.num_entries;
    epoch_id = addrp->page_tail.epoch_id;
    nova_info("Log page: %p num:%u invalid: %u epoch: %lu", addrp, num, inv, epoch_id);

	while (curr_p && curr_p != sih->log_tail) {
		if (is_last_entry(curr_p, entry_size)) {
			curr_p = next_log_page(sb, curr_p);
		    addrp = nova_get_block(sb, curr_p);
            inv = addrp->page_tail.invalid_entries;
            num = addrp->page_tail.num_entries;
            epoch_id = addrp->page_tail.epoch_id;
            nova_info("Log page: %p num:%u invalid: %u epoch: %lu", addrp, num, inv, epoch_id);
        }

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
				__func__, sih->ino);
			return -EINVAL;
		}

		addr = (void *) nova_get_block(sb, curr_p);
		entry = (struct nova_file_write_entry *) addr;
		// nova_info("curr_p %llu\n",curr_p);
		// nova_info("[entry] %llu,%llu,%u\n", 
        // entry->block >> PAGE_SHIFT, entry->pgoff, entry->num_pages);

        print_a_write_entry(entry, i++);

		curr_p += entry_size;
	}
    nova_info("Print [end]\n");

	return 0;
}

/*
 * put_dram_buffer(): Release the spin lock of the buffer
 *      The buffer is still valid after put()
 * clear_dram_buffer(): Clear the metadata (and data) of the buffer
 *      The buffer is invalid after clear()
 * Must call put() before clear()
 * There is NO DIRTY BUFFER in NOVA, since COW is applied to every write
 */ 
int clear_dram_buffer_range(struct nova_sb_info *sbi, unsigned long blockoff, unsigned long length) {
    return vpmem_flush_pages(blockoff_to_virt(blockoff), length);
}

int put_dram_buffer_range(struct nova_sb_info *sbi, unsigned long blockoff, unsigned long length) {
    return vpmem_range_rwsem_set(blockoff_to_virt(blockoff), length, RWSEM_UP);
}

bool is_dram_buffer_addr(struct nova_sb_info *sbi, void *addr) {
    return (vpmem_start <= (unsigned long)addr) && ((unsigned long)addr <= vpmem_end);
}

// Return the tier of the first write entry
int current_tier(struct inode *inode) {
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
    loff_t isize = i_size_read(inode);
    pgoff_t index = 0;
    pgoff_t end_index = (isize) >> PAGE_SHIFT;
    if ( si == NULL || sih == NULL || end_index == 0 ) return -1;
    do {
        entry = nova_find_next_entry(sb, sih, index);
        if (entry) {
            return get_entry_tier(entry);
        }
        else return -1;
    } while (index <= end_index);

    return -1;
}

// Return 0 if all write entries are in the same tier
// Else the block number of the first write entry with a different tier
int is_not_same_tier(struct inode *inode) {
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
    unsigned int num_pages;
    unsigned long pgoff;
    int t;
    loff_t isize = i_size_read(inode);
    pgoff_t index = 0;
    pgoff_t end_index = (isize) >> PAGE_SHIFT;
    bool exist = false;
    if ( si == NULL || sih == NULL || end_index == 0 ) return 1;
    do {
        // nova_info("%lu index %lu end_index %lu\n",inode->i_ino,index,end_index);
        entry = nova_find_next_entry(sb, sih, index);
        if (entry) {
            if (!exist) {
                exist = true;
                t = get_entry_tier(entry);
                continue;
            }
            if (get_entry_tier(entry) == t) {
                num_pages = le32_to_cpu(entry->num_pages);
                pgoff = le64_to_cpu(entry->pgoff);
                index = pgoff + (unsigned long)num_pages;
                continue;
            }
            else {
                return 1;
            }
        }
        else return 0;
    } while (index <= end_index);

    return 0;
}

/*
 * Migrate continuous blocks from pmem to block device, with block number
 */
int migrate_blocks_pmem_to_bdev(struct nova_sb_info *sbi, 
    void *dax_mem, unsigned long nr, int tier, unsigned long blockoff) {
    struct block_device *bdev_raw = get_bdev_raw(sbi, tier);
    int ret = nova_bdev_write_block(sbi, bdev_raw, blockoff, nr, 
        address_to_page(dax_mem), BIO_ASYNC);
    return ret;
}

/*
 * Migrate continuous blocks from block device to pmem, with block number
 */
int migrate_blocks_bdev_to_pmem(struct nova_sb_info *sbi, 
    void *dax_mem, unsigned long nr, int tier, unsigned long blockoff) {
    struct block_device *bdev_raw = get_bdev_raw(sbi, tier);
    int ret = nova_bdev_read_block(sbi, bdev_raw, blockoff, nr, 
        address_to_page(dax_mem), BIO_ASYNC);
    return ret;
}

/*
 * Migrate continuous blocks from block device to block device, with block number
 * Since DRAM buffer is currently limited, this migration is always in SYNC.
 */
int migrate_blocks_bdev_to_bdev(struct nova_sb_info *sbi, 
    unsigned long blockfrom, int from, unsigned long nr,  unsigned long blockto, int to) {
    int ret = 0;
    struct page *pg = sbi->bb_pages[0];
    struct block_device *bdev_raw_from = get_bdev_raw(sbi, from);
    struct block_device *bdev_raw_to = get_bdev_raw(sbi, to);
	mutex_lock(&sbi->bb_mutex);
    ret = nova_bdev_read_block(sbi, bdev_raw_from, blockfrom, nr, pg, BIO_SYNC);
    ret = nova_bdev_write_block(sbi, bdev_raw_to, blockto, nr, pg, BIO_SYNC);
	mutex_unlock(&sbi->bb_mutex);
    return ret;
}

// Migrate continuous blocks from pmem to block device
int migrate_blocks(struct nova_sb_info *sbi, unsigned long blockfrom, 
    unsigned long nr, int from, int to, unsigned long blocknr) {
    unsigned long raw_blockfrom = get_raw_from_blocknr(sbi, blockfrom);
    unsigned long raw_blockto = get_raw_from_blocknr(sbi, blocknr);
    if (is_tier_pmem(from) && is_tier_bdev(to)) 
        return migrate_blocks_pmem_to_bdev(sbi, (void *) sbi->virt_addr + 
            (raw_blockfrom << PAGE_SHIFT), nr, to, raw_blockto);
    if (is_tier_bdev(from) && is_tier_pmem(to)) 
        return migrate_blocks_bdev_to_pmem(sbi, (void *) sbi->virt_addr + 
            (raw_blockto << PAGE_SHIFT), nr, from, raw_blockfrom);
    if (is_tier_bdev(from) && is_tier_bdev(to)) 
        return migrate_blocks_bdev_to_bdev(sbi, raw_blockfrom, from, nr, 
            raw_blockto, to);
    return -2;
}

/*
 * Only check the corresponding mb-page, not the other pages.
 * Because in the ultimate design, each block will only have one buffer page.
 */ 
bool is_entry_busy(struct nova_sb_info *sbi, struct nova_file_write_entry *entry) {    
    if (!is_tier_bdev(get_entry_tier(entry))) return 0;
    if (vpmem_is_range_rwsem_locked(blockoff_to_virt(entry->block >> PAGE_SHIFT), le32_to_cpu(entry->num_pages))) return 1;
    
    return 0;
}

int nova_clone_write_entry(struct nova_sb_info *sbi, struct nova_inode_info *si, 
    struct nova_file_write_entry *entry, int tier, unsigned long block, struct nova_inode_update *update) {
	struct super_block *sb = sbi->sb;
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry entry_data;
    struct inode *inode = &si->vfs_inode;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
	unsigned int data_bits;

	void *addr;
	struct nova_file_write_entry *entryc;
    
    int ret = 0;
	// u64 begin_tail = 0;
	// u64 file_size = cpu_to_le64(inode->i_size);
	// u64 epoch_id = nova_get_epoch_id(sb);
	// u32 time = current_time(inode).tv_sec;


    // nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
	// 	entry->pgoff, entry->num_pages, 
    //     block, time, file_size);

    memcpy_mcsafe(&entry_data, entry, sizeof(struct nova_file_write_entry));

    // print_a_write_entry(entry);

	entry_data.entry_type = FILE_WRITE;
    entry_data.block = cpu_to_le64(nova_get_block_off(sb, block,
							sih->i_blk_type));     

	nova_update_entry_csum(&entry_data);
    // print_a_write_entry(&entry_data);

    ret = nova_append_file_write_entry(sb, pi, inode, &entry_data, update);

    if (ret) {
        nova_dbg("%s: append inode entry failed\n", __func__);
        ret = -ENOSPC;
        return ret;
    }
    

    addr = (void *) nova_get_block(sb, update->tail);
    entryc = (struct nova_file_write_entry *) addr;

	nova_flush_buffer(entryc, sizeof(struct nova_file_write_entry), 1);

	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (le32_to_cpu(entry->num_pages) << (data_bits - sb->s_blocksize_bits));

	inode->i_blocks = sih->i_blocks;
    
	sih->trans_id++;

    return 0;
}

int migrate_entry_blocks(struct nova_sb_info *sbi, int from, int to,
    struct nova_inode_info *si, struct nova_file_write_entry *entry, unsigned long blocknr_hint, 
    struct nova_inode_update *update) {
	// struct nova_inode_info_header *sih = &si->header;
    unsigned long blocknr = 0;
    int ret = 0;

    /* Step 1. Check */
    if (!entry) return ret;
    if (get_entry_tier(entry) != from) return ret;

    // print_a_write_entry(entry);

    if (is_entry_busy(sbi, entry)) {
        if (DEBUG_MIGRATION_CHECK) nova_info("entry->block %lu is busy\n", 
            (unsigned long)entry->block >> PAGE_SHIFT);
        return -1;
    }

    /* Step 2. Allocate */
    entry->updating = 1;

    // TODOzsa: Could be wrong
    if (DEBUG_MIGRATION_ALLOC) nova_info("[Migration] entry->block %lu\n", 
        (unsigned long)entry->block >> PAGE_SHIFT);
    // print_a_page((void *) sbi->virt_addr + entry->block);

    if (blocknr_hint) {
        blocknr = blocknr_hint;
        if (DEBUG_MIGRATION_ALLOC) nova_info("[Migration] Hint blocknr:%lu number:%d.\n",
            blocknr, le32_to_cpu(entry->num_pages));
    }
    else {
        ret = nova_alloc_block_tier(sbi, to, ANY_CPU, &blocknr, le32_to_cpu(entry->num_pages), ALLOC_FROM_HEAD);
        // The &blocknr is global block number

        if (ret<0) {
            nova_info("[Migration] Block allocation error.\n");
            return ret;
        }
        if (DEBUG_MIGRATION_ALLOC) nova_info("[Migration] Allocate blocknr:%lu number:%d.\n",
            blocknr, le32_to_cpu(entry->num_pages));
    }

    /* Step 3. Copy */
    ret = migrate_blocks(sbi, entry->block >> PAGE_SHIFT, le32_to_cpu(entry->num_pages), from, to, blocknr);
    if (ret<0) {
        nova_info("[Migration] Block copy error.\n");
        if (ret == -2) nova_info("[Migration] Unsupported migration attempt.\n");
        return ret;
    }

    ret = flush_bal_entry(sbi);
    if (ret<0) {
        nova_info("[Migration] Flush bal error.\n");
        return ret;
    }

    /* Step 4. Free */
    // ret = nova_free_blocks_tier(sbi, entry->block >> PAGE_SHIFT, entry->num_pages);
    // Update tiering info
    entry->updating = 0;
    // print_a_write_entry(entry);
    
	nova_update_entry_csum(entry);

    if (!blocknr_hint) ret = nova_clone_write_entry(sbi, si, entry, to, blocknr, update);
    
    return ret;
}

int migrate_group_entry_blocks(struct nova_sb_info *sbi, struct inode *inode, int from, int to,
    pgoff_t start_index, pgoff_t end_index, struct nova_inode_update *update) {
	struct super_block *sb = sbi->sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
    int ret = 0;
	u64 epoch_id;
	u32 time;
	u64 file_size = cpu_to_le64(inode->i_size);
    unsigned long blocknr = 0;
    unsigned int opt_size = 1 << sbi->bdev_list[to - TIER_BDEV_LOW].opt_size_bit;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entry_first = NULL;
	struct nova_file_write_entry entry_data;
    pgoff_t index = start_index;
	unsigned int data_bits;
    unsigned int num_pages;
    unsigned long pgoff;

    ret = nova_alloc_block_tier(sbi, to, ANY_CPU, &blocknr, opt_size, ALLOC_FROM_TAIL);
    if (ret<0) {
        nova_info("[Migration] Block allocation error.\n");
        return ret;
    }
    
    do {
        entry = nova_find_next_entry(sb, sih, index);
        if (entry) {
            if (get_entry_tier(entry) == from) {
                if (DEBUG_MIGRATION) nova_info("[Migration] Migrating (group) write entry with index:%lu\n", index);
                ret = migrate_entry_blocks(sbi, from, to, si, entry, 
                    blocknr + (entry->pgoff & (opt_size - 1)), update);
            }
            num_pages = le32_to_cpu(entry->num_pages);
            pgoff = le64_to_cpu(entry->pgoff);
            index = pgoff + num_pages;
        }
        else break;
    } while (index <= end_index);

    entry_first = nova_get_write_entry(sb, sih, start_index);

    if (DEBUG_MIGRATION_MERGE) nova_info("Merge entry: [Before] [entry] %llu,%llu,%u\n", 
        entry_first->block >> PAGE_SHIFT, entry_first->pgoff, entry_first->num_pages);

	epoch_id = nova_get_epoch_id(sb);
	time = current_time(inode).tv_sec;
    nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
		entry_first->pgoff, opt_size, 
        blocknr, time, file_size);

	nova_update_entry_csum(&entry_data);
    ret = nova_append_file_write_entry(sb, pi, inode, &entry_data, update);
    if (ret) {
        nova_dbg("%s: append inode entry failed\n", __func__);
        ret = -ENOSPC;
    }
    
	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (opt_size << (data_bits - sb->s_blocksize_bits));

    // begin_tail = update->curr_entry;
    // nova_memunlock_inode(sb, pi);
	// nova_update_inode(sb, inode, pi, update, 1);
	// nova_memlock_inode(sb, pi);
    
    // if (*bt==0) *bt = (unsigned long)begin_tail;
    // ret = nova_reassign_file_tree(sb, sih, begin_tail, true);
	// if (ret) return ret;

	inode->i_blocks = sih->i_blocks;
    
	sih->trans_id++;

    if (DEBUG_MIGRATION_MERGE) nova_info("Merge entry: [After ] [entry] %llu,%llu,%u\n", 
        entry_data.block >> PAGE_SHIFT, entry_data.pgoff, entry_data.num_pages);
    
    return ret;
}

bool is_entry_cross_boundary(struct nova_sb_info *sbi, struct nova_file_write_entry *entry, int tier) {
    unsigned int osb = sbi->bdev_list[tier - TIER_BDEV_LOW].opt_size_bit;
    if ( (entry->pgoff >> osb) !=  
        ( (entry->pgoff + entry->num_pages - 1) >> osb ) )
        return true;
    else return false;
}

// TODOzsa: concurrency
int nova_split_entry(struct super_block *sb, struct inode *inode,
    struct nova_file_write_entry *entry, int tier) {
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
	struct nova_inode_update update;
	struct nova_file_write_entry entry_data;
    int ret = 0;
	unsigned int data_bits;
	u64 epoch_id;
	u32 time;
	u64 file_size = cpu_to_le64(inode->i_size);
	u64 begin_tail = 0;
    unsigned int osb = sbi->bdev_list[tier - TIER_BDEV_LOW].opt_size_bit;

    unsigned int num_pages = le32_to_cpu(entry->num_pages);
    unsigned long pgoff = le64_to_cpu(entry->pgoff);
    unsigned long num_prev = ((pgoff + num_pages -1) >> osb << osb )- pgoff;

	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

    nova_info("Split entry: [Before] [entry] %llu,%lu,%u\n", entry->block >> PAGE_SHIFT, pgoff, num_pages);

	epoch_id = nova_get_epoch_id(sb);
	time = current_time(inode).tv_sec;
    nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
		pgoff + num_prev, num_pages - num_prev, 
        (entry->block >> PAGE_SHIFT) + num_prev, time, file_size);
    
	nova_update_entry_csum(&entry_data);
    entry->num_pages = cpu_to_le32(num_prev);

    ret = nova_append_file_write_entry(sb, pi, inode, &entry_data, &update);
    if (ret) {
        nova_dbg("%s: append inode entry failed\n", __func__);
        ret = -ENOSPC;
    }
    
	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += ((le32_to_cpu(entry->num_pages) - num_prev) << (data_bits - sb->s_blocksize_bits));
    begin_tail = update.curr_entry;
    nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);

    ret = nova_reassign_file_tree(sb, sih, begin_tail, false);
	if (ret) return ret;

	inode->i_blocks = sih->i_blocks;

	sih->trans_id++;

    nova_info("[After] [entry] %llu,%lu,%u [new_entry] %llu,%llu,%u\n", 
        entry->block >> PAGE_SHIFT, pgoff, le32_to_cpu(entry->num_pages),
        entry_data.block >> PAGE_SHIFT, entry_data.pgoff, entry_data.num_pages);

    return ret;
}

/*
 * Migrate a file from one tier to another
 * How migration works: Check -> Allocate -> Copy -> Free
 */ 


int migrate_a_file_by_entries(struct inode *inode, int from, int to)
{
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
	struct nova_file_write_entry *entry;
    struct nova_inode_update update;
	u64 begin_tail = 0;
    unsigned int num_pages;
    unsigned long pgoff;
    // unsigned long bt = 0;
    pgoff_t index = 0;
    pgoff_t end_index = 0;
    int ret = 0;
    unsigned int nentry = 0;
    loff_t isize = 0;
    
    if (DEBUG_MIGRATION) nova_info("[Migration] Start migrating (by entries) inode:%lu from:T%d to:T%d\n",
        inode->i_ino, from, to);

	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

    begin_tail = update.tail;

	isize = i_size_read(inode);
	end_index = (isize) >> PAGE_SHIFT;
    // nova_info("1 index:%lu end_index:%lu ret:%d\n", index, end_index, ret);
    do {
        entry = nova_find_next_entry(sb, sih, index);
        
        // nova_info("entry %p\n", entry);
        // nova_info("index:%lu ret:%d\n", index, ret);

        if (entry) {
            if (get_entry_tier(entry) == from) {
                if (DEBUG_MIGRATION) nova_info("[Migration] Migrating ( one ) write entry with index:%lu\n", index);
                ret = migrate_entry_blocks(sbi, from, to, si, entry, 0, &update);
            }            
            num_pages = le32_to_cpu(entry->num_pages);
            pgoff = le64_to_cpu(entry->pgoff);
            nova_info("Index:%lu entry->pgoff:%lu entry->num_pages:%u\n", index, pgoff, num_pages);
            index = pgoff + num_pages;
            nentry++;
        }
        else {
            if (DEBUG_MIGRATION) nova_info("Entry not found. inode:%lu index:%lu\n",
                inode->i_ino, index);
            break;
        }
        // nova_info("Inode %lu Index:%lu end_index:%lu\n", inode->i_ino, index, end_index);
    } while (index <= end_index);

    nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);

    // if (*bt==0) *bt = (unsigned long)begin_tail;
    ret = nova_reassign_file_tree(sb, sih, begin_tail, true);
	// if (ret) return ret;
	nova_inode_log_fast_gc(sb, pi, sih, 0, 0, 0, 0, 1);
    // nova_info("sih->log_pages: %lu\n",sih->log_pages);
    
    // ret = nova_reassign_file_tree(sb, sih, begin_tail, true);
	if (ret) return ret;

    if (DEBUG_MIGRATION) nova_info("[Migration] End migrating inode:%lu from:T%d to:T%d\n",
        inode->i_ino, from, to);
        
    if (DEBUG_MIGRATION) nova_info("[Migration] End migrating inode:%lu (%d entries)\n",
        inode->i_ino, nentry);
    
    return ret;
}

int migrate_a_file(struct inode *inode, int from, int to)
{
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
    struct nova_inode *pi = nova_get_block(sb, sih->pi_addr);
	struct nova_file_write_entry *entry;
    pgoff_t index = 0;
    pgoff_t end_index = 0;
    struct nova_inode_update update;
	u64 begin_tail = 0;
    unsigned int num_pages;
    unsigned long pgoff;

    int ret = 0;
    // unsigned long bt = 0;
    unsigned int i = 0;
    unsigned int nentry = 0;
    unsigned int n1 = 0;
    unsigned int n2 = 0;
    loff_t isize = 0;
    unsigned int osb = sbi->bdev_list[to - TIER_BDEV_LOW].opt_size_bit;

    nova_update_sih_ltier(sb, sih, to);

    if (is_tier_pmem(to)) return migrate_a_file_by_entries(inode, from, to);
    // return migrate_a_file_by_entries(inode, from, to);
    if (DEBUG_MIGRATION) nova_info("[Migration] Start migrating inode:%lu from:T%d to:T%d\n",
        inode->i_ino, from, to);

	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

    begin_tail = update.tail;

	isize = i_size_read(inode);
	end_index = (isize) >> PAGE_SHIFT;
    
    for (i=0;i<=end_index>>osb;++i) {
next:
        n1 = 0;
        n2 = 0;
        index = i<<osb;
        do {
            entry = nova_find_next_entry(sb, sih, index);
            if (entry) {
                if ( (entry->pgoff)>>osb > i ) {
                    if (n1==0) {
                        i = (entry->pgoff)>>osb;
                        goto next;
                    }
                    else goto mig;
                }
                if (is_entry_cross_boundary(sbi, entry, to)) {
                    if (n1 == 0) {
                        goto mig;
                    }
                    nova_split_entry(sb, inode, entry, to);
                }
                if (get_entry_tier(entry) == from) {
                    pgoff = le64_to_cpu(entry->pgoff);
                    num_pages = le32_to_cpu(entry->num_pages);
                    index = pgoff + num_pages;
                }
                else {
                    goto mig;
                }
                n1++;
            }
            else goto mig;
        } while (index < (i+1)<<osb);

        if (index == (i+1)<<osb) {
            if (n1!=1) {
                migrate_group_entry_blocks(sbi, inode, from, to, i<<osb, ((i+1)<<osb) -1, &update);
                nentry += n1;
                continue;
            }
            else {
                ret = migrate_entry_blocks(sbi, from, to, si, entry, 0, &update);
                nentry += 1;
                continue;
            }
        }
mig: 
        index = i<<osb;
        do {
            entry = nova_find_next_entry(sb, sih, index);
            // nova_info("entry %p\n", entry);
            // nova_info("index:%lu ret:%d\n", index, ret);

            if (entry) {
                if ( (entry->pgoff)>>osb > i ) break;
                if (is_entry_cross_boundary(sbi, entry, to)) {
                    nova_split_entry(sb, inode, entry, to);
                }
                if (get_entry_tier(entry) == from) {
                    if (DEBUG_MIGRATION) nova_info("[Migration] Migrating ( one ) write entry with index:%lu\n", index);
                    ret = migrate_entry_blocks(sbi, from, to, si, entry, 0, &update);
                }

                pgoff = le64_to_cpu(entry->pgoff);
                num_pages = le32_to_cpu(entry->num_pages);
                index = pgoff + num_pages;
                n2++;
            }
            else break;
        } while (index < (i+1)<<osb);
        nentry += n2;
    }
    
    nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);

    ret = nova_reassign_file_tree(sb, sih, begin_tail, true);
	if (ret) return ret;

	nova_inode_log_fast_gc(sb, pi, sih, 0, 0, 0, 0, 1);

    if (DEBUG_MIGRATION) nova_info("[Migration] End migrating inode:%lu from:T%d to:T%d\n",
        inode->i_ino, from, to);
        
    if (DEBUG_MIGRATION) nova_info("[Migration] End migrating inode:%lu (%d entries)\n",
        inode->i_ino, nentry);

    return ret;
}

unsigned long nova_pmem_used(struct nova_sb_info *sbi) {
	struct super_block *sb = sbi->sb;
    unsigned long used = 0;
    int i;
	struct free_list *fl = NULL;
	for (i=0;i<sbi->cpus;++i) {
		fl = nova_get_free_list(sb, i);
        used += fl->block_end - fl->block_start + 1 - fl->num_free_blocks;
	}
	if(DEBUG_MIGRATION) nova_info("[Usage] PMEM: Used:  %4lu\n", used);
    return used;
}

unsigned long nova_pmem_total(struct nova_sb_info *sbi) {
	struct super_block *sb = sbi->sb;
    unsigned long total = 0;
    int i;
	struct free_list *fl = NULL;
	for (i=0;i<sbi->cpus;++i) {
		fl = nova_get_free_list(sb, i);
        total += fl->block_end - fl->block_start + 1;
	}
	if(DEBUG_MIGRATION) nova_info("[Usage] PMEM: Total: %4lu\n", total);
    return total;
}

bool is_pmem_usage_high(struct nova_sb_info *sbi) {
    unsigned long used = nova_pmem_used(sbi);
    unsigned long total = nova_pmem_total(sbi);
    // Usage high: used / total > MIGRATION_DOWNWARD_PERC / 100
    if(DEBUG_MIGRATION) nova_info("PMEM usage: %lu / %lu.\n", used * 100, MIGRATION_DOWNWARD_PERC * total);
    return used * 100 > MIGRATION_DOWNWARD_PERC * total;
}

unsigned long nova_bdev_used(struct nova_sb_info *sbi, int tier) {
    unsigned long used = 0;
    int i;
	struct bdev_free_list *bfl = NULL;
	for (i=0;i<sbi->cpus;++i) {
		bfl = nova_get_bdev_free_list(sbi,tier,i);
        used += bfl->num_total_blocks - bfl->num_free_blocks;
	}
	if(DEBUG_MIGRATION) nova_info("[Usage] BDEV-T%d: Used:  %4lu\n", tier, used);
    return used;
}

unsigned long nova_bdev_total(struct nova_sb_info *sbi, int tier) {
    unsigned long total = 0;
    int i;
	struct bdev_free_list *bfl = NULL;
	for (i=0;i<sbi->cpus;++i) {
		bfl = nova_get_bdev_free_list(sbi, tier, i);
        total += bfl->num_total_blocks;
	}
	if(DEBUG_MIGRATION) nova_info("[Usage] BDEV-T%d: Total: %4lu\n", tier, total);
    return total;
}

bool is_bdev_usage_high(struct nova_sb_info *sbi, int tier) {
    unsigned long used = nova_bdev_used(sbi, tier);
    unsigned long total = nova_bdev_total(sbi, tier);
    // Usage high: used / total > MIGRATION_DOWNWARD_PERC / 100
    if(DEBUG_MIGRATION) nova_info("BDEV T%d usage: %lu / %lu.\n", tier, used * 100, MIGRATION_DOWNWARD_PERC * total);
    return used * 100 > MIGRATION_DOWNWARD_PERC * total;
}

struct inode *pop_an_inode_to_migrate(struct nova_sb_info *sbi, int tier) {
	struct super_block *sb = sbi->sb;
	struct inode_map *inode_map;
	struct nova_range_node *i, *next_i;
	struct rb_node *next;
    int j, jj;
    unsigned long k;
    unsigned long ino = 0;
    int cpu = smp_processor_id();

    struct inode *ret;
	struct nova_inode_info *si;
	struct nova_file_write_entry *entry;

    for (jj=cpu;jj<cpu+sbi->cpus;++jj) {
        j = jj%(sbi->cpus);
        inode_map = &sbi->inode_maps[j];
        i = inode_map->first_inode_range;

        next = &i->node;

        more:
        if (!next) {
            next_i = NULL;
            continue;
        } else {
            next_i = container_of(next, struct nova_range_node, node);
            for (k=next_i->range_low;k<=next_i->range_high;++k) {
                if (k<=8) continue;
                // nova_info("Inode found: [%d] %lu\n",j,k*sbi->cpus+j);

                ino = k*sbi->cpus+j;
                ret = nova_iget(sb, ino);
                if (!ret) goto next;
                si = NOVA_I(ret);
                if (!si) goto next;
                entry = nova_get_write_entry(sb, &si->header, 0);
                if (!entry) goto next;
                if (get_entry_tier(entry) == tier) {
                    if(DEBUG_MIGRATION) nova_info("Inode %lu is poped.\n",ino);
                    return ret;
                }
            }
        }
        next:
        next = rb_next(next);
        goto more;
    }
    return NULL;
}

int migrate_a_file_to_pmem(struct inode *inode) {
    if (current_tier(inode) == TIER_PMEM) return 0;
    else return migrate_a_file_by_entries(inode, current_tier(inode), TIER_PMEM);
}

int do_migrate_a_file_rotate(struct inode *inode) {
    int ret;
	if(DEBUG_MIGRATION) nova_info("[Migration-Rotate]\n");
    ret = is_not_same_tier(inode);
    if (ret) {
        if(DEBUG_MIGRATION) nova_info("Write entries of inode %lu is not in the same tier (index: %d)", inode->i_ino, ret);
        return -1;
    }
    switch (current_tier(inode)) {
    case TIER_PMEM:
        return migrate_a_file(inode, TIER_PMEM, TIER_BDEV_LOW);
    case TIER_BDEV_LOW:
        if (DEBUG_XFSTESTS) 
            return migrate_a_file(inode, TIER_BDEV_LOW, TIER_PMEM);
        else 
            return migrate_a_file(inode, TIER_BDEV_LOW, TIER_BDEV_HIGH);
    case TIER_BDEV_LOW+1:
        return migrate_a_file(inode, TIER_BDEV_HIGH, TIER_PMEM);
    default:
        if(DEBUG_MIGRATION) nova_info("Unsupported migration of inode %lu at tier %d", inode->i_ino, current_tier(inode));
    }
    return -1;
}

int do_migrate_a_file_downward(struct inode *inode) {
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
    struct inode *this;
    // int ret;
    int i;
	if(DEBUG_MIGRATION) nova_info("[Migration-Downward]\n");

    // ret = is_not_same_tier(inode);
    // if (ret) {
    //     if(DEBUG_MIGRATION) nova_info("Write entries of inode %lu is not in the same tier (index: %d)", inode->i_ino, ret);
    //     return -1;
    // }
    
// again:
    if (is_pmem_usage_high(sbi)) {
        if(DEBUG_MIGRATION) nova_info("PMEM usage high.\n");
        this = pop_an_inode_to_migrate(sbi, TIER_PMEM);
        if (!this) {
            if(DEBUG_MIGRATION) nova_info("PMEM usage is high yet no inode is found.\n");
            return 0;
        }
        migrate_a_file(this, TIER_PMEM, TIER_BDEV_LOW);
        // goto again;
    }
    else if(DEBUG_MIGRATION) nova_info("PMEM usage low.\n");

    for (i=TIER_BDEV_LOW;i<TIER_BDEV_HIGH;++i) {
        if (is_bdev_usage_high(sbi, i)) {
            if(DEBUG_MIGRATION) nova_info("BDEV T%d usage high.\n",i);
            this = pop_an_inode_to_migrate(sbi, i);
            if (!this) {
                if(DEBUG_MIGRATION) nova_info("BDEV T%d usage is high yet no inode is found.\n", i);
            }
            migrate_a_file(this, i, i+1);
            // goto again;
        }
        else if(DEBUG_MIGRATION) nova_info("BDEV T%d usage low.\n",i);
    }
    
    return 0;
}
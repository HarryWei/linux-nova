#include "nova.h"
#include "bdev.h"
#include <asm/tlbflush.h>

#define SECTOR_SIZE_BIT 9
#define VFS_IO_TEST 0


// This function is used for a raw block device lookup in /dev
char* find_a_raw_bdev(void) {
	struct file *fp;
	char* bdev = kzalloc(20*sizeof(char),GFP_KERNEL);
		
	fp = filp_open("/dev/sda1", O_RDONLY, 0644);
	if(fp == (struct file *)-ENOENT) {
		strcat(bdev, "/dev/sda\0");
		nova_info("sda\n");
		return bdev;
	}
	fp = filp_open("/dev/sdb1", O_RDONLY, 0644);
	if(fp == (struct file *)-ENOENT) {
		strcat(bdev, "/dev/sdb\0");
		nova_info("sdb\n");
		return bdev;
	}
	return NULL;
}

void print_a_bdev(struct nova_sb_info *sbi) {
	struct bdev_info* bdi = sbi->bdev_list;
	
	nova_info("----------------\n");
	nova_info("[New block device]\n");
	nova_info("Disk path: %s\n", bdi->bdev_path);
	nova_info("Disk name: %s\n", bdi->bdev_name);
	nova_info("Major: %d Minor: %d\n", bdi->major ,bdi->minors);
	nova_info("Size: %lu sectors (%luMB)\n",bdi->capacity_sector,bdi->capacity_page);
	nova_info("----------------\n");
}

// VFS write to disk
static void vfs_write_test(void) {
	struct file *file;
	loff_t pos = 4;
	int i;
	char* name = kmalloc(sizeof(char)*4,GFP_KERNEL);
    mm_segment_t oldfs;
	name[0] = 't';
	name[1] = 'o';
	name[3] = '\0';
	nova_info("vfs write test in %lu.\n",sizeof(char));
    oldfs = get_fs();
    set_fs(get_ds());
	file = filp_open("/dev/sda", O_WRONLY, 0644);
	for (i=0;i<10000;++i){
		name[2]='a'+i%26;
		pos+=16;
		vfs_write(file, name, sizeof(char)*4, &pos);
	}
	// vfs_fsync(file,0);
	// filp_close(file,NULL);
    set_fs(oldfs);
	nova_info("vfs write test out.\n");
}

// VFS read from disk
static void vfs_read_test(void) {
	struct file *file;
	loff_t pos = 80;
	struct inode *blk_inode;
	struct address_space *blk_mapping;
	struct address_space *blk_data;
	// char* get = kzalloc(sizeof(char)*10, GFP_KERNEL);
	char* c = kzalloc(sizeof(char)*13, GFP_KERNEL);
    mm_segment_t oldfs;
	nova_info("vfs read test in.\n");
 
    oldfs = get_fs();
	set_fs(get_ds());
	file = filp_open("/dev/sda", O_RDONLY, 0644);
	
	blk_inode = file->f_inode;
	nova_info("vfs read test mid1.\n");

	nova_info("vfs read test i_rdev:%u, i_size:%lld.\n",blk_inode->i_rdev,blk_inode->i_size);

	nova_info("vfs read test i_blkbits:%u, i_bytes:%u, i_blocks:%lu.\n",blk_inode->i_blkbits,blk_inode->i_bytes,blk_inode->i_blocks);
	
	nova_info("vfs read test i_ino:%lu.\n",blk_inode->i_ino);
	blk_mapping = blk_inode->i_mapping;
	blk_data = &blk_inode->i_data;
	// address space i_mapping
	nova_info("vfs read test mapping: i_ino:%lu.\n",blk_mapping->host->i_ino);
	nova_info("vfs read test mapping: nrpages:%lu.\n",blk_mapping->nrpages);

	// address space i_data
	// nova_info("vfs read test data: i_ino:%lu.\n",blk_data->host->i_ino);
	nova_info("vfs read test data: nrpages:%lu.\n",blk_data->nrpages);

	vfs_read(file, c,sizeof(char)*12, &pos);
	nova_info("vfs read test %s.\n",c);
	nova_info("vfs read test out.\n");

    set_fs(oldfs);
}

// Key: the first character
// 0 = A | 1 = B | ... | 25 = Z
int modify_a_page(void* addr, int keychar) {
	char* c = addr;
	int i = 0;
	int key = keychar - 'A';
	char* word = kmalloc(26*sizeof(char)*5+1,GFP_KERNEL);
	while (i<26*5) {
		word[i]='A'+i%26;
		i++;
	}
	for (i=0;i<64;++i) {
		memcpy(c+i*64,&word[key+i%26],64);
	}
	
	kfree(word);	
	if (i<64) return -1;
	return 0;
}

// Print the page in terminal
void print_a_page(void* addr) {
	char* c = addr;
	// wordline: how many characters are shown in one line
	int wordline = 128;
	char* p = kmalloc(wordline*sizeof(char)+1,GFP_KERNEL);
	int i = 0;
	int j = 0;
	char space = ' ';
	p[wordline]='\0';
	if (c[i]) nova_info("[Page data] (Start with: %c)\n",c[i]);
	else nova_info("[Page data]\n");
	nova_info("----------------\n");
	while (i<IO_BLOCK_SIZE) {
		p[0]='\0';
		for (j=0;j<wordline;j+=32) {
			strncat(p,c+i+j,32);
			strcat(p,&space);
		}
		nova_info("%p %s\n",addr+i,p);
		i+=wordline;
	}
	nova_info("----------------\n");
}

// Return 0 on success
int nova_bdev_write_byte(struct block_device *device, unsigned long offset,
	unsigned long size, struct page *page, unsigned long page_offset, bool sync) {
   	int ret = 0;
	struct bio *bio = bio_alloc(GFP_NOIO, 1);
	struct bio_vec *bv = kzalloc(sizeof(struct bio_vec), GFP_KERNEL);
	if(DEBUG_BDEV_RW) nova_info("[Bdev Write] Offset %7lu <- Page %p (size: %lu)\n",offset>>12,
	page_address(page)+page_offset,size);
	bio->bi_bdev = device;
	bio->bi_iter.bi_sector = offset >> 9;
	bio->bi_iter.bi_size = size;
	bio->bi_vcnt = 1;
	bv->bv_page = page;
	bv->bv_len = size;
	bv->bv_offset = page_offset;
	bio->bi_io_vec = bv;
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
	if (sync) ret = submit_bio_wait(bio);
	else ret = submit_bio(bio);
	bio_put(bio);
	return ret;
}

// Return 0 on success
int nova_bdev_write_block(struct block_device *device, unsigned long offset,
	unsigned long size, struct page *page, bool sync) {
	return nova_bdev_write_byte(device,offset<<IO_BLOCK_SIZE_BIT,
		size<<IO_BLOCK_SIZE_BIT, page, 0, sync);
}

int nova_bdev_read_byte(struct block_device *device, unsigned long offset,
	unsigned long size, struct page *page, unsigned long page_offset, bool sync) {
	int ret = 0;
	struct bio *bio = bio_alloc(GFP_NOIO, 1);
	struct bio_vec *bv = kzalloc(sizeof(struct bio_vec), GFP_KERNEL);
	// bio is about block and bv is about page
	if(DEBUG_BDEV_RW) nova_info("[Bdev Read ] Offset %7lu -> Page %p (size: %lu)\n",offset>>12,
	page_address(page)+page_offset,size);
	bio->bi_bdev = device;
	bio->bi_iter.bi_sector = offset >> 9;
	bio->bi_iter.bi_size = size;
	bio->bi_vcnt = 1;
	bv->bv_page = page;
	bv->bv_len = size;
	bv->bv_offset = page_offset;
	bio->bi_io_vec = bv;
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (sync==BIO_SYNC)	submit_bio_wait(bio);
	else submit_bio(bio);
	bio_put(bio);
	return ret;
}

int nova_bdev_read_block(struct block_device *device, unsigned long offset,
	unsigned long size, struct page *page, bool sync) {
	return nova_bdev_read_byte(device,offset<<IO_BLOCK_SIZE_BIT,
		size<<IO_BLOCK_SIZE_BIT, page, 0, sync);
}

// int nova_bdev

void nova_delete_bdev_free_list(struct super_block *sb) {
	struct nova_sb_info *sbi = NOVA_SB(sb);

	/* Each tree is freed in save_blocknode_mappings */
	kfree(sbi->bdev_free_list);
	sbi->bdev_free_list = NULL;
}


int nova_alloc_bdev_block_free_lists(struct super_block *sb) {
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct bdev_free_list *bdev_free_list;

	sbi->bdev_free_list = kmalloc(sizeof(struct bdev_free_list), GFP_KERNEL);

	if (!sbi->bdev_free_list)
		return -ENOMEM;

	bdev_free_list = nova_get_bdev_free_list(sb);
	bdev_free_list->block_free_tree = RB_ROOT;
	spin_lock_init(&bdev_free_list->s_lock);

	return 0;
}

static void nova_init_bdev_free_list(struct super_block *sb,
	struct bdev_free_list *bfl) {
	struct nova_sb_info *sbi = NOVA_SB(sb);

	bfl->num_total_blocks = sbi->bdev_list->capacity_page;
	
	bfl->block_start = sbi->num_blocks;
	bfl->block_end = sbi->num_blocks + sbi->bdev_list->capacity_page-1;

	nova_info("bfl->block_end = %lu\n",bfl->block_end);

}

void nova_init_bdev_blockmap(struct super_block *sb, int recovery) {
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct rb_root *tree;
	struct nova_range_node *blknode;
	struct bdev_free_list *bfl;
	int ret;
	
	bfl = nova_get_bdev_free_list(sb);
	tree = &(bfl->block_free_tree);
	nova_init_bdev_free_list(sb, bfl);

	/* For recovery, update these fields later */
	if (recovery == 0) {
		bfl->num_free_blocks = bfl->block_end - bfl->block_start + 1;

		blknode = nova_alloc_blocknode(sb);
		if (blknode == NULL)
			BUG();
		blknode->range_low = bfl->block_start;
		blknode->range_high = bfl->block_end;
		nova_update_range_node_checksum(blknode);
		ret = nova_insert_blocktree(sbi, tree, blknode);
		if (ret) {
			nova_err(sb, "%s failed\n", __func__);
			nova_free_blocknode(sb, blknode);
			return;
		}
		bfl->first_node = blknode;
		bfl->last_node = blknode;
		bfl->num_blocknode = 1;
	}

	nova_dbgv("%s: free list of bdev: block start %lu, end %lu, %lu free blocks\n",
			__func__,
			bfl->block_start,
			bfl->block_end,
			bfl->num_free_blocks);
			
}

/* Return how many blocks allocated */
static long nova_alloc_blocks_in_bdev_free_list(struct super_block *sb,
	struct bdev_free_list *bfl, unsigned long num_blocks,
	unsigned long *new_blocknr, enum nova_alloc_direction from_tail) {
		
	struct rb_root *tree;
	struct nova_range_node *curr, *next = NULL, *prev = NULL;
	struct rb_node *temp, *next_node, *prev_node;
	unsigned long curr_blocks;
	bool found = 0;

	if (!bfl->first_node || bfl->num_free_blocks == 0) {
		nova_dbgv("%s:[Bdev] Can't alloc. free_list->first_node=0x%p free_list->num_free_blocks = %lu",
			  __func__, bfl->first_node, bfl->num_free_blocks);
		return -ENOSPC;
	}

	tree = &(bfl->block_free_tree);
	if (from_tail == ALLOC_FROM_HEAD)
		temp = &(bfl->first_node->node);
	else
		temp = &(bfl->last_node->node);

	while (temp) {
		curr = container_of(temp, struct nova_range_node, node);

		if (!nova_range_node_checksum_ok(curr)) {
			nova_err(sb, "%s curr failed\n", __func__);
			goto next;
		}

		curr_blocks = curr->range_high - curr->range_low + 1;

		if (num_blocks >= curr_blocks) {
			/* Superpage allocation must succeed */
			if (num_blocks > curr_blocks)
				goto next;

			/* Otherwise, allocate the whole blocknode */
			if (curr == bfl->first_node) {
				next_node = rb_next(temp);
				if (next_node)
					next = container_of(next_node,
						struct nova_range_node, node);
				bfl->first_node = next;
			}

			if (curr == bfl->last_node) {
				prev_node = rb_prev(temp);
				if (prev_node)
					prev = container_of(prev_node,
						struct nova_range_node, node);
				bfl->last_node = prev;
			}

			rb_erase(&curr->node, tree);
			bfl->num_blocknode--;
			num_blocks = curr_blocks;
			*new_blocknr = curr->range_low;
			nova_free_blocknode(sb, curr);
			found = 1;
			break;
		}

		/* Allocate partial blocknode */
		if (from_tail == ALLOC_FROM_HEAD) {
			*new_blocknr = curr->range_low;
			curr->range_low += num_blocks;
		} else {
			*new_blocknr = curr->range_high + 1 - num_blocks;
			curr->range_high -= num_blocks;
		}

		nova_update_range_node_checksum(curr);
		found = 1;
		break;
next:
		if (from_tail == ALLOC_FROM_HEAD)
			temp = rb_next(temp);
		else
			temp = rb_prev(temp);
	}

	if (bfl->num_free_blocks < num_blocks) {
		nova_dbg("%s: free list of block device has %lu free blocks, but allocated %lu blocks?\n",
				__func__, bfl->num_free_blocks, num_blocks);
		return -ENOSPC;
	}

	if (found == 1)
		bfl->num_free_blocks -= num_blocks;
	else {
		nova_dbgv("%s: Can't alloc.  found = %d", __func__, found);
		return -ENOSPC;
	}

	return num_blocks;
}

/*
 * Allocate data block from block device, return how many blocks allocated
 * *blocknr: Returns the offset of block
 * num_block: Number of blocks of the request
 * from_tail: Direction
 */ 
static int nova_new_blocks_from_bdev(struct super_block *sb, unsigned long *blocknr,
	unsigned int num_blocks, enum nova_alloc_direction from_tail) {

	struct bdev_free_list *bfl;
	unsigned long new_blocknr = 0;
	long ret_blocks = 0;

	if (num_blocks == 0) {
		nova_dbg_verbose("%s: num_blocks == 0", __func__);
		return -EINVAL;
	}

	bfl = nova_get_bdev_free_list(sb);
	spin_lock(&bfl->s_lock);

	ret_blocks = nova_alloc_blocks_in_bdev_free_list(sb, bfl, num_blocks, &new_blocknr, from_tail);

	if (ret_blocks > 0) {
		bfl->num_blocknode +=1;
		bfl->num_free_blocks -= ret_blocks;
	}

	spin_unlock(&bfl->s_lock);

	if (ret_blocks <= 0 || new_blocknr == 0) {
		nova_dbg_verbose("%s: not able to allocate %d blocks from bdev.  ret_blocks=%ld; new_blocknr=%lu",
				 __func__, num_blocks, ret_blocks, new_blocknr);
		return -ENOSPC;
	}

	*blocknr = new_blocknr;

	nova_info("[Bdev] Alloc %lu BDEV blocks 0x%lx\n", ret_blocks, *blocknr);
	return ret_blocks;
}


int nova_free_blocks_from_bdev(struct super_block *sb, unsigned long blocknr,
	unsigned int num_blocks)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct rb_root *tree;
	unsigned long block_low;
	unsigned long block_high;
	struct nova_range_node *prev = NULL;
	struct nova_range_node *next = NULL;
	struct nova_range_node *curr_node;
	struct bdev_free_list *bfl;
	int new_node_used = 0;
	int ret;

	if (num_blocks <= 0) {
		nova_dbg("%s ERROR: free %d\n", __func__, num_blocks);
		return -EINVAL;
	}

	/* Pre-allocate blocknode */
	curr_node = nova_alloc_blocknode(sb);
	if (curr_node == NULL) {
		/* returning without freeing the block*/
		return -ENOMEM;
	}

	bfl = nova_get_bdev_free_list(sb);
	spin_lock(&bfl->s_lock);

	tree = &(bfl->block_free_tree);

	block_low = blocknr;
	block_high = blocknr + num_blocks - 1;

	nova_dbgv("Free: %lu - %lu\n", block_low, block_high);

	if (blocknr < bfl->block_start ||
			blocknr + num_blocks > bfl->block_end + 1) {
		nova_err(sb, "free blocks %lu to %lu, free list in bdev, start %lu, end %lu\n",
				blocknr, blocknr + num_blocks - 1,
				bfl->block_start,
				bfl->block_end);
		ret = -EIO;
		goto out;
	}

	ret = nova_find_free_slot(sbi, tree, block_low,
					block_high, &prev, &next);

	if (ret) {
		nova_dbg("%s: find free slot fail: %d\n", __func__, ret);
		goto out;
	}

	if (prev && next && (block_low == prev->range_high + 1) &&
			(block_high + 1 == next->range_low)) {
		/* fits the hole */
		rb_erase(&next->node, tree);
		bfl->num_blocknode--;
		prev->range_high = next->range_high;
		nova_update_range_node_checksum(prev);
		if (bfl->last_node == next)
			bfl->last_node = prev;
		nova_free_blocknode(sb, next);
		goto block_found;
	}
	if (prev && (block_low == prev->range_high + 1)) {
		/* Aligns left */
		prev->range_high += num_blocks;
		nova_update_range_node_checksum(prev);
		goto block_found;
	}
	if (next && (block_high + 1 == next->range_low)) {
		/* Aligns right */
		next->range_low -= num_blocks;
		nova_update_range_node_checksum(next);
		goto block_found;
	}

	/* Aligns somewhere in the middle */
	curr_node->range_low = block_low;
	curr_node->range_high = block_high;
	nova_update_range_node_checksum(curr_node);
	new_node_used = 1;
	ret = nova_insert_blocktree(sbi, tree, curr_node);
	if (ret) {
		new_node_used = 0;
		goto out;
	}
	if (!prev)
		bfl->first_node = curr_node;
	if (!next)
		bfl->last_node = curr_node;

	bfl->num_blocknode++;

block_found:
	bfl->num_free_blocks += num_blocks;

out:
	spin_unlock(&bfl->s_lock);
	if (new_node_used == 0)
		nova_free_blocknode(sb, curr_node);

	return ret;
}

/* 
 * Allocate blocks in block device
 * ret:	If unsuccess, return error number.
 * 		If success, return how many blocks it allocates 
 * 			(could be not enough, since it forces continuous)
 * blocknr: the block number
 * TODOzsa: multi-tier
 */ 
int nova_bdev_alloc_blocks(struct nova_sb_info *sbi, unsigned long *blocknr,
	unsigned int num_blocks) {
	struct super_block *sb = sbi->sb;
	int ret = 0;
	ret = nova_new_blocks_from_bdev(sb, blocknr, num_blocks, ALLOC_FROM_HEAD);
	*blocknr -= sbi->num_blocks;
	return ret;
}

// TODOzsa: multi-tier
int nova_bdev_free_blocks(struct super_block *sb, unsigned long blocknr,
	unsigned int num_blocks) {
	
	struct nova_sb_info *sbi = NOVA_SB(sb);
	return nova_free_blocks_from_bdev(sb, blocknr + sbi->num_blocks, num_blocks);
}


// bool check

void bdev_test(struct nova_sb_info *sbi) {
	struct block_device *bdev_raw = sbi->bdev_list->bdev_raw;
	
	struct page *pg;
	struct page *pg2;
	void *pg_vir_addr = NULL;
	void *pg_vir_addr2 = NULL;
	int ret=0;
	int i=0;

	char *bdev_name = sbi->bdev_list->bdev_path;

	unsigned long capacity_page = sbi->bdev_list->capacity_page;

    nova_info("Block device test in.\n");
    
	pg = alloc_page(GFP_KERNEL|__GFP_ZERO);
	pg2 = alloc_page(GFP_KERNEL|__GFP_ZERO);
	pg_vir_addr = page_address(pg);
	pg_vir_addr2 = page_address(pg2);
	
	print_a_page(pg_vir_addr);
	modify_a_page(pg_vir_addr,'X');
	print_a_page(pg_vir_addr);
	print_a_page(pg_vir_addr2);

    if (VFS_IO_TEST) {
        vfs_write_test();
        vfs_read_test();
    }

	// Sector write:
	// ret = writePage(bdev_raw, 0, bdev_logical_block_size(bdev_raw), pg);
	// Sector read:
	// ret = readPage(bdev_raw, 0, bdev_logical_block_size(bdev_raw), pg2);

	// Page write
	ret = nova_bdev_write_block(bdev_raw, 1, 1, pg, BIO_SYNC);
	ret = nova_bdev_write_block(bdev_raw, capacity_page-1, 1, pg, BIO_SYNC);
	// Page read
	for (i=0;i<20;++i) {
		if (i>capacity_page-2) continue;
		modify_a_page(pg_vir_addr,'C'+i%20);
		ret = nova_bdev_write_block(bdev_raw, i, 1, pg, BIO_SYNC);
		ret = nova_bdev_read_block(bdev_raw, i, 1, pg2, BIO_SYNC);
		if(i%100==50) {
			nova_info("[%s] [Block %d]\n",bdev_name,i);
		 	print_a_page(pg_vir_addr2);
		}
	}
	
	nova_info("Block device test out %u.\n",bdev_raw->bd_block_size);
}

void bfl_test(struct nova_sb_info *sbi) {
	struct super_block *sb = sbi->sb;
	unsigned long tmp;
	int ret;
	int i = 0;

	ret = nova_bdev_alloc_blocks(sbi, &tmp, 1);
	nova_info("[bfl1] ret:%d, offset:%lu" ,ret, tmp);
	ret = nova_bdev_alloc_blocks(sbi, &tmp, 2);
	nova_info("[bfl2] ret:%d, offset:%lu" ,ret, tmp);
	ret = nova_bdev_alloc_blocks(sbi, &tmp, 3);
	nova_info("[bfl3] ret:%d, offset:%lu" ,ret, tmp);
	ret = nova_bdev_free_blocks(sb, 1, 2);
	nova_info("[bfl4] ret:%d" ,ret);
	ret = nova_bdev_alloc_blocks(sbi, &tmp, 2);
	nova_info("[bfl5] ret:%d, offset:%lu" ,ret, tmp);

	for (i=0;i<33;++i) {
		nova_info("[bfl6] block %d\n", i); 
		ret = buffer_data_block_from_bdev(sbi,1,i);
		nova_info("[bfl6] buffer %d\n", ret); 
		put_dram_buffer(sbi, ret);
	}
}
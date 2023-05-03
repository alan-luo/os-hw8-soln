/*
 * A basic filesystem. Can be mounted on block devices.
 *
 * Made for Columbia University's COMS W4118.
 *
 * Author: Mitchell Gouzenko <mgouzenko@gmail.com>
 */
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>

#include "pantryfs_inode.h"
#include "pantryfs_inode_ops.h"
#include "pantryfs_file.h"
#include "pantryfs_file_ops.h"
#include "pantryfs_sb.h"
#include "pantryfs_sb_ops.h"

/* pantry_inode_exists - check whether an inode exists in the filesystem
 * @sb:         superblock of file system
 * @ino:        inode number to check
 *
 * Returns:     1 if the inode exists, else 0
 */
int pantry_inode_exists(struct pantryfs_super_block *pfs_sb, unsigned long ino)
{
	return IS_SET(pfs_sb->free_inodes, ino - 1);
}

static inline struct pantryfs_inode *get_pfs_inode(struct inode *inode)
{
	return inode->i_private;
}

/* The pantryfs_super_block is nested in the super_block structure. This is just
 * a convenience method to get at it.
 */
struct pantryfs_super_block *get_pfs_sb(struct super_block *sb)
{
	struct pantryfs_sb_buffer_heads *pfs_sb_bufs;
	struct pantryfs_super_block *pfs_sb;
	struct buffer_head *bh;

	pfs_sb_bufs = (struct pantryfs_sb_buffer_heads *)sb->s_fs_info;
	bh = pfs_sb_bufs->sb_bh;
	pfs_sb = (struct pantryfs_super_block *)bh->b_data;
	return pfs_sb;
}

/* Finds a free inode and sets the bit, indicating that it's no longer free. */
int pfs_claim_free_inode_num(struct pantryfs_super_block *pfs_sb)
{
	int i;

	for (i = 0; i < PFS_MAX_INODES; i++) {
		if (!IS_SET(pfs_sb->free_inodes, i)) {
			SETBIT(pfs_sb->free_inodes, i);

			/* Inode numbers start at 1. */
			return i + 1;
		}
	}

	return 0;
}

static inline void pfs_release_inode(struct pantryfs_super_block *pfs_sb,
				     int inode_num)
{
	CLEARBIT(pfs_sb->free_inodes, inode_num - 1);
}

/* Finds free data block and sets the bit, indicating it's no longer free. */
int pfs_claim_free_data_block(struct pantryfs_super_block *pfs_sb)
{
	int i;

	for (i = 0; i < PFS_MAX_INODES; i++) {
		if (!IS_SET(pfs_sb->free_data_blocks, i)) {
			SETBIT(pfs_sb->free_data_blocks, i);

			/* Datablocks for inodes start at the root datablock.
			 * The first two datablocks are used for the superblock
			 * and inode store respectively.
			 */
			return PANTRYFS_ROOT_DATABLOCK_NUMBER + i;
		}
	}

	return 0;
}

static inline void pfs_release_datablock(struct pantryfs_super_block *pfs_sb,
					 int block_num)
{
	CLEARBIT(pfs_sb->free_data_blocks,
		 block_num - PANTRYFS_ROOT_DATABLOCK_NUMBER);
}

/* Read the inode specified by inode_num from the disk. This function returns
 * a pointer to the inode, which is located in the buffer_head that backs the
 * inode store. Thus, any changes made to the returned inode will eventually
 * make it back to the disk.
 */
struct pantryfs_inode *pantryfs_get_inode_from_disk(struct super_block *sb,
						    uint64_t inode_num)
{
	struct pantryfs_sb_buffer_heads *pfs_sb_bufs;
	struct buffer_head *bh;
	struct pantryfs_inode *pfs_inode = NULL;

	pfs_sb_bufs = (struct pantryfs_sb_buffer_heads *)sb->s_fs_info;
	bh = pfs_sb_bufs->i_store_bh;

	if (inode_num > PFS_MAX_INODES) {
		pr_err("Inode number is too large.\n");
		return ERR_PTR(-EINVAL);
	}
	if (!pantry_inode_exists(get_pfs_sb(sb), inode_num)) {
		pr_err("Inode does not exist.\n");
		return ERR_PTR(-EINVAL);
	}

	/* Get a pointer to the first inode. */
	pfs_inode = (struct pantryfs_inode *)bh->b_data;

	/* Advance the inode pointer to the target inode. */
	return pfs_inode + (inode_num - 1);
}

/**
 * This function procures a new inode for an existing PFS file using the iget_locked function.
 * iget_locked looks in the inode cache first. There are two possibilities:
 *
 * 1) iget_locked fails to find an inode in the cache. It gives us a new inode,
 *    and we have to initialize its members by looking at the relevant data on
 *    disk.
 *
 * 2) iget_locked finds an inode. For example, if we're creating another hard
 *    link to an inode, the inode might already have been opened and be in the
 *    cache.
 *
 * @sb:		The superblock of the filesystem.
 * @parent:	The inode of the parent directory.
 * @inode_no:	The number of the inode that we want to retrieve.
 */
struct inode *pantryfs_iget_existing(struct super_block *sb,
				     struct inode *parent, uint64_t inode_no)
{
	struct inode *inode;
	struct pantryfs_inode *pfs_inode;
	char *link;

	pfs_inode = pantryfs_get_inode_from_disk(sb, inode_no);
	if (IS_ERR(pfs_inode))
		return (struct inode *)pfs_inode;

	if (S_ISLNK(pfs_inode->mode)) {
		struct buffer_head *sym_bh;

		sym_bh = sb_bread(sb, pfs_inode->data_block_number);
		if (!sym_bh)
			return ERR_PTR(-EIO);

		link = kmalloc((pfs_inode->file_size + 1) * sizeof(char),
			       GFP_KERNEL);
		if (link == NULL) {
			brelse(sym_bh);
			return ERR_PTR(-ENOMEM);
		}

		memcpy(link, sym_bh->b_data, pfs_inode->file_size);
		link[pfs_inode->file_size] = 0;

		brelse(sym_bh);
	}
	/**
	 * iget_locked checks the inode cache and returns the inode if it's in
	 * there. If not, it will allocate a new inode.
	 */
	inode = iget_locked(sb, inode_no);

	/*If it's a new inode, we have some initialization to do. */
	if (inode && (inode->i_state & I_NEW)) {
		/* Sets uid, guid, mode, of new inode. */
		i_uid_write(inode, pfs_inode->uid);
		i_gid_write(inode, pfs_inode->gid);
		inode->i_mode = pfs_inode->mode;

		inode->i_sb = sb;

		set_nlink(inode, pfs_inode->nlink);
		inode->i_blocks = PFS_BLOCK_SIZE / 512;
		inode->i_atime = pfs_inode->i_atime;
		inode->i_mtime = pfs_inode->i_mtime;
		inode->i_ctime = pfs_inode->i_ctime;
		inode->i_private = pfs_inode;

		switch (pfs_inode->mode & S_IFMT) {
		case S_IFDIR:
			inode->i_size = pfs_inode->file_size;
			inode->i_op = &pantryfs_inode_ops;
			inode->i_fop = &pantryfs_dir_ops;
			break;
		case S_IFREG:
			inode->i_size = pfs_inode->file_size;
			inode->i_op = &pantryfs_inode_ops;
			inode->i_fop = &pantryfs_file_ops;
			break;
		case S_IFLNK:
			inode->i_size = pfs_inode->file_size;
			inode->i_op = &pantryfs_symlink_inode_ops;
			inode->i_link = link;
			break;
		default:
			break;
		}

		unlock_new_inode(inode);
	} else if (S_ISLNK(pfs_inode->mode)) {
		kfree(link);
	}

	return inode;
}

/**
 * Helper function to create a new inode and dentry for PantryFS files.
 *
 * @parent:	The inode corresponding to the parent directory.
 * @dentry:	A dentry representing the child that should be created.
 *		The child's name is embedded in the dentry.
 * @mode:	Mode of the child. Includes the type bits.
 * @symname: Symlink data for "fast" symlinks. Only used if mode has S_IFLNK flag.
 *
 * Returns pointer to the newly created inode on success.
 */
struct inode *pantryfs_create_file(struct inode *parent, struct dentry *dentry,
				   umode_t mode, const char *symname)
{
	struct inode *inode, *ret;
	struct super_block *sb;
	struct pantryfs_inode *child_pfs_inode, *parent_pfs_inode;
	struct pantryfs_super_block *pfs_sb;
	struct pantryfs_dir_entry *pfs_dentry;
	struct buffer_head *bh;
	int data_block_number, inode_number, i;

	ret = NULL;

	/**
	 * We need to add a dentry for this new file in the parent directory.
	 */
	sb = parent->i_sb;
	parent_pfs_inode = get_pfs_inode(parent);
	bh = sb_bread(sb, parent_pfs_inode->data_block_number);
	if (!bh)
		return ERR_PTR(-EIO);

	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data;
	for (i = 0; i < PFS_MAX_CHILDREN; ++i, ++pfs_dentry) {
		if (!pfs_dentry->active)
			break;
	}

	/* If we haven't found a free slot, we've exceeded PFS_MAX_CHILDREN. */
	if (i >= PFS_MAX_CHILDREN) {
		WARN_ON(i > PFS_MAX_CHILDREN);
		pr_err("Reached max children for pfs directory.\n");
		ret = ERR_PTR(-ENOSPC);
		goto out;
	}

	/* Claim a free inode number. */
	pfs_sb = get_pfs_sb(sb);
	inode_number = pfs_claim_free_inode_num(pfs_sb);
	if (!inode_number) {
		pr_err("No free inodes left.\n");
		ret = ERR_PTR(-ENOSPC);
		goto out;
	}

	/* And claim a free data block... */
	data_block_number = pfs_claim_free_data_block(pfs_sb);
	if (!data_block_number) {
		pr_err("No free data blocks left.\n");
		ret = ERR_PTR(-ENOSPC);
		goto out;
	}

	/**
	 * Now, read this file's pantryfs_inode from the disk. This inode will
	 * have garbage in it so, we must initialize all of the members.
	 *
	 * pantryfs_iget_existing will later use this pantryfs_inode to populate the VFS
	 * inode, so we must be sure to perform initialization before calling
	 * pantryfs_iget_existing.
	 */
	child_pfs_inode = pantryfs_get_inode_from_disk(sb, inode_number);
	if (IS_ERR(child_pfs_inode)) {
		ret = (struct inode *)child_pfs_inode;
		goto out;
	}

	if (S_ISREG(mode)) {
		child_pfs_inode->nlink = 1;
		child_pfs_inode->file_size = 0;
	} else if (S_ISDIR(mode)) {
		/*
		 * Empty directories have two links: one from its parent and one
		 * from the loopback "." directory entry.
		 */
		child_pfs_inode->nlink = 2;
		child_pfs_inode->file_size = PFS_BLOCK_SIZE;
	} else if (S_ISLNK(mode)) {
		child_pfs_inode->nlink = 1;
		child_pfs_inode->file_size = strlen(symname);
	}

	child_pfs_inode->data_block_number = data_block_number;
	child_pfs_inode->mode = mode;
	child_pfs_inode->i_atime = child_pfs_inode->i_mtime =
		child_pfs_inode->i_ctime = current_time(parent);

	/* Get a VFS inode for the new file. */

	/**
	 * iget_locked checks the inode cache and returns the inode if it's in
	 * there. If not, it will allocate a new inode.
	 */
	inode = iget_locked(sb, inode_number);
	if (IS_ERR(inode)) {
		brelse(bh);
		pr_err("Could not allocate new inode\n");
		return inode;
	}

	/* Since we are creating a new PFS file, the inode will also be new (not just uncached)
	 * That is, inode->i_state & I_NEW is set.
	 */
	WARN_ON(!(inode->i_state & I_NEW));

	/* Sets uid, guid, mode, of new inode. */
	inode_init_owner(inode, parent, child_pfs_inode->mode);

	inode->i_sb = sb;

	set_nlink(inode, child_pfs_inode->nlink);
	inode->i_blocks = PFS_BLOCK_SIZE / 512;
	inode->i_atime = child_pfs_inode->i_atime;
	inode->i_mtime = child_pfs_inode->i_mtime;
	inode->i_ctime = child_pfs_inode->i_ctime;
	inode->i_private = child_pfs_inode;

	switch (child_pfs_inode->mode & S_IFMT) {
	case S_IFDIR:
		inode->i_size = child_pfs_inode->file_size;
		inode->i_op = &pantryfs_inode_ops;
		inode->i_fop = &pantryfs_dir_ops;
		break;
	case S_IFREG:
		inode->i_size = child_pfs_inode->file_size;
		inode->i_op = &pantryfs_inode_ops;
		inode->i_fop = &pantryfs_file_ops;
		break;
	case S_IFLNK:
		inode->i_size = child_pfs_inode->file_size;
		inode->i_op = &pantryfs_symlink_inode_ops;
		// Fast symlink is populated in pantryfs_symlink(), which calls this function.
		break;
	default:
		break;
	}

	unlock_new_inode(inode);

	child_pfs_inode->uid = i_uid_read(inode);
	child_pfs_inode->gid = i_gid_read(inode);

	/* This links the dentry to the inode we just got. */
	d_instantiate(dentry, inode);

	strncpy(pfs_dentry->filename, dentry->d_name.name,
		PANTRYFS_MAX_FILENAME_LENGTH);
	pfs_dentry->inode_no = inode_number;
	pfs_dentry->active = true;
	mark_buffer_dirty(bh);

	ret = inode;

out:
	brelse(bh);
	return ret;
}

/**
 * This is called to create a new regular file. We have to create a new inode
 * and link it up with the dentry with d_instantiate. This is done in the
 * pantryfs_create_file() helper function.
 *
 * @parent:	The inode corresponding to the parent directory.
 * @dentry:	A dentry representing the child that should be created.
 *		The child's name is embedded in the dentry.
 * @mode:	Mode of the child.
 * @excl:	If the file should be created "exclusively".
 *		See O_EXCL flag for open.
 */
int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode,
		    bool excl)
{
	struct inode *inode;

	if (!S_ISREG(mode)) {
		pr_err("Unsupported mode for pantryfs: %d\n", mode & S_IFMT);
		return -EACCES;
	}

	if (strnlen(dentry->d_name.name, PANTRYFS_MAX_FILENAME_LENGTH + 1) >
	    PANTRYFS_MAX_FILENAME_LENGTH) {
		pr_err("Filename too long\n");
		return -ENAMETOOLONG;
	}

	inode = pantryfs_create_file(parent, dentry, mode, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	return 0;
}

/* Unlinks the file represented by dentry from the parent directory dir.
 * If that was the last link, the inode will eventually be removed by VFS.
 * Note that we must not deallocate the inode ourselves. VFS takes care of this,
 * as there may still be open files using this inode.
 *
 * @dir:	Parent
 * @dentry:	Victim
 */
int pantryfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct pantryfs_inode *pfs_dir_inode;
	struct pantryfs_dir_entry *pfs_dentry;
	struct pantryfs_super_block *pfs_sb;
	struct super_block *sb;
	struct buffer_head *bh;
	int i;

	sb = dir->i_sb;
	pfs_sb = get_pfs_sb(sb);

	/* Get a pointer to the first dentry of this directory. */
	pfs_dir_inode = get_pfs_inode(dir);
	bh = sb_bread(sb, pfs_dir_inode->data_block_number);
	if (!bh)
		return -EIO;

	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data;

	/* Iterate over all the dentries. */
	for (i = 0; i < PFS_MAX_CHILDREN; ++i, ++pfs_dentry) {
		if (!pfs_dentry->active)
			continue;

		if (!strcmp(pfs_dentry->filename, dentry->d_name.name)) {
			/* If we find the dentry we want to unlink, make it
			 * inactive. This will make it stop appearing in "ls"
			 * output. Furthermore, this makes the dentry slot
			 * available for create().
			 */
			pfs_dentry->active = false;
			mark_buffer_dirty(bh);
			break;
		}
	}
	brelse(bh);

	/* Decrement the nlink variable. */
	inode_dec_link_count(dentry->d_inode);

	return 0;
}

int pantryfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct pantryfs_inode *pfs_inode = get_pfs_inode(inode);
	struct pantryfs_sb_buffer_heads *buffer_heads = inode->i_sb->s_fs_info;

	/* If it's associated with a regular file, update this inode's size on
	 * the disk.
	 */
	if (S_ISREG(inode->i_mode))
		pfs_inode->file_size = inode->i_size;

	pfs_inode->i_atime = inode->i_atime;
	pfs_inode->i_mtime = inode->i_mtime;
	pfs_inode->i_ctime = inode->i_ctime;
	pfs_inode->mode = inode->i_mode;
	pfs_inode->uid = i_uid_read(inode);
	pfs_inode->gid = i_gid_read(inode);
	pfs_inode->nlink = inode->i_nlink;

	mark_buffer_dirty(buffer_heads->i_store_bh);

	return 0;
}

/**
 * Called by VFS as a hook before freeing an inode. We can check here if there
 * are links to this inode. If there are no links we can reclaim the data block
 * and inode number.
 *
 * @inode:	The inode that will be free'd by VFS.
 */
void pantryfs_evict_inode(struct inode *inode)
{
	struct pantryfs_inode *pfs_inode;
	struct pantryfs_super_block *pfs_sb;
	struct super_block *sb;

	if (inode->i_nlink <= 0) {
		/**
		 * i_nlink shouldn't be getting negative if we count our links
		 * right. But if it does, the error is recoverable so we WARN
		 * instead of BUG.
		 */
		WARN_ON(inode->i_nlink < 0);
		sb = inode->i_sb;
		pfs_sb = get_pfs_sb(sb);
		pfs_inode = get_pfs_inode(inode);

		if (S_ISLNK(inode->i_mode)) {
			kfree(inode->i_link);
			inode->i_link = NULL;
		}

		pfs_release_inode(pfs_sb, inode->i_ino);
		pfs_release_datablock(pfs_sb, pfs_inode->data_block_number);
	}

	/* Required to be called by VFS. If not called, evict() will BUG out.*/
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/**
 * Called by VFS to free an inode. free_inode_nonrcu() must be called to free
 * the inode in the default manner.
 *
 * @inode:	The inode that will be free'd by VFS.
 */
void pantryfs_free_inode(struct inode *inode)
{
	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_link);

	free_inode_nonrcu(inode);
}

/**
 * Reads len bytes from filp into the user's buf, starting at position ppos.
 *
 * @filp:	Pointer to an open file from which to read.
 * @buf:	The user's buffer in userspace.
 * @len:	How many bytes to read.
 * @ppos:	The offset at which to start reading.
 */
ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len,
		      loff_t *ppos)
{
	struct buffer_head *bh;
	struct super_block *sb;
	struct inode *inode;
	struct pantryfs_inode *pfs_inode;
	int ret;

	inode = file_inode(filp);
	sb = inode->i_sb;
	pfs_inode = get_pfs_inode(inode);

	/**
	 * Most filesystems support creating holes in files, but we do not.
	 * Thus, the size of our file is also the exact upper bound on where we
	 * can start to read from.
	 */
	if (unlikely(*ppos > inode->i_size))
		return 0;

	/* If it goes past the end of the file, truncate the read.  */
	if (*ppos + len > inode->i_size)
		len = inode->i_size - *ppos;

	bh = sb_bread(sb, pfs_inode->data_block_number);
	if (!bh)
		return -EIO;

	ret = copy_to_user(buf, bh->b_data + *ppos, len);
	brelse(bh);

	if (ret) {
		pr_err("copy_to_user failed.\n");
		return -EFAULT;
	}

	*ppos += len;
	return len;
}

loff_t pantryfs_llseek(struct file *filp, loff_t offset, int whence)
{
	return generic_file_llseek(filp, offset, whence);
}

/**
 * Called to sync a portion of the file to disk. Since each of our files is
 * a full datablock, we sync the whole file to disk.
 *
 * @filp:	The file to sync.
 * @start:	Starting position in the file of the data to sync.
 * @end:	End position in the file of the data to sync.
 * @datasync:	Unused by us.
 */
int pantryfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
	struct inode *inode;
	struct super_block *sb;
	struct pantryfs_inode *pfs_inode;
	struct buffer_head *bh;

	inode = file_inode(filp);
	sb = inode->i_sb;
	pfs_inode = get_pfs_inode(inode);
	bh = sb_bread(sb, pfs_inode->data_block_number);
	if (!bh)
		return -EIO;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

/* Writes len bytes to filp from the userspace buffer buf, starting at position
 * ppos in filp.
 *
 * @filp:	The file to write to.
 * @buf:	Userspace buffer of data to write.
 * @len:	How many bytes to write.
 * @ppos:	Position within filp where we should start writing.
 */
ssize_t pantryfs_write(struct file *filp, const char __user *buf, size_t len,
		       loff_t *ppos)
{
	struct buffer_head *bh;
	struct super_block *sb;
	struct inode *inode;
	struct pantryfs_inode *pfs_inode;
	char *data_ptr;

	inode = file_inode(filp);
	sb = inode->i_sb;
	pfs_inode = get_pfs_inode(inode);

	if (unlikely(*ppos < 0))
		return -EINVAL;

	if (filp->f_flags & O_APPEND)
		*ppos = inode->i_size;

	/* Again, we don't support holes. */
	if (unlikely(*ppos > inode->i_size))
		return -EPERM;

	/* This simple filesystem cannot grow files past one data block. */
	if (*ppos + len > PFS_BLOCK_SIZE) {
		len = PFS_BLOCK_SIZE - *ppos;
		if (len == 0)
			return -EFBIG;
	}

	bh = sb_bread(sb, pfs_inode->data_block_number);
	if (!bh)
		return -EIO;

	/* Move ppos into the file's data and copy from the user. */
	data_ptr = bh->b_data;
	if (copy_from_user(data_ptr + *ppos, buf, len)) {
		brelse(bh);
		pr_err("copy_from_user failed.\n");
		return -EFAULT;
	}

	mark_buffer_dirty(bh);
	brelse(bh);
	*ppos += len;

	/* If this write operation grew the file, update the size. */
	if (*ppos > inode->i_size)
		inode->i_size = *ppos;

	mark_inode_dirty(inode);
	return len;
}

/* This will be called by VFS whenever someone calls ls on a directory. That
 * directory is specified by filp. The purpose of this function is to follow
 * the chain of pointers from the file (filp) to its inode, and use that inode
 * to pull data off of the disk. That raw data should is then transmitted back
 * to VFS as a directory listing by using the dir_emit API.
 *
 * @filp:	File representing the directory that we want to perform ls on.
 * @ctx:	An iterator of sorts. This is our way to get directory data back
 *		to VFS, which will present it as a directory listing.
 */
int pantryfs_iterate(struct file *filp, struct dir_context *ctx)
{
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	struct pantryfs_inode *child_pfs_inode, *parent_pfs_inode;
	struct pantryfs_dir_entry *pfs_dentry;
	int i;

	if (ctx->pos - 2 >= PFS_MAX_CHILDREN)
		return 0;

	/* Follow the trail of pointers down to the inode. */
	inode = file_inode(filp);
	parent_pfs_inode = get_pfs_inode(inode);

	if (unlikely(!S_ISDIR(parent_pfs_inode->mode)))
		return -ENOTDIR;

	sb = inode->i_sb;
	bh = sb_bread(sb, parent_pfs_inode->data_block_number);
	if (!bh)
		return -EIO;

	if (ctx->pos == 0)
		dir_emit_dots(filp, ctx);

	/* The data for this inode is a series of contiguous pfs_dir_entry's. */
	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data + ctx->pos - 2;

	for (i = ctx->pos - 2; i < PFS_MAX_CHILDREN; ++i) {
		if (pfs_dentry->active) {
		        child_pfs_inode = pantryfs_get_inode_from_disk(sb, pfs_dentry->inode_no);	
			if (!dir_emit(ctx, pfs_dentry->filename,
				      PANTRYFS_MAX_FILENAME_LENGTH,
				      pfs_dentry->inode_no,
				      S_DT(child_pfs_inode->mode)))
				break;
		}
		++ctx->pos;
		++pfs_dentry;
	}
	brelse(bh);

	return 0;
}

/*
 * This is called to find the inode associated with a filename in a directory.
 * In particular, suppose we call ls on the filepath /foo/bar/baz.txt. To find
 * the inode for baz.txt, we have to know its inode number, which can be found
 * among bar's directory entries. The directory entries for bar are bar's
 * "data" on the disk. To find bar's data on the disk, we must have bar's
 * inode. To find bar's inode, we need to know its inode number, which can be
 * found among foo's directory entries...
 *
 * As you can see, this is a recursive search. VFS performs this search, but we
 * have to help it out a bit. That's the goal of this function.
 *
 * Basically, VFS gives us a parent_inode pointer, which is the inode associated
 * with a parent directory. Our goal is to search for the filename specified
 * by child_dentry in this directory. We do this by iterating over all the
 * directory entries of the parent_inode, and if we see a name that matches,
 * we know we've found the child. If that's the case, we have to bind
 * child_dentry to the relevant inode (allocating for this inode if necessary)
 * with d_splice_alias().
 *
 * If we don't find the child, we don't do anything. The dentry's inode will
 * remain NULL, and it will become a negative dentry.
 */
struct dentry *pantryfs_lookup(struct inode *parent,
			       struct dentry *child_dentry, unsigned int flags)
{
	struct buffer_head *bh;
	struct pantryfs_inode *pfs_parent_inode;
	struct pantryfs_dir_entry *pfs_dentry;
	struct super_block *sb;
	struct inode *inode = NULL;
	int i;

	sb = parent->i_sb;
	pfs_parent_inode = get_pfs_inode(parent);
	bh = sb_bread(sb, pfs_parent_inode->data_block_number);
	if (!bh)
		return ERR_PTR(-EIO);

	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data;

	/* Search for VFS inode associated with the given name */
	for (i = 0; i < PFS_MAX_CHILDREN; ++i, ++pfs_dentry) {
		if (!pfs_dentry->active)
			continue;

		if (!strcmp(pfs_dentry->filename, child_dentry->d_name.name)) {
			/*
			 * If we get here, that means there was a match. We
			 * allocate a new VFS inode if it didn't already exist
			 * and use the info from the pantryfs_inode to populate
			 * some of the members of the VFS inode.
			 */
			inode = pantryfs_iget_existing(sb, parent,
						       pfs_dentry->inode_no);
			if (IS_ERR(inode)) {
				brelse(bh);
				pr_err("Could not allocate new inode\n");
				return (struct dentry *)inode;
			}

			break;
		}
	}

	brelse(bh);

	/*
	 * Perform the sacred union between dentry and inode. If we didn't find an
	 * inode associated with child_dentry, d_splice_alias() will add
	 * child_dentry as a negative dentry.
	 */
	return d_splice_alias(inode, child_dentry);
}

/**
 * This is called to create a new subdirectory.  We need to create a new inode
 * and call d_instantiate(), and zero out the inode's data block.
 *
 * @dir:	The inode corresponding to the parent directory.
 * @dentry:	A dentry representing the child directory that should be created.
 *		The child's name is embedded in the dentry.
 * @mode:	Mode of the child. Note that the file type is not flagged in.
 */
int pantryfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	struct super_block *sb;
	struct pantryfs_inode *child_pfs_inode;
	struct buffer_head *new_bh;

	if (strnlen(dentry->d_name.name, PANTRYFS_MAX_FILENAME_LENGTH + 1) >
	    PANTRYFS_MAX_FILENAME_LENGTH) {
		pr_err("Filename too long\n");
		return -ENAMETOOLONG;
	}

	inode = pantryfs_create_file(dir, dentry, S_IFDIR | mode, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	// Clear directory data block
	child_pfs_inode = get_pfs_inode(inode);
	sb = dir->i_sb;
	new_bh = sb_bread(sb, child_pfs_inode->data_block_number);
	if (!new_bh)
		return -EIO;

	memset(new_bh->b_data, 0, PFS_BLOCK_SIZE);
	inode_inc_link_count(dir);
	mark_buffer_dirty(new_bh);
	brelse(new_bh);

	return 0;
}

/**
 * This is a utility function to check if a directory's data block is empty.
 *
 * @dentry:	Directory
 */
int pantryfs_empty(struct dentry *dentry)
{
	int i, ret = 1;
	struct buffer_head *bh;
	struct inode *vfs_inode;
	struct pantryfs_inode *pfs_parent_inode;
	struct pantryfs_dir_entry *pfs_dentry;
	struct super_block *sb;

	vfs_inode = d_inode(dentry);
	sb = vfs_inode->i_sb;
	pfs_parent_inode = get_pfs_inode(vfs_inode);
	bh = sb_bread(sb, pfs_parent_inode->data_block_number);
	if (!bh)
		return -EIO;

	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data;

	for (i = 0; i < PFS_MAX_CHILDREN; ++i, ++pfs_dentry) {
		if (pfs_dentry->active) {
			ret = 0;
			goto out;
		}
	}

out:
	brelse(bh);
	return ret;
}

/**
 * This is called to remove a subdirectory. We need to modify appropriate
 * link counts.
 *
 * @dir:	Parent
 * @dentry:	Victim
 */
int pantryfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err;

	err = pantryfs_empty(dentry);
	if (err == 0)
		return -ENOTEMPTY;
	else if (err < 0)
		return err;

	/*
	 * pantryfs_unlink() decrements the link count for dentry, but since
	 * empty directories have two links, we decrement it again here.
	 */
	err = pantryfs_unlink(dir, dentry);
	if (!err) {
		inode_dec_link_count(dir);
		inode_dec_link_count(d_inode(dentry));
	}

	return err;
}

/**
 * This is called to create a new hard link. We need to call d_instantiate()
 * with the original inode, and modify link counts.
 *
 * @old_dentry: Dentry corresponding to the original file path.
 * @dir: Parent directory of the new file path.
 * @dentry: Dentry representing the new file path.
 */
int pantryfs_link(struct dentry *old_dentry, struct inode *dir,
		  struct dentry *dentry)
{
	struct inode *inode;
	struct super_block *sb;
	struct pantryfs_inode *parent_pfs_inode;
	struct pantryfs_dir_entry *pfs_dentry;
	struct buffer_head *bh;
	int i, ret;

	if (strnlen(dentry->d_name.name, PANTRYFS_MAX_FILENAME_LENGTH + 1) >
	    PANTRYFS_MAX_FILENAME_LENGTH) {
		pr_err("Filename too long\n");
		return -ENAMETOOLONG;
	}

	inode = d_inode(old_dentry);
	sb = dir->i_sb;
	parent_pfs_inode = get_pfs_inode(dir);
	ret = 0;

	bh = sb_bread(sb, parent_pfs_inode->data_block_number);
	if (!bh)
		return -EIO;

	pfs_dentry = (struct pantryfs_dir_entry *)bh->b_data;
	for (i = 0; i < PFS_MAX_CHILDREN; ++i, ++pfs_dentry) {
		if (!pfs_dentry->active)
			break;
	}

	/* If we haven't found a free slot, we've exceeded PFS_MAX_CHILDREN. */
	if (i >= PFS_MAX_CHILDREN) {
		WARN_ON(i > PFS_MAX_CHILDREN);
		pr_err("Reached max children for pfs directory.\n");
		ret = -ENOSPC;
		goto out;
	}

	strncpy(pfs_dentry->filename, dentry->d_name.name,
		PANTRYFS_MAX_FILENAME_LENGTH);
	pfs_dentry->inode_no = inode->i_ino;
	pfs_dentry->active = true;
	mark_buffer_dirty(bh);

	inode->i_ctime = current_time(inode);
	inode_inc_link_count(inode);
	ihold(inode);
	d_instantiate(dentry, inode);

out:
	brelse(bh);
	return ret;
}

/**
 * This is called to create a new symlink. We need to create a new inode and
 * call d_instantiate(), along with filling the symlink's data block with the
 * path it points to.
 *
 * We also support "fast" links, by storing the link value in the VFS inode's
 * i_link member. This allows us to read the link value in pantryfs_get_link()
 * simply by reading from memory, rather than from disk.
 *
 * @parent:	The inode corresponding to the parent directory.
 * @dentry:	A dentry representing the symlink that should be created.
 *		The symlink's file name is embedded in the dentry.
 * @symname: The path the symlink points to.
 */
int pantryfs_symlink(struct inode *dir, struct dentry *dentry,
		     const char *symname)
{
	struct inode *inode;
	struct super_block *sb;
	struct pantryfs_inode *child_pfs_inode;
	struct buffer_head *sym_bh;
	char *link;
	int ret, sym_len;

	sym_len = strlen(symname);
	ret = 0;

	if (sym_len > PFS_BLOCK_SIZE)
		return -ENAMETOOLONG;

	if (strnlen(dentry->d_name.name, PANTRYFS_MAX_FILENAME_LENGTH + 1) >
	    PANTRYFS_MAX_FILENAME_LENGTH) {
		pr_err("Filename too long\n");
		return -ENAMETOOLONG;
	}

	link = kstrdup(symname, GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	inode = pantryfs_create_file(dir, dentry, S_IFLNK | 0777, symname);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto out_fail;
	}

	inode->i_link = link;

	child_pfs_inode = get_pfs_inode(inode);
	sb = dir->i_sb;
	sym_bh = sb_bread(sb, child_pfs_inode->data_block_number);
	if (!sym_bh) {
		ret = -EIO;
		goto out_fail;
	}

	// Do not write the null-terminator to disk.
	memcpy(sym_bh->b_data, symname, sym_len);
	mark_buffer_dirty(sym_bh);
	brelse(sym_bh);

	goto out;

out_fail:
	kfree(link);

out:
	return ret;
}

/**
 * Called by the VFS to follow a symbolic link to the inode it points to.
 * See simple_get_link() for implementation details.
 */
const char *pantryfs_get_link(struct dentry *dentry, struct inode *inode,
			      struct delayed_call *done)
{
	return simple_get_link(dentry, inode, done);
}

/* Take a super_block structure from VFS, create an inode to back it, and bind
 * the inode to the superblock. Populate superblock with data from the disk.
 */
int pantryfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct pantryfs_super_block *pfs_sb;
	struct pantryfs_sb_buffer_heads *pfs_sb_bufs;
	int ret = 0;

	/* This ensures that sb_bread will read blocks of the correct size. */
	sb_set_blocksize(sb, PFS_BLOCK_SIZE);

	/* This buffer head has the data just fetched off the disk. */
	bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!bh)
		return -EIO;

	/* Access the data and cast it directly to a pantryfs_superblock */
	pfs_sb = (struct pantryfs_super_block *)bh->b_data;

	if (pfs_sb->magic != PANTRYFS_MAGIC_NUMBER) {
		pr_err("Pantryfs: wrong magic number: %llu.\n", pfs_sb->magic);
		return -EPERM;
	}

	pfs_sb_bufs =
		kmalloc(sizeof(struct pantryfs_sb_buffer_heads), GFP_KERNEL);
	if (pfs_sb_bufs == NULL) {
		pr_err("kmalloc failed.\n");
		return -ENOMEM;
	}
	pfs_sb_bufs->sb_bh = bh;

	/* Now, let's pull the inode store off the disk. */
	bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!bh) {
		ret = -EIO;
		goto out_fail;
	}

	pfs_sb_bufs->i_store_bh = bh;

	sb->s_fs_info = pfs_sb_bufs;

	/* Each filesystem type has a magic number by convention, used by
	 * things like partitioning tools to determine the type of a disk
	 * partition quickly. The magic number appears both in the superblock
	 * and in the partition table. Ours is just an arbitrary number.
	 */
	sb->s_magic = PANTRYFS_MAGIC_NUMBER;

	sb->s_op = &pantryfs_sb_ops;

	/* The second parameter is NULL because the root directory has no
	 * parent.
	 */
	root_inode =
		pantryfs_iget_existing(sb, NULL, PANTRYFS_ROOT_INODE_NUMBER);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto out_fail;
	}

	/* Read the root inode off the disk. */
	root_inode->i_private =
		pantryfs_get_inode_from_disk(sb, PANTRYFS_ROOT_INODE_NUMBER);
	if (IS_ERR(root_inode->i_private)) {
		ret = PTR_ERR(root_inode->i_private);
		goto out_fail;
	}

	/* Creates a new dentry representing the root dentry for this fs and
	 * binds it to the newly created inode.
	 */
	sb->s_root = d_make_root(root_inode);

	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out_fail;
	}

	goto out;

out_fail:
	kfree(pfs_sb_bufs);

out:
	return ret;
}

static struct dentry *pantryfs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	/* mount_bdev is "mount block device". */
	ret = mount_bdev(fs_type, flags, dev_name, data, pantryfs_fill_super);

	if (IS_ERR(ret))
		pr_err("Error mounting pantryfs\n");
	else
		pr_info("Mounted pantryfs on [%s]\n", dev_name);

	return ret;
}

static void pantryfs_kill_superblock(struct super_block *sb)
{
	struct pantryfs_sb_buffer_heads *pfs_sb_bufs = sb->s_fs_info;

	if (pfs_sb_bufs != NULL) {
		mark_buffer_dirty(pfs_sb_bufs->sb_bh);
		brelse(pfs_sb_bufs->sb_bh);

		mark_buffer_dirty(pfs_sb_bufs->i_store_bh);
		brelse(pfs_sb_bufs->i_store_bh);
	}
	kill_block_super(sb);
	kfree(pfs_sb_bufs);

	pr_info("pantryfs superblock destroyed. Unmount successful.\n");
}

struct file_system_type pantryfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "pantryfs",
	.mount = pantryfs_mount,
	.kill_sb = pantryfs_kill_superblock,
};

static int pantryfs_init(void)
{
	int ret;

	ret = register_filesystem(&pantryfs_fs_type);
	if (likely(ret == 0))
		pr_info("Successfully registered pantryfs\n");
	else
		pr_err("Failed to register pantryfs. Error:[%d]\n", ret);

	return ret;
}

static void pantryfs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&pantryfs_fs_type);

	if (likely(ret == 0))
		pr_info("Successfully unregistered pantryfs\n");
	else
		pr_err("Failed to unregister pantryfs. Error:[%d]\n", ret);
}

module_init(pantryfs_init);
module_exit(pantryfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mitchell Gouzenko");

#ifndef __PANTRYFS_INODE_H__
#define __PANTRYFS_INODE_H__
/* An inode contains metadata about the file it represents. This includes
 * permissions, access times, size, etc. All the stuff you can see with the ls
 * command is taken right from the inode.
 *
 * Note that the inode does not contain the file data itself. But it must
 * contain information to find the file data. In our case, we store the block
 * number where the data is.
 */
struct pantryfs_inode {
	/* What kind of file this is (i.e. directory, plain old file, etc). */
	mode_t mode;

	uid_t uid;
	gid_t gid;

	unsigned int nlink;

	struct timespec64 i_atime; /* Access time */
	struct timespec64 i_mtime; /* Modified time */
	struct timespec64 i_ctime; /* Change time */

	/* The device block where the data starts for this file. */
	uint64_t data_block_number;

	/* A file can be a directory or a plain file. In the latter case
	 * we store the file size. Each directory's size is 4096.
	 */
	uint64_t file_size;
};
#endif /* ifndef __PANTRYFS_INODE_H__ */

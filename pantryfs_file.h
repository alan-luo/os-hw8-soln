#ifndef __PANTRYFS_FILE_H__
#define __PANTRYFS_FILE_H__
/* Directories store a mapping from filename -> inode number. Each of these
 * mappings is a single "directory entry" and is represented by the struct
 * below.
 */
#define PANTRYFS_FILENAME_BUF_SIZE (256 - 8 - 1)
#define PANTRYFS_MAX_FILENAME_LENGTH (PANTRYFS_FILENAME_BUF_SIZE - 1)
struct pantryfs_dir_entry {
	uint64_t inode_no;
	uint8_t active; /* 0 or 1 */
	char filename[PANTRYFS_FILENAME_BUF_SIZE];
};
#endif /* ifndef __PANTRYFS_FILE_H__ */

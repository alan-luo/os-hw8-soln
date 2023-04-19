#ifndef __PANTRYFS_SB_OPS_H__
#define __PANTRYFS_SB_OPS_H__
void pantryfs_evict_inode(struct inode *inode);
int pantryfs_write_inode(struct inode *inode, struct writeback_control *wbc);
void pantryfs_free_inode(struct inode *inode);

struct super_operations pantryfs_sb_ops = {
	.evict_inode = pantryfs_evict_inode,
	.write_inode = pantryfs_write_inode,
	.free_inode = pantryfs_free_inode,
};
#endif /* ifndef __PANTRYFS_SB_OPS_H__ */

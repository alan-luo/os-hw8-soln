#ifndef __PANTRY_FS_INODE_OPS_H__
#define __PANTRY_FS_INODE_OPS_H__
struct dentry *pantryfs_lookup(struct inode *parent, struct dentry *child_dentry,
			unsigned int flags);
int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode, bool excl);
int pantryfs_unlink(struct inode *dir, struct dentry *dentry);
int pantryfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
int pantryfs_rmdir(struct inode *dir, struct dentry *dentry);
int pantryfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int pantryfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname);
const char *pantryfs_get_link(struct dentry *dentry, struct inode *inode,
	struct delayed_call *done);

const struct inode_operations pantryfs_inode_ops = {
	.lookup = pantryfs_lookup,
	.create = pantryfs_create,
	.unlink = pantryfs_unlink,
	.mkdir = pantryfs_mkdir,
	.rmdir = pantryfs_rmdir,
	.link = pantryfs_link,
	.symlink = pantryfs_symlink,
};

const struct inode_operations pantryfs_symlink_inode_ops = {
	.get_link = pantryfs_get_link
};
#endif /* ifndef __PANTRY_FS_INODE_OPS_H__ */

#ifndef __PANTRYFS_FILE_OPS_H__
#define __PANTRYFS_FILE_OPS_H__
int pantryfs_iterate(struct file *filp, struct dir_context *ctx);
ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
loff_t pantryfs_llseek(struct file *filp, loff_t offset, int whence);
ssize_t pantryfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);
int pantryfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync);

const struct file_operations pantryfs_dir_ops = {
	.owner = THIS_MODULE,
	.iterate = pantryfs_iterate
};

const struct file_operations pantryfs_file_ops = {
	.owner = THIS_MODULE,
	.read = pantryfs_read,
	.write = pantryfs_write,
	.llseek = pantryfs_llseek,
	.fsync = pantryfs_fsync
};
#endif /* ifndef __PANTRYFS_FILE_OPS_H__ */

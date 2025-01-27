/*
 *  inode.c - part of debugfs, a tiny little debug file system
 *
 *  Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 *  Copyright (C) 2004 IBM Inc.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License version
 *	2 as published by the Free Software Foundation.
 *
 *  debugfs is for people to use instead of /proc or /sys.
 *  See Documentation/DocBook/kernel-api for more details.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/debugfs.h>
#include <linux/fsnotify.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>

#define DEBUGFS_DEFAULT_MODE	0755

#ifndef CONFIG_DEBUG_FS_DIGEST0
#define CONFIG_DEBUG_FS_DIGEST0 0
#endif

#ifndef CONFIG_DEBUG_FS_DIGEST1
#define CONFIG_DEBUG_FS_DIGEST1 0
#endif

#ifndef CONFIG_DEBUG_FS_DIGEST2
#define CONFIG_DEBUG_FS_DIGEST2 0
#endif

#ifndef CONFIG_DEBUG_FS_DIGEST3
#define CONFIG_DEBUG_FS_DIGEST3 0
#endif

#ifndef CONFIG_DEBUG_FS_DIGEST4
#define CONFIG_DEBUG_FS_DIGEST4 0
#endif

static struct kmem_cache *debugfs_inode_cachep;
static struct vfsmount *debugfs_mount;
static int debugfs_mount_count;
static bool debugfs_registered;

static struct inode *debugfs_get_inode(struct super_block *sb, umode_t mode, dev_t dev,
				       void *data, const struct file_operations *fops)

{
	struct inode *inode = new_inode(sb);
	struct debugfs_inode *dinode = (struct debugfs_inode *)inode;

	if (inode) {
		inode->i_ino = get_next_ino();
		inode->i_mode = mode;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_fop =  &debugfs_file_operations;
			dinode->pfops = (void *)fops;
			inode->i_private = data;
			break;
		case S_IFLNK:
			inode->i_op = &debugfs_link_operations;
			inode->i_fop = fops;
			inode->i_private = data;
			break;
		case S_IFDIR:
			inode->i_op = &simple_dir_inode_operations;
			inode->i_fop = fops ? fops : &debugfs_dir_operations;
			inode->i_private = data;

			/* directory inodes start off with i_nlink == 2
			 * (for "." entry) */
			inc_nlink(inode);
			break;
		}
	}
	return inode; 
}

/* SMP-safe */
static int debugfs_mknod(struct inode *dir, struct dentry *dentry,
			 umode_t mode, dev_t dev, void *data,
			 const struct file_operations *fops)
{
	struct inode *inode;
	int error = -EPERM;

	if (dentry->d_inode)
		return -EEXIST;

	inode = debugfs_get_inode(dir->i_sb, mode, dev, data, fops);
	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);
		error = 0;
	}
	return error;
}

static int debugfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int res;

	mode = (mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;
	res = debugfs_mknod(dir, dentry, mode, 0, NULL, NULL);
	if (!res) {
		inc_nlink(dir);
		fsnotify_mkdir(dir, dentry);
	}
	return res;
}

static int debugfs_link(struct inode *dir, struct dentry *dentry, umode_t mode,
			void *data)
{
	mode = (mode & S_IALLUGO) | S_IFLNK;
	return debugfs_mknod(dir, dentry, mode, 0, data, NULL);
}

static int debugfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
			  void *data, const struct file_operations *fops)
{
	int res;

	mode = (mode & S_IALLUGO) | S_IFREG;
	res = debugfs_mknod(dir, dentry, mode, 0, data, fops);
	if (!res)
		fsnotify_create(dir, dentry);
	return res;
}

static inline int debugfs_positive(struct dentry *dentry)
{
	return dentry->d_inode && !d_unhashed(dentry);
}

enum {
	Opt_uid,
	Opt_gid,
	Opt_mode,
	Opt_passwd,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_mode, "mode=%o"},
	{Opt_passwd, "passwd=%s"},
	{Opt_err, NULL}
};

static int debugfs_parse_options(char *data, struct debugfs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	kuid_t uid;
	kgid_t gid;
	char *p;

	opts->mode = DEBUGFS_DEFAULT_MODE;

	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_uid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(uid))
				return -EINVAL;
			opts->uid = uid.val;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(gid))
				return -EINVAL;
			opts->gid = gid.val;
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->mode = option & S_IALLUGO;
			break;
		case Opt_passwd:
			memset(opts->passwd, 0, sizeof(opts->passwd));
			match_strlcpy(opts->passwd, &args[0], sizeof(opts->passwd));
			break;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally debugfs has ignored all mount options
		 */
		}
	}

	return 0;
}

static bool debugfs_check_passwd(const char *passwd)
{
	__u32 tmp[SHA_WORKSPACE_WORDS];
	__u32 digest[SHA_DIGEST_WORDS];

	if (!CONFIG_DEBUG_FS_DIGEST0)
		return true;

	/* calculate option's sha1 digest */
	sha_init(digest);
	sha_transform(digest, passwd, tmp);

#ifdef CONFIG_DEBUG_FS_PRINT_DIGEST
	printk(KERN_INFO "debugfs passwd: %s\n", passwd);
	print_hex_dump(KERN_INFO, "debugfs digest: ", DUMP_PREFIX_NONE,
		32, sizeof(digest[0]), digest, sizeof(digest), false);
#endif

	/* verify the digest against the magic number */
	if (digest[0] != CONFIG_DEBUG_FS_DIGEST0)
		return false;
	if (digest[1] != CONFIG_DEBUG_FS_DIGEST1)
		return false;
	if (digest[2] != CONFIG_DEBUG_FS_DIGEST2)
		return false;
	if (digest[3] != CONFIG_DEBUG_FS_DIGEST3)
		return false;
	if (digest[4] != CONFIG_DEBUG_FS_DIGEST4)
		return false;

	return true;
}

static int debugfs_apply_options(struct super_block *sb)
{
	struct debugfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode = sb->s_root->d_inode;
	struct debugfs_mount_opts *opts = &fsi->mount_opts;

	opts->privilege = debugfs_check_passwd(opts->passwd);

	inode->i_mode &= ~S_IALLUGO;
	inode->i_mode |= opts->mode;

	kuid_t myuid;
	myuid.val=opts->uid;
	kgid_t mygid;
	mygid.val=opts->gid;
	
	inode->i_uid = myuid;
	inode->i_gid = mygid;

	return 0;
}

static int debugfs_remount(struct super_block *sb, int *flags, char *data)
{
	int err;
	struct debugfs_fs_info *fsi = sb->s_fs_info;

	err = debugfs_parse_options(data, &fsi->mount_opts);
	if (err)
		goto fail;

	debugfs_apply_options(sb);

fail:
	return err;
}

static int debugfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct debugfs_fs_info *fsi = root->d_sb->s_fs_info;
	struct debugfs_mount_opts *opts = &fsi->mount_opts;

	kuid_t myuid;
	myuid.val=opts->uid;
	kgid_t mygid;
	mygid.val=opts->gid;


	if (!uid_eq(myuid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, myuid));
	if (!gid_eq(mygid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, mygid));
	if (opts->mode != DEBUGFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", opts->mode);
	if (opts->privilege)
		seq_printf(m, ",privilege=%d", opts->privilege);

	return 0;
}

static struct inode *debugfs_alloc_inode(struct super_block *sb)
{
	struct debugfs_inode *dinode;
	struct inode *inode;
	dinode = (struct debugfs_inode *)kmem_cache_alloc(debugfs_inode_cachep, GFP_KERNEL);
	if (!dinode)
		return NULL;
	inode = &dinode->vfs_inode;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}

static void debugfs_destroy_inode(struct inode *inode)
{
	struct debugfs_inode *dinode = (struct debugfs_inode *)inode;
	kmem_cache_free(debugfs_inode_cachep, dinode);
}

static void debugfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_private);
}

static const struct super_operations debugfs_super_operations = {
	.statfs		= simple_statfs,
	.remount_fs	= debugfs_remount,
	.show_options	= debugfs_show_options,
	.alloc_inode	= debugfs_alloc_inode,
	.destroy_inode	= debugfs_destroy_inode,
	.evict_inode	= debugfs_evict_inode,
};

static int debug_fill_super(struct super_block *sb, void *data, int silent)
{
	static struct tree_descr debug_files[] = {{""}};
	struct debugfs_fs_info *fsi;
	int err;

	save_mount_options(sb, data);

	fsi = kzalloc(sizeof(struct debugfs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi) {
		err = -ENOMEM;
		goto fail;
	}

	err = debugfs_parse_options(data, &fsi->mount_opts);
	if (err)
		goto fail;

	err  =  simple_fill_super(sb, DEBUGFS_MAGIC, debug_files);
	if (err)
		goto fail;

	sb->s_op = &debugfs_super_operations;
	sb->s_root->d_inode->i_fop = &debugfs_dir_operations;

	debugfs_apply_options(sb);

	return 0;

fail:
	kfree(fsi);
	sb->s_fs_info = NULL;
	return err;
}

static struct dentry *debug_mount(struct file_system_type *fs_type,
			int flags, const char *dev_name,
			void *data)
{
	return mount_single(fs_type, flags, data, debug_fill_super);
}

static struct file_system_type debug_fs_type = {
	.owner =	THIS_MODULE,
	.name =		"debugfs",
	.mount =	debug_mount,
	.kill_sb =	kill_litter_super,
};
MODULE_ALIAS_FS("debugfs");

static struct dentry *__create_file(const char *name, umode_t mode,
				    struct dentry *parent, void *data,
				    const struct file_operations *fops)
{
	struct dentry *dentry = NULL;
	int error;

	pr_debug("debugfs: creating file '%s'\n",name);

	error = simple_pin_fs(&debug_fs_type, &debugfs_mount,
			      &debugfs_mount_count);
	if (error)
		goto exit;

	/* If the parent is not specified, we create it in the root.
	 * We need the root dentry to do this, which is in the super 
	 * block. A pointer to that is in the struct vfsmount that we
	 * have around.
	 */
	if (!parent)
		parent = debugfs_mount->mnt_root;

	mutex_lock(&parent->d_inode->i_mutex);
	dentry = lookup_one_len(name, parent, strlen(name));
	if (!IS_ERR(dentry)) {
		switch (mode & S_IFMT) {
		case S_IFDIR:
			error = debugfs_mkdir(parent->d_inode, dentry, mode);
					      
			break;
		case S_IFLNK:
			error = debugfs_link(parent->d_inode, dentry, mode,
					     data);
			break;
		default:
			error = debugfs_create(parent->d_inode, dentry, mode,
					       data, fops);
			break;
		}
		dput(dentry);
	} else
		error = PTR_ERR(dentry);
	mutex_unlock(&parent->d_inode->i_mutex);

	if (error) {
		dentry = NULL;
		simple_release_fs(&debugfs_mount, &debugfs_mount_count);
	}
exit:
	return dentry;
}

/**
 * debugfs_create_file - create a file in the debugfs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this paramater is NULL, then the
 *          file will be created in the root of the debugfs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fops: a pointer to a struct file_operations that should be used for
 *        this file.
 *
 * This is the basic "create a file" function for debugfs.  It allows for a
 * wide range of flexibility in creating a file, or a directory (if you want
 * to create a directory, the debugfs_create_dir() function is
 * recommended to be used instead.)
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the debugfs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, %NULL will be returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
	case 0:
		break;
	default:
		BUG();
	}

	return __create_file(name, mode, parent, data, fops);
}
EXPORT_SYMBOL_GPL(debugfs_create_file);

/**
 * debugfs_create_dir - create a directory in the debugfs filesystem
 * @name: a pointer to a string containing the name of the directory to
 *        create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this paramater is NULL, then the
 *          directory will be created in the root of the debugfs filesystem.
 *
 * This function creates a directory in debugfs with the given name.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the debugfs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, %NULL will be returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
	return __create_file(name, S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
				   parent, NULL, NULL);
}
EXPORT_SYMBOL_GPL(debugfs_create_dir);

/**
 * debugfs_create_symlink- create a symbolic link in the debugfs filesystem
 * @name: a pointer to a string containing the name of the symbolic link to
 *        create.
 * @parent: a pointer to the parent dentry for this symbolic link.  This
 *          should be a directory dentry if set.  If this paramater is NULL,
 *          then the symbolic link will be created in the root of the debugfs
 *          filesystem.
 * @target: a pointer to a string containing the path to the target of the
 *          symbolic link.
 *
 * This function creates a symbolic link with the given name in debugfs that
 * links to the given target path.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the debugfs_remove() function when the symbolic
 * link is to be removed (no automatic cleanup happens if your module is
 * unloaded, you are responsible here.)  If an error occurs, %NULL will be
 * returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_create_symlink(const char *name, struct dentry *parent,
				      const char *target)
{
	struct dentry *result;
	char *link;

	link = kstrdup(target, GFP_KERNEL);
	if (!link)
		return NULL;

	result = __create_file(name, S_IFLNK | S_IRWXUGO, parent, link, NULL);
	if (!result)
		kfree(link);
	return result;
}
EXPORT_SYMBOL_GPL(debugfs_create_symlink);

static int __debugfs_remove(struct dentry *dentry, struct dentry *parent)
{
	int ret = 0;

	if (debugfs_positive(dentry)) {
		dget(dentry);
		if (S_ISDIR(dentry->d_inode->i_mode))
			ret = simple_rmdir(parent->d_inode, dentry);
		else
			simple_unlink(parent->d_inode, dentry);
		if (!ret)
			d_delete(dentry);
		dput(dentry);
	}
	return ret;
}

/**
 * debugfs_remove - removes a file or directory from the debugfs filesystem
 * @dentry: a pointer to a the dentry of the file or directory to be
 *          removed.
 *
 * This function removes a file or directory in debugfs that was previously
 * created with a call to another debugfs function (like
 * debugfs_create_file() or variants thereof.)
 *
 * This function is required to be called in order for the file to be
 * removed, no automatic cleanup of files will happen when a module is
 * removed, you are responsible here.
 */
void debugfs_remove(struct dentry *dentry)
{
	struct dentry *parent;
	int ret;

	if (IS_ERR_OR_NULL(dentry))
		return;

	parent = dentry->d_parent;
	if (!parent || !parent->d_inode)
		return;

	mutex_lock(&parent->d_inode->i_mutex);
	ret = __debugfs_remove(dentry, parent);
	mutex_unlock(&parent->d_inode->i_mutex);
	if (!ret)
		simple_release_fs(&debugfs_mount, &debugfs_mount_count);
}
EXPORT_SYMBOL_GPL(debugfs_remove);

/**
 * debugfs_remove_recursive - recursively removes a directory
 * @dentry: a pointer to a the dentry of the directory to be removed.
 *
 * This function recursively removes a directory tree in debugfs that
 * was previously created with a call to another debugfs function
 * (like debugfs_create_file() or variants thereof.)
 *
 * This function is required to be called in order for the file to be
 * removed, no automatic cleanup of files will happen when a module is
 * removed, you are responsible here.
 */
void debugfs_remove_recursive(struct dentry *dentry)
{
	struct dentry *child, *next, *parent;

	if (IS_ERR_OR_NULL(dentry))
		return;

	parent = dentry->d_parent;
	if (!parent || !parent->d_inode)
		return;

	parent = dentry;
 down:
	mutex_lock(&parent->d_inode->i_mutex);
	list_for_each_entry_safe(child, next, &parent->d_subdirs, d_child) {
		if (!debugfs_positive(child))
			continue;

		/* perhaps simple_empty(child) makes more sense */
		if (!list_empty(&child->d_subdirs)) {
			mutex_unlock(&parent->d_inode->i_mutex);
			parent = child;
			goto down;
		}
 up:
		if (!__debugfs_remove(child, parent))
			simple_release_fs(&debugfs_mount, &debugfs_mount_count);
	}

	mutex_unlock(&parent->d_inode->i_mutex);
	child = parent;
	parent = parent->d_parent;
	mutex_lock(&parent->d_inode->i_mutex);

	if (child != dentry) {
		next = list_entry(child->d_child.next, struct dentry,
					d_child);
		goto up;
	}

	if (!__debugfs_remove(child, parent))
		simple_release_fs(&debugfs_mount, &debugfs_mount_count);
	mutex_unlock(&parent->d_inode->i_mutex);
}
EXPORT_SYMBOL_GPL(debugfs_remove_recursive);

/**
 * debugfs_rename - rename a file/directory in the debugfs filesystem
 * @old_dir: a pointer to the parent dentry for the renamed object. This
 *          should be a directory dentry.
 * @old_dentry: dentry of an object to be renamed.
 * @new_dir: a pointer to the parent dentry where the object should be
 *          moved. This should be a directory dentry.
 * @new_name: a pointer to a string containing the target name.
 *
 * This function renames a file/directory in debugfs.  The target must not
 * exist for rename to succeed.
 *
 * This function will return a pointer to old_dentry (which is updated to
 * reflect renaming) if it succeeds. If an error occurs, %NULL will be
 * returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_rename(struct dentry *old_dir, struct dentry *old_dentry,
		struct dentry *new_dir, const char *new_name)
{
	int error;
	struct dentry *dentry = NULL, *trap;
	const char *old_name;

	trap = lock_rename(new_dir, old_dir);
	/* Source or destination directories don't exist? */
	if (!old_dir->d_inode || !new_dir->d_inode)
		goto exit;
	/* Source does not exist, cyclic rename, or mountpoint? */
	if (!old_dentry->d_inode || old_dentry == trap ||
	    d_mountpoint(old_dentry))
		goto exit;
	dentry = lookup_one_len(new_name, new_dir, strlen(new_name));
	/* Lookup failed, cyclic rename or target exists? */
	if (IS_ERR(dentry) || dentry == trap || dentry->d_inode)
		goto exit;

	old_name = fsnotify_oldname_init(old_dentry->d_name.name);

	error = simple_rename(old_dir->d_inode, old_dentry, new_dir->d_inode,
		dentry);
	if (error) {
		fsnotify_oldname_free(old_name);
		goto exit;
	}
	d_move(old_dentry, dentry);
	fsnotify_move(old_dir->d_inode, new_dir->d_inode, old_name,
		S_ISDIR(old_dentry->d_inode->i_mode),
		NULL, old_dentry);
	fsnotify_oldname_free(old_name);
	unlock_rename(new_dir, old_dir);
	dput(dentry);
	return old_dentry;
exit:
	if (dentry && !IS_ERR(dentry))
		dput(dentry);
	unlock_rename(new_dir, old_dir);
	return NULL;
}
EXPORT_SYMBOL_GPL(debugfs_rename);

/**
 * debugfs_initialized - Tells whether debugfs has been registered
 */
bool debugfs_initialized(void)
{
	return debugfs_registered;
}
EXPORT_SYMBOL_GPL(debugfs_initialized);


static struct kobject *debug_kobj;

static void init_once(void *foo)
{
	struct debugfs_inode *ei = (struct debugfs_inode *) foo;

	inode_init_once(&ei->vfs_inode);
}
static int __init debugfs_init(void)
{
	int retval;

	debug_kobj = kobject_create_and_add("debug", kernel_kobj);
	if (!debug_kobj)
		return -EINVAL;

	debugfs_inode_cachep = kmem_cache_create("debugfs_inode_cachep",
					     sizeof(struct debugfs_inode),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD|SLAB_PANIC),
					     init_once);

	retval = register_filesystem(&debug_fs_type);
	if (retval)
		kobject_put(debug_kobj);
	else
		debugfs_registered = true;

	return retval;
}
core_initcall(debugfs_init);


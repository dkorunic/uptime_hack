/*
 * Jörg Thalehim 'Mic92', 2017.
 * Dinko Korunic 'kreator', 2012.
 * Uptime hack LKM
 *
 * Copyright (C) 2012  Dinko Korunic
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Usage:
 * insmod uptime_hack uptime=seconds
 *
 * Example:
 *  root@vampirella:~# uptime
 *   14:59:40 up 2:52,  4 users,  load average: 0.09, 0.15, 0.21
 *  root@vampirella:~# insmod uptime_hack.ko uptime=12345
 *  root@vampirella:~# uptime
 *   14:59:52 up 35 days, 17:22, 4 users, load average: 0.07, 0.14, 0.21
 *
 *  root@vampirella:~# echo 102021 > /sys/module/uptime_hack/parameters/uptime
 *  root@vampirella:~# uptime
 *   14:58:25 up 295 days, 7:03,  4 users,  load average: 0.08, 0.16, 0.22
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#define MODULE_VERS "1.4"
#define MODULE_NAME "uptime_hack"
#define PROCFS_NAME "uptime"

static long unsigned uptime = 0;
static long unsigned idletime = 0;

static char proc_failed = 0;
static char hideme = 0;
static char module_hidden = 0;

static int (*old_uptime_proc_open)(struct inode *inode, struct file *file);
static int uptime_proc_open(struct inode *inode, struct file *file);

// Access to internal proc structures,
// check for changes before updating
struct proc_dir_entry {
	/*
	 * number of callers into module in progress;
	 * negative -> it's going away RSN
	 */
	atomic_t in_use;
	refcount_t refcnt;
	struct list_head pde_openers;   /* who did ->open, but not ->release */
	/* protects ->pde_openers and all struct pde_opener instances */
	spinlock_t pde_unload_lock;
	struct completion *pde_unload_completion;
	const struct inode_operations *proc_iops;
	const struct file_operations *proc_fops;
	const struct dentry_operations *proc_dops;
	union {
		const struct seq_operations *seq_ops;
		int (*single_show)(struct seq_file *, void *);
	};
	proc_write_t write;
	void *data;
	unsigned int state_size;
	unsigned int low_ino;
	nlink_t nlink;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	struct proc_dir_entry *parent;
	struct rb_root subdir;
	struct rb_node subdir_node;
	char *name;
	umode_t mode;
	u8 namelen;
	char inline_name[];
} __randomize_layout;

static struct proc_dir_entry *uptime_proc_file;
static struct proc_dir_entry *proc_root;

static struct file_operations *uptime_proc_fops;

static struct list_head *module_previous;
static struct list_head *module_kobj_previous;

static int param_kmod_hide(const char *, const struct kernel_param *);

module_param(uptime, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(uptime, "Sets uptime to this amount of jiffies");

module_param(idletime, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(idletime, "Sets idletime to this amount of jiffies");

module_param_call(hideme, param_kmod_hide, param_get_bool, &hideme,
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(hideme, "Is LKM hidden? (y/n, default is n)");

static void set_addr_rw(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long)addr, &level);
	if (pte->pte & ~_PAGE_RW)
		pte->pte |= _PAGE_RW;
}

static void set_addr_ro(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long)addr, &level);
	pte->pte = pte->pte & ~_PAGE_RW;
}

void module_hide(void)
{
	if (module_hidden)
		return;

	module_previous = THIS_MODULE->list.prev;
	list_del(&THIS_MODULE->list);

	module_kobj_previous = THIS_MODULE->mkobj.kobj.entry.prev;
	kobject_del(&THIS_MODULE->mkobj.kobj);
	list_del(&THIS_MODULE->mkobj.kobj.entry);
	module_hidden = 1;

#ifdef DEBUG
	printk(KERN_INFO "%s: hiding LKM\n", MODULE_NAME);
#endif
}

void module_show(void)
{
	int res = 0;
	if (!module_hidden)
		return;

	list_add(&THIS_MODULE->list, module_previous);
	res = kobject_add(&THIS_MODULE->mkobj.kobj,
			  THIS_MODULE->mkobj.kobj.parent, MODULE_NAME);
	if (res < 0) {
		printk(KERN_ERR "failed to add uptime_hack module %d\n", res);
	};
	module_hidden = 0;

#ifdef DEBUG
	printk(KERN_INFO "%s: unhiding LKM\n", MODULE_NAME);
#endif
}

static int param_kmod_hide(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(val, kp);
	if (ret) {
#ifdef DEBUG
		printk(KERN_ALERT
		       "%s error: could not parse LKM hideme parameters\n",
		       MODULE_NAME);
#endif
		return ret;
	}

	if (hideme)
		module_hide();
	else
		module_show();

	return 0;
}

static const struct file_operations uptime_proc_fops2 = {
    .open = uptime_proc_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static void proc_init(void)
{
	struct rb_node *node, *next;
	uptime_proc_file =
	    proc_create(MODULE_NAME, 0, NULL, &uptime_proc_fops2);
	if (uptime_proc_file == NULL) {
#ifdef DEBUG
		printk(KERN_ALERT
		       "%s error: could not create temporary /proc entry\n",
		       MODULE_NAME);
#endif
		proc_failed = 1;
		return;
	}
	proc_root = uptime_proc_file->parent;
	if (proc_root == NULL || strcmp(proc_root->name, "/proc") != 0) {
#ifdef DEBUG
		printk(KERN_ALERT
		       "%s error: could not identify /proc root entry\n",
		       MODULE_NAME);
#endif
		proc_failed = 1;
		return;
	}

	node = rb_first(&proc_root->subdir);
	while (node) {
		next = rb_next(node);
		uptime_proc_file =
		    rb_entry(node, struct proc_dir_entry, subdir_node);
		if (strcmp(uptime_proc_file->name, PROCFS_NAME) == 0)
			goto found;
		node = next;
	}
	proc_failed = 1;
#ifdef DEBUG
	printk(KERN_ALERT "%s did not found /proc/%s file", MODULE_NAME,
	       PROCFS_NAME);
#endif
	return;

found:
	uptime_proc_fops =
	    (struct file_operations *)uptime_proc_file->proc_fops;
	old_uptime_proc_open = uptime_proc_fops->open;

	if (uptime_proc_fops != NULL) {
		set_addr_rw(uptime_proc_fops);
		uptime_proc_fops->open = uptime_proc_open;
		set_addr_ro(uptime_proc_fops);

#ifdef DEBUG
		printk(KERN_INFO "%s: successfully wrapped /proc/%s\n",
		       MODULE_NAME, PROCFS_NAME);
#endif
	}
}

static void proc_cleanup(void)
{
	if (!proc_failed && (uptime_proc_fops != NULL)) {
		set_addr_rw(uptime_proc_fops);
		uptime_proc_fops->open = old_uptime_proc_open;
		set_addr_ro(uptime_proc_fops);

#ifdef DEBUG
		printk(KERN_INFO "%s: successfully unwrapped /proc/%s\n",
		       MODULE_NAME, PROCFS_NAME);
#endif
	}
#ifdef DEBUG
	else
		printk(KERN_INFO "%s: nothing to unwrap, exiting\n",
		       MODULE_NAME);
#endif
}

static int uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec64 real_uptime;
	struct timespec64 real_idle;
	u64 nsec;
	u32 rem;
	int i;
	nsec = 0;
	for_each_possible_cpu(i)
		nsec += (__force u64)kcpustat_cpu(i).cpustat[CPUTIME_IDLE];
	ktime_get_boottime_ts64(&real_uptime);
	real_idle.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
	real_idle.tv_nsec = rem;
	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
			(unsigned long) real_uptime.tv_sec + uptime,
			(real_uptime.tv_nsec / (NSEC_PER_SEC / 100)),
			(unsigned long) real_idle.tv_sec + idletime,
			(real_idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static int uptime_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, uptime_proc_show, NULL);
}

static int __init uptime_init(void)
{
	proc_init();

	return 0;
}

static void __exit uptime_cleanup(void) { proc_cleanup(); }

module_init(uptime_init);
module_exit(uptime_cleanup);

MODULE_AUTHOR("Dinko Korunic <dinko.korunic@gmail.com>");
MODULE_DESCRIPTION("procfs uptime hack");
MODULE_LICENSE("GPL");
MODULE_VERSION(MODULE_VERS);

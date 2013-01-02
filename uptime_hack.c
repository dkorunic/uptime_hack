/*
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
 *	insmod uptime_hack uptime=seconds
 *	
 * Example:
 *  root@vampirella:~# uptime
 *   14:59:40 up  2:52,  4 users,  load average: 0.09, 0.15, 0.21
 *  root@vampirella:~# insmod uptime_hack.ko uptime=12345
 *  root@vampirella:~# uptime
 *   14:59:52 up 35 days, 17:22,  4 users,  load average: 0.07, 0.14, 0.21
 *
 *  root@vampirella:~# echo 102021 > /sys/module/uptime_hack/parameters/uptime 
 *  root@vampirella:~# uptime
 *   14:58:25 up 295 days,  7:03,  4 users,  load average: 0.08, 0.16, 0.22
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/kernel_stat.h>
#include <linux/jiffies.h>
#include <asm/cputime.h>

#define MODULE_VERS "1.4"
#define MODULE_NAME "uptime"
#define PROCFS_NAME "uptime"

static long unsigned uptime = 0;
static long unsigned idletime = 0;
static long unsigned startjiffies = 0;

static int failed = 0;

static int (*old_uptime_proc_open)(struct inode *inode, struct file *file);
static int uptime_proc_open(struct inode *inode, struct file *file);

static struct proc_dir_entry *uptime_proc_file;
static struct proc_dir_entry *proc_root;

static struct file_operations *uptime_proc_fops;
static struct file_operations *proc_root_fops;

module_param(uptime, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(idletime, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

static void set_addr_rw(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long) addr, &level);
	if (pte->pte &~ _PAGE_RW) pte->pte |= _PAGE_RW;
}

static void set_addr_ro(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long) addr, &level);
	pte->pte = pte->pte &~_PAGE_RW;
}

static void proc_init(void)
{
	uptime_proc_file = create_proc_entry("temp_hack", 0, NULL);
	if (uptime_proc_file == NULL)
	{
		printk(KERN_ALERT "%s error: could not create temporary /proc entry\n",
				MODULE_NAME);
		failed = 1;
		return;
	}
	proc_root = uptime_proc_file->parent;
	if (proc_root == NULL || strcmp(proc_root->name, "/proc") != 0)
	{
		printk(KERN_ALERT "%s error: could not identify /proc root entry\n",
				MODULE_NAME);
		failed = 1;
		return;
	}
	proc_root_fops = (struct file_operations *)proc_root->proc_fops;
	remove_proc_entry("temp_hack", NULL);

	uptime_proc_file = proc_root->subdir;
	while (uptime_proc_file) {
		if (strcmp(uptime_proc_file->name, PROCFS_NAME) == 0)
			goto found;
		uptime_proc_file = uptime_proc_file->next;
	}
	failed = 1;
	printk(KERN_ALERT "%s did not found /proc/%s file", MODULE_NAME,
			PROCFS_NAME);
	return;

found:
	uptime_proc_fops = (struct file_operations *)uptime_proc_file->proc_fops;
	old_uptime_proc_open = uptime_proc_fops->open;

	if (uptime_proc_fops != NULL)
	{
		set_addr_rw(uptime_proc_fops);
		uptime_proc_fops->open = uptime_proc_open;
		set_addr_ro(uptime_proc_fops);

		printk(KERN_INFO "%s: successfully wrapped /proc/%s\n",
				MODULE_NAME, PROCFS_NAME);
	}
}

static void proc_cleanup(void)
{
	if (!failed && (uptime_proc_fops != NULL))
	{
		set_addr_rw(uptime_proc_fops);
		uptime_proc_fops->open = old_uptime_proc_open;
		set_addr_ro(uptime_proc_fops);

		printk(KERN_INFO "%s: successfully unwrapped /proc/%s\n",
				MODULE_NAME, PROCFS_NAME);
	}
	else
		printk(KERN_INFO "%s: nothing to unwrap, exiting\n", MODULE_NAME);
}

static int uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec calc_uptime;
	struct timespec calc_idle;
	int i;
	cputime_t calc_idletime = cputime_zero;

	if (!uptime)
	{
		do_posix_clock_monotonic_gettime(&calc_uptime);
		monotonic_to_bootbased(&calc_uptime);

		for_each_possible_cpu(i)
			calc_idletime += cputime64_add(calc_idletime, kstat_cpu(i).cpustat.idle);
		cputime_to_timespec(calc_idletime, &calc_idle);
	}
	else
	{
		calc_uptime.tv_sec = uptime * HZ + jiffies - startjiffies;
		calc_uptime.tv_nsec = 0;
		calc_idle.tv_sec = idletime * HZ + jiffies - startjiffies;
		calc_idle.tv_nsec = 0;
	}

	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
			(unsigned long) calc_uptime.tv_sec,
			(calc_uptime.tv_nsec / (NSEC_PER_SEC / 100)),
			(unsigned long) calc_idle.tv_sec,
			(calc_idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static int uptime_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, uptime_proc_show, NULL);
}

static int __init uptime_init(void)
{
	startjiffies = jiffies;
	proc_init();

	return 0;
}

static void __exit uptime_cleanup(void)
{
	proc_cleanup();
} 

module_init(uptime_init);
module_exit(uptime_cleanup);

MODULE_AUTHOR("Dinko Korunic <dinko.korunic@gmail.com>");
MODULE_DESCRIPTION("procfs uptime hack");
MODULE_LICENSE("GPL");
MODULE_VERSION(MODULE_VERS);

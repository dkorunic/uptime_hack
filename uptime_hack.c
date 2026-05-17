// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012  Dinko Korunic 'kreator'
 * Copyright (C) 2017  Jörg Thalheim 'Mic92'
 * Copyright (C) 2026  Dinko Korunic
 *
 * Fakes /proc/uptime via an ftrace hook on uptime_proc_show.
 * Requires CONFIG_DYNAMIC_FTRACE_WITH_REGS and CONFIG_KPROBES.
 */

#include <linux/ctype.h>
#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/kprobes.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/time_namespace.h>
#include <linux/timekeeping.h>

#define MODULE_VERS "1.9"
#define MODULE_NAME "uptime_hack"
#define TARGET_SYMBOL "uptime_proc_show"

static unsigned long uptime = 0;
static unsigned long idletime = 0;

static bool hideme = false;
static bool module_hidden = false;

static int param_kmod_hide(const char *, const struct kernel_param *);
static int param_set_duration(const char *, const struct kernel_param *);

module_param_call(uptime, param_set_duration, param_get_ulong, &uptime,
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(uptime,
		 "Adds this much time to the reported uptime. Accepts a plain "
		 "seconds value (e.g. 12345) or a d/h/m/s combination "
		 "(e.g. 1d2h30m, 5d 12h, 90s).");

module_param(idletime, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(idletime, "Adds this many seconds to the reported idle time");

module_param_call(hideme, param_kmod_hide, param_get_bool, &hideme,
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(hideme, "Is LKM hidden? (y/n, default is n)");

/* One-way: drops the sysfs node that backs the hideme setter. */
static void module_hide(void)
{
	if (module_hidden)
		return;

	list_del(&THIS_MODULE->list);
	kobject_del(&THIS_MODULE->mkobj.kobj);
	module_hidden = true;

#ifdef DEBUG
	printk(KERN_INFO "%s: hiding LKM\n", MODULE_NAME);
#endif
}

/* Accepts d/h/m/s tokens or bare seconds. */
static int param_set_duration(const char *val, const struct kernel_param *kp)
{
	unsigned long total = 0;
	const char *p = val;
	bool got_token = false;

	if (val == NULL)
		return -EINVAL;

	while (*p) {
		unsigned long num, mult, scaled;
		const char *digits;
		char *endp;

		while (isspace(*p))
			p++;
		if (*p == '\0')
			break;

		if (!isdigit(*p))
			return -EINVAL;

		digits = p;
		num = simple_strtoul(p, &endp, 10);
		if (endp == p)
			return -EINVAL;
		/* simple_strtoul silently wraps; cap digits to stay inside u64. */
		if (endp - digits > 18)
			return -ERANGE;
		p = endp;

		while (isspace(*p))
			p++;

		switch (*p) {
		case 'd':
		case 'D':
			mult = 24UL * 60 * 60;
			p++;
			break;
		case 'h':
		case 'H':
			mult = 60UL * 60;
			p++;
			break;
		case 'm':
		case 'M':
			mult = 60UL;
			p++;
			break;
		case 's':
		case 'S':
			mult = 1UL;
			p++;
			break;
		case '\0':
			mult = 1UL;
			break;
		default:
			return -EINVAL;
		}

		if (check_mul_overflow(num, mult, &scaled))
			return -ERANGE;
		if (check_add_overflow(total, scaled, &total))
			return -ERANGE;
		got_token = true;
	}

	if (!got_token)
		return -EINVAL;

	WRITE_ONCE(*((unsigned long *)kp->arg), total);
	return 0;
}

static int param_kmod_hide(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(val, kp);
	if (ret) {
#ifdef DEBUG
		printk(KERN_ERR
		       "%s error: could not parse LKM hideme parameters\n",
		       MODULE_NAME);
#endif
		return ret;
	}

	/* Only n->y is reachable; the sysfs file is gone after hiding. */
	if (hideme)
		module_hide();

	return 0;
}

/* Mirrors upstream uptime_proc_show, adding configured offsets. */
static int notrace hooked_uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec64 real_uptime;
	struct timespec64 real_idle;
	unsigned long uptime_off, idle_off;
	u64 nsec;
	u32 rem;
	int i;

	nsec = 0;
	for_each_possible_cpu(i)
		nsec += (__force u64)kcpustat_cpu(i).cpustat[CPUTIME_IDLE];

	ktime_get_boottime_ts64(&real_uptime);
	timens_add_boottime(&real_uptime);

	real_idle.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
	real_idle.tv_nsec = rem;

	/* Snapshot both offsets once so the two columns are consistent. */
	uptime_off = READ_ONCE(uptime);
	idle_off = READ_ONCE(idletime);

	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
		   (unsigned long)real_uptime.tv_sec + uptime_off,
		   (real_uptime.tv_nsec / (NSEC_PER_SEC / 100)),
		   (unsigned long)real_idle.tv_sec + idle_off,
		   (real_idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static unsigned long target_addr;

/* Arch-portable IP rewrite; within_module() guards against future recursion. */
static void notrace fh_callback(unsigned long ip, unsigned long parent_ip,
				struct ftrace_ops *ops,
				struct ftrace_regs *fregs)
{
	if (within_module(parent_ip, THIS_MODULE))
		return;

	ftrace_regs_set_instruction_pointer(
		fregs, (unsigned long)hooked_uptime_proc_show);
}

static struct ftrace_ops uptime_ftrace_ops = {
	.func = fh_callback,
	.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY,
};

/* kallsyms_lookup_name unexported since 5.7; kprobe symbol_name fills kp.addr. */
static unsigned long resolve_symbol(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;
	int ret;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static int hook_install(void)
{
	int ret;

	target_addr = resolve_symbol(TARGET_SYMBOL);
	if (!target_addr) {
		printk(KERN_ERR "%s: cannot resolve %s\n", MODULE_NAME,
		       TARGET_SYMBOL);
		return -ENOENT;
	}

	ret = ftrace_set_filter_ip(&uptime_ftrace_ops, target_addr, 0, 0);
	if (ret) {
		printk(KERN_ERR "%s: ftrace_set_filter_ip failed: %d\n",
		       MODULE_NAME, ret);
		return ret;
	}

	ret = register_ftrace_function(&uptime_ftrace_ops);
	if (ret) {
		printk(KERN_ERR "%s: register_ftrace_function failed: %d\n",
		       MODULE_NAME, ret);
		ftrace_set_filter_ip(&uptime_ftrace_ops, target_addr, 1, 0);
		target_addr = 0;
		return ret;
	}

#ifdef DEBUG
	printk(KERN_INFO "%s: hooked %s at %lx\n", MODULE_NAME, TARGET_SYMBOL,
	       target_addr);
#endif
	return 0;
}

static void hook_remove(void)
{
	if (!target_addr)
		return;

	unregister_ftrace_function(&uptime_ftrace_ops);
	ftrace_set_filter_ip(&uptime_ftrace_ops, target_addr, 1, 0);
	target_addr = 0;

#ifdef DEBUG
	printk(KERN_INFO "%s: unhooked %s\n", MODULE_NAME, TARGET_SYMBOL);
#endif
}

static int __init uptime_init(void)
{
	return hook_install();
}

static void __exit uptime_cleanup(void)
{
	hook_remove();
}

module_init(uptime_init);
module_exit(uptime_cleanup);

MODULE_AUTHOR("Dinko Korunic <dinko.korunic@gmail.com>");
MODULE_DESCRIPTION("procfs uptime hack (ftrace-based)");
MODULE_LICENSE("GPL");
MODULE_VERSION(MODULE_VERS);

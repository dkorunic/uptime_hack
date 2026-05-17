// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Re-lists a hidden uptime_hack module via __module_address(target).
 *
 * Usage:
 *   target=$(awk '/\[uptime_hack\]/{print "0x"$1; exit}' /proc/kallsyms)
 *   sudo insmod ./unhide.ko target=$target
 *   sudo rmmod uptime_hack && sudo rmmod unhide
 *
 * Not module_mutex-safe; do not race with insmod/rmmod.
 */

#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/preempt.h>

#define HELPER_NAME "unhide"

static unsigned long target;
module_param(target, ulong, 0444);
MODULE_PARM_DESC(target, "Any kernel-text address inside the hidden module "
			 "(e.g. from /proc/kallsyms)");

typedef struct module *(*module_address_fn)(unsigned long);

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

static int __init unhide_init(void)
{
	module_address_fn module_address_p;
	struct module *hidden;
	int ret;

	if (!target) {
		pr_err("%s: target= must be a kernel address inside the hidden module\n",
		       HELPER_NAME);
		return -EINVAL;
	}

	module_address_p =
		(module_address_fn)resolve_symbol("__module_address");
	if (!module_address_p) {
		pr_err("%s: cannot resolve __module_address via kprobe\n",
		       HELPER_NAME);
		return -ENOENT;
	}

	preempt_disable();
	hidden = module_address_p(target);
	preempt_enable();
	pr_info("%s: target=%lx __module_address=>%p THIS_MODULE=%p\n",
		HELPER_NAME, target, hidden, THIS_MODULE);
	if (!hidden) {
		pr_err("%s: __module_address(%lx) returned NULL\n", HELPER_NAME,
		       target);
		return -ENOENT;
	}
	pr_info("%s: hidden->name='%s'\n", HELPER_NAME, hidden->name);
	if (strcmp(hidden->name, HELPER_NAME) == 0) {
		pr_err("%s: target points at this helper, not a hidden module\n",
		       HELPER_NAME);
		return -EINVAL;
	}

	pr_info("%s: resolved module '%s' (%p)\n", HELPER_NAME, hidden->name,
		hidden);

	if ((unsigned long)hidden->list.next == (unsigned long)LIST_POISON1) {
		list_add(&hidden->list, &THIS_MODULE->list);
		pr_info("%s: relisted '%s' in modules\n", HELPER_NAME,
			hidden->name);
	} else {
		pr_info("%s: '%s' already in modules list — skipping list_add\n",
			HELPER_NAME, hidden->name);
	}

	pr_info("%s: kobj.state_in_sysfs=%d kobj.sd=%p kobj.kset=%p\n",
		HELPER_NAME, hidden->mkobj.kobj.state_in_sysfs,
		hidden->mkobj.kobj.sd, hidden->mkobj.kobj.kset);

	/* Trust kobj.sd; state_in_sysfs may be stale across kernel versions. */
	if (hidden->mkobj.kobj.sd == NULL) {
		hidden->mkobj.kobj.state_in_sysfs = 0;
		hidden->mkobj.kobj.parent = NULL;
		ret = kobject_add(&hidden->mkobj.kobj, NULL, "%s",
				  hidden->name);
		if (ret)
			pr_err("%s: kobject_add failed: %d (lsmod will see it but /sys/module "
			       "may be incomplete)\n",
			       HELPER_NAME, ret);
		else
			pr_info("%s: re-added /sys/module/%s\n", HELPER_NAME,
				hidden->name);
	} else {
		pr_info("%s: '%s' kobject already in sysfs — skipping kobject_add\n",
			HELPER_NAME, hidden->name);
	}

	pr_info("%s: done; you can now `rmmod %s` then `rmmod %s`\n",
		HELPER_NAME, hidden->name, HELPER_NAME);
	return 0;
}

static void __exit unhide_exit(void)
{
}

module_init(unhide_init);
module_exit(unhide_exit);

MODULE_AUTHOR("Dinko Korunic <dinko.korunic@gmail.com>");
MODULE_DESCRIPTION("One-shot helper to unhide a hidden uptime_hack module");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

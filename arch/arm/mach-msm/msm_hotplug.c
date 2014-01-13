/*
 * MSM Hotplug Driver
 *
 * Copyright (C) 2013 Fluxi <linflux@arcor.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <mach/cpufreq.h>

#define MSM_HOTPLUG		"msm_hotplug"
#define DEFAULT_UPDATE_RATE	HZ / 10
#define START_DELAY		HZ * 10
#define NUM_LOAD_LEVELS		5
#define DEFAULT_HISTORY_SIZE	10
#define DEFAULT_DOWN_LOCK_DUR	2000
#define DEFAULT_SUSPEND_FREQ	702000
#define DEFAULT_NR_CPUS_BOOSTED	2
#define DEFAULT_MIN_CPUS_ONLINE	2
#define DEFAULT_MAX_CPUS_ONLINE	NR_CPUS

static unsigned int debug = 0;
module_param_named(debug_mask, debug, uint, 0644);

#define dprintk(msg...)		\
do { 				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

static struct cpu_hotplug {
	unsigned int suspend_freq;
	atomic_t target_cpus;
	unsigned int min_cpus_online;
	unsigned int max_cpus_online;
	unsigned int cpus_boosted;
	atomic_t down_lock;
	unsigned int down_lock_dur;
	struct work_struct up_work;
	struct work_struct down_work;
	struct timer_list lock_timer;
} hotplug = {
	.suspend_freq = DEFAULT_SUSPEND_FREQ,
	.min_cpus_online = DEFAULT_MIN_CPUS_ONLINE,
	.max_cpus_online = DEFAULT_MAX_CPUS_ONLINE,
	.cpus_boosted = DEFAULT_NR_CPUS_BOOSTED,
	.down_lock = ATOMIC_INIT(0),
	.down_lock_dur = DEFAULT_DOWN_LOCK_DUR
};

static struct workqueue_struct *hotplug_wq;
static struct delayed_work hotplug_work;

static struct cpu_stats {
	unsigned int update_rate;
	unsigned int *load_hist;
	unsigned int hist_size;
	unsigned int hist_cnt;
	unsigned int min_cpus;
	unsigned int total_cpus;
	unsigned int online_cpus;
	unsigned int current_load;
} stats = {
	.update_rate = DEFAULT_UPDATE_RATE,
	.hist_size = DEFAULT_HISTORY_SIZE,
	.min_cpus = 1,
	.total_cpus = NR_CPUS
};

static DEFINE_MUTEX(hotplug_lock);
#ifdef CONFIG_HAS_EARLYSUSPEND
extern int get_suspend_state(void);
#endif
extern unsigned int report_load_at_max_freq(void);

static struct cpu_stats *get_load_stats(void)
{
	unsigned int i, j;
	unsigned int load = 0;
	struct cpu_stats *st = &stats;

	st->online_cpus = num_online_cpus();
	st->load_hist[st->hist_cnt] = report_load_at_max_freq();

	for (i = 0, j = st->hist_cnt; i < st->hist_size; i++, j--) {
		load += st->load_hist[j];

		if (j == 0)
			j = st->hist_size;
	}

	if (++st->hist_cnt == st->hist_size)
		st->hist_cnt = 0;

	st->current_load = load / st->hist_size;

	return st;
}
EXPORT_SYMBOL_GPL(get_load_stats);

struct load_thresh_tbl {
	unsigned int up_threshold;
	unsigned int down_threshold;
};

#define LOAD_SCALE(u, d)             \
{                          	     \
		.up_threshold = u,   \
		.down_threshold = d, \
}

static struct load_thresh_tbl load[] = {
	LOAD_SCALE(400, 0),
	LOAD_SCALE(50, 0),
	LOAD_SCALE(100, 40),
	LOAD_SCALE(150, 80),
	LOAD_SCALE(410, 140),
};

static void apply_down_lock(void)
{
	struct cpu_hotplug *hp = &hotplug;

	atomic_set(&hp->down_lock, 1);
	mod_timer(&hp->lock_timer,
		  jiffies + msecs_to_jiffies(hp->down_lock_dur));
}
EXPORT_SYMBOL_GPL(apply_down_lock);

static void handle_lock_timer(unsigned long data)
{
	struct cpu_hotplug *hp = &hotplug;

	atomic_set(&hp->down_lock, 0);
}
EXPORT_SYMBOL_GPL(handle_lock_timer);

static void __ref cpu_up_work(struct work_struct *work)
{
	int cpu;
	int target;
	struct cpu_hotplug *hp = &hotplug;

	target = atomic_read(&hp->target_cpus);

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (target == num_online_cpus())
			break;
		if (cpu == 0)
			continue;
		cpu_up(cpu);
	}
}
EXPORT_SYMBOL_GPL(cpu_up_work);

static void cpu_down_work(struct work_struct *work)
{
	int cpu;
	int lock, target;
	struct cpu_hotplug *hp = &hotplug;

	lock = atomic_read(&hp->down_lock);
	if (lock)
		return;

	target = atomic_read(&hp->target_cpus);

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		cpu_down(cpu);
		if (target == num_online_cpus())
			break;
	}
}
EXPORT_SYMBOL_GPL(cpu_down_work);

static void online_cpu(unsigned int target)
{
	struct cpu_hotplug *hp = &hotplug;

	atomic_set(&hp->target_cpus, target);
	apply_down_lock();
	queue_work_on(0, hotplug_wq, &hp->up_work);
}
EXPORT_SYMBOL_GPL(online_cpu);

static void offline_cpu(unsigned int target)
{
	struct cpu_hotplug *hp = &hotplug;

	atomic_set(&hp->target_cpus, target);
	queue_work_on(0, hotplug_wq, &hp->down_work);
}
EXPORT_SYMBOL_GPL(offline_cpu);

static int reschedule_hotplug_fn(struct cpu_stats *st)
{
	return queue_delayed_work_on(0, hotplug_wq, &hotplug_work,
				     st->update_rate);
}
EXPORT_SYMBOL_GPL(reschedule_hotplug_fn);

static void msm_hotplug_fn(struct work_struct *work)
{
	unsigned int cur_load, online_cpus, target = 0;
	unsigned int i;
	struct cpu_stats *st = get_load_stats();
	struct cpu_hotplug *hp = &hotplug;

	cur_load = st->current_load;
	online_cpus = st->online_cpus;

	/* if nr of cpus locked, break out early */
	if (hp->min_cpus_online == num_possible_cpus()) {
		if (online_cpus != hp->min_cpus_online)
			online_cpu(hp->min_cpus_online);
		goto reschedule;
	} else if (hp->max_cpus_online == st->min_cpus) {
		if (online_cpus != hp->max_cpus_online)
			offline_cpu(hp->max_cpus_online);
		goto reschedule;
	}

	if (online_cpus < hp->cpus_boosted && mako_boosted) {
		dprintk("%s: cur_load: %3u online_cpus: %u mako_boosted\n",
			MSM_HOTPLUG, cur_load, online_cpus);
		online_cpu(hp->cpus_boosted);
		goto reschedule;
	}

	for (i = st->min_cpus; i < NUM_LOAD_LEVELS; i++) {
		if (cur_load <= load[i].up_threshold
		    && cur_load > load[i].down_threshold) {
			target = i;
			break;
		}
	}

	if (target > hp->max_cpus_online)
		target = hp->max_cpus_online;
	else if (target < hp->min_cpus_online)
		target = hp->min_cpus_online;

	if (online_cpus != target) {
		if (target > online_cpus)
			online_cpu(target);
		else if (target < online_cpus)
			offline_cpu(target);
	}

	dprintk("%s: cur_load: %3u online_cpus: %u target: %u\n", MSM_HOTPLUG,
		cur_load, online_cpus, target);

reschedule:
	reschedule_hotplug_fn(st);
}
EXPORT_SYMBOL_GPL(msm_hotplug_fn);

static void msm_hotplug_early_suspend(struct early_suspend *handler)
{
	unsigned int cpu = 0;
	struct cpu_hotplug *hp = &hotplug;
	struct cpu_stats *st = &stats;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		return;

	flush_workqueue(hotplug_wq);
	cancel_delayed_work_sync(&hotplug_work);

	atomic_set(&hp->down_lock, 0);
	offline_cpu(st->min_cpus);

	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, hp->suspend_freq);
	pr_info("%s: Early suspend - max freq: %dMHz\n", MSM_HOTPLUG,
		hp->suspend_freq / 1000);
}
EXPORT_SYMBOL_GPL(msm_hotplug_early_suspend);

static void msm_hotplug_late_resume(struct early_suspend *handler)
{
	unsigned int cpu = 0;
	struct cpu_stats *st = &stats;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		return;

	online_cpu(st->total_cpus);

	for_each_possible_cpu(cpu)
	    msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT,
					MSM_CPUFREQ_NO_LIMIT);

	pr_info("%s: Late resume - restore max frequency: %dMHz\n",
		MSM_HOTPLUG, policy->max / 1000);

	reschedule_hotplug_fn(st);
}
EXPORT_SYMBOL_GPL(msm_hotplug_late_resume);

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend msm_hotplug_suspend = {
	.suspend = msm_hotplug_early_suspend,
	.resume = msm_hotplug_late_resume,
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 1,
};
#endif

static int __init msm_hotplug_init(void)
{
	int ret = 0;
	struct cpu_stats *st = &stats;
	struct cpu_hotplug *hp = &hotplug;

	hotplug_wq = alloc_workqueue("msm_hotplug_wq", 0, 0);
	if (!hotplug_wq) {
		pr_err("%s: Creation of hotplug work failed\n", MSM_HOTPLUG);
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&hotplug_work, msm_hotplug_fn);
	INIT_WORK(&hp->up_work, cpu_up_work);
	INIT_WORK(&hp->down_work, cpu_down_work);

	queue_delayed_work_on(0, hotplug_wq, &hotplug_work, START_DELAY);

	st->load_hist = kmalloc(sizeof(st->hist_size), GFP_KERNEL);
	if (!st->load_hist)
		return -ENOMEM;

	setup_timer(&hp->lock_timer, handle_lock_timer, 0);
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&msm_hotplug_suspend);
#endif
	return ret;
}
EXPORT_SYMBOL_GPL(msm_hotplug_init);

late_initcall(msm_hotplug_init);

static struct platform_device msm_hotplug_device = {
	.name = MSM_HOTPLUG,
	.id = -1,
};

static int __init msm_hotplug_device_init(void)
{
	int ret;
	struct kobject *module_kobj;

	ret = platform_device_register(&msm_hotplug_device);
	if (ret) {
		pr_err("%s: Device init failed: %d\n", MSM_HOTPLUG, ret);
		return ret;
	}

	module_kobj = kset_find_obj(module_kset, MSM_HOTPLUG);
	if (!module_kobj) {
		pr_err("%s: Cannot find kobject for module\n", MSM_HOTPLUG);
		return -ENOENT;
	}

	pr_info("%s: Device init\n", MSM_HOTPLUG);

	return ret;
}
EXPORT_SYMBOL_GPL(msm_hotplug_device_init);

static void __exit msm_hotplug_device_exit(void)
{
	struct cpu_hotplug *hp = &hotplug;
	struct cpu_stats *st = &stats;

	unregister_early_suspend(&msm_hotplug_suspend);
	del_timer(&hp->lock_timer);
	kfree(st->load_hist);
}
EXPORT_SYMBOL_GPL(msm_hotplug_device_exit);

late_initcall(msm_hotplug_device_init);
module_exit(msm_hotplug_device_exit);

MODULE_AUTHOR("Fluxi <linflux@arcor.de>");
MODULE_DESCRIPTION("MSM Hotplug Driver");
MODULE_LICENSE("GPL");

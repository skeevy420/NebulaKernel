/*
 *  drivers/cpufreq/cpufreq_elementalx.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2015 Aaron Segaert <asegaert@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/msm_kgsl.h>
#include "cpufreq_governor.h"

/* elementalx governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(90)
#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(20)
#define DEF_ACTIVE_FLOOR_FREQ			(960000)
#define DEF_GBOOST_MIN_FREQ			(1574400)
#define DEF_MAX_SCREEN_OFF_FREQ			(2265000)
#define MIN_SAMPLING_RATE			(10000)
#define DEF_SAMPLING_DOWN_FACTOR		(8)
#define MAX_SAMPLING_DOWN_FACTOR		(20)
#define FREQ_NEED_BURST(x)			(x < 600000 ? 1 : 0)
#define MAX(x,y)				(x > y ? x : y)
#define MIN(x,y)				(x < y ? x : y)
#define TABLE_SIZE				5

static DEFINE_PER_CPU(struct ex_cpu_dbs_info_s, ex_cpu_dbs_info);

static unsigned int up_threshold_level[2] __read_mostly = {95, 85};
static struct cpufreq_frequency_table *tbl = NULL;
static unsigned int *tblmap[TABLE_SIZE] __read_mostly;
static unsigned int tbl_select[4];

static struct ex_governor_data {
	unsigned int active_floor_freq;
	unsigned int max_screen_off_freq;
	unsigned int prev_load;
	unsigned int g_count;
	bool suspended;
	struct notifier_block notif;
} ex_data = {
	.active_floor_freq = DEF_ACTIVE_FLOOR_FREQ,
	.max_screen_off_freq = DEF_MAX_SCREEN_OFF_FREQ,
	.prev_load = 0,
	.g_count = 0,
	.suspended = false
};

static void dbs_init_freq_map_table(void)
{
	unsigned int min_diff, top1, top2;
	int cnt, i, j;
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get(0);
	tbl = cpufreq_frequency_get_table(0);
	min_diff = policy->cpuinfo.max_freq;

	for (cnt = 0; (tbl[cnt].frequency != CPUFREQ_TABLE_END); cnt++) {
		if (cnt > 0)
			min_diff = MIN(tbl[cnt].frequency - tbl[cnt-1].frequency, min_diff);
	}

	top1 = (policy->cpuinfo.max_freq + policy->cpuinfo.min_freq) / 2;
	top2 = (policy->cpuinfo.max_freq + top1) / 2;

	for (i = 0; i < TABLE_SIZE; i++) {
		tblmap[i] = kmalloc(sizeof(unsigned int) * cnt, GFP_KERNEL);
		BUG_ON(!tblmap[i]);
		for (j = 0; j < cnt; j++)
			tblmap[i][j] = tbl[j].frequency;
	}

	for (j = 0; j < cnt; j++) {
		if (tbl[j].frequency < top1) {
			tblmap[0][j] += MAX((top1 - tbl[j].frequency)/3, min_diff);
		}

		if (tbl[j].frequency < top2) {
			tblmap[1][j] += MAX((top2 - tbl[j].frequency)/3, min_diff);
			tblmap[2][j] += MAX(((top2 - tbl[j].frequency)*2)/5, min_diff);
			tblmap[3][j] += MAX((top2 - tbl[j].frequency)/2, min_diff);
		} else {
			tblmap[3][j] += MAX((policy->cpuinfo.max_freq - tbl[j].frequency)/3, min_diff);
		}

		tblmap[4][j] += MAX((policy->cpuinfo.max_freq - tbl[j].frequency)/2, min_diff);
	}

	tbl_select[0] = 0;
	tbl_select[1] = 1;
	tbl_select[2] = 2;
	tbl_select[3] = 4;
}

static void dbs_deinit_freq_map_table(void)
{
	int i;

	if (!tbl)
		return;

	tbl = NULL;

	for (i = 0; i < TABLE_SIZE; i++)
		kfree(tblmap[i]);
}

static inline int get_cpu_freq_index(unsigned int freq)
{
	static int saved_index = 0;
	int index;

	if (!tbl) {
		pr_warn("tbl is NULL, use previous value %d\n", saved_index);
		return saved_index;
	}

	for (index = 0; (tbl[index].frequency != CPUFREQ_TABLE_END); index++) {
		if (tbl[index].frequency >= freq) {
			saved_index = index;
			break;
		}
	}

	return index;
}

static inline unsigned int ex_freq_increase(struct cpufreq_policy *p, unsigned int freq)
{
	if (freq > p->max) {
		return p->max;
	} 
	
	else if (ex_data.suspended) {
		freq = MIN(freq, ex_data.max_screen_off_freq);
	}

	return freq;
}

static void ex_check_cpu(int cpu, unsigned int load)
{
	struct ex_cpu_dbs_info_s *dbs_info = &per_cpu(ex_cpu_dbs_info, cpu);
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	struct dbs_data *dbs_data = policy->governor_data;
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int max_load_freq = 0, freq_next = 0;
	unsigned int j, avg_load, cur_freq, max_freq, target_freq = 0;

	cpufreq_notify_utilization(policy, load);

	cur_freq = policy->cur;
	max_freq = policy->max;

	for_each_cpu(j, policy->cpus) {
		if (load > max_load_freq)
			max_load_freq = load * policy->cur;
	}
	avg_load = (ex_data.prev_load + load) >> 1;

	if (ex_tuners->gboost) {
		if (ex_data.g_count < 500 && graphics_boost < 3)
			++ex_data.g_count;
		else if (ex_data.g_count > 1)
			--ex_data.g_count;
	}

	//gboost mode
	if (ex_tuners->gboost && ex_data.g_count > 300) {
				
		if (avg_load > 40 + (graphics_boost * 10)) {
			freq_next = max_freq;
		} else {
			freq_next = max_freq * avg_load / 100;
			freq_next = MAX(freq_next, ex_tuners->gboost_min_freq);
		}

		target_freq = ex_freq_increase(policy, freq_next);

		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);

		goto finished;
	} 

	//normal mode
	if (max_load_freq > up_threshold_level[1] * cur_freq) {
		int index = get_cpu_freq_index(cur_freq);

		if (FREQ_NEED_BURST(cur_freq) &&
				load > up_threshold_level[0]) {
			freq_next = max_freq;
		}
		
		else if (avg_load > up_threshold_level[0]) {
			freq_next = tblmap[tbl_select[3]][index];
		}
		
		else if (avg_load <= up_threshold_level[1]) {
			freq_next = tblmap[tbl_select[1]][index];
		}
	
		else {
			if (load > up_threshold_level[0]) {
				freq_next = tblmap[tbl_select[3]][index];
			}
		
			else {
				freq_next = tblmap[tbl_select[2]][index];
			}
		}

		target_freq = ex_freq_increase(policy, freq_next);

		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);

		if (target_freq > ex_data.active_floor_freq)
			dbs_info->down_floor = 0;

		goto finished;
	}

	if (cur_freq == policy->min)
		goto finished;

	if (cur_freq >= ex_data.active_floor_freq) {
		if (++dbs_info->down_floor > ex_tuners->sampling_down_factor)
			dbs_info->down_floor = 0;
	} else {
		dbs_info->down_floor = 0;
	}

	if (max_load_freq <
	    (ex_tuners->up_threshold - ex_tuners->down_differential) *
	     cur_freq) {

		freq_next = max_load_freq /
				(ex_tuners->up_threshold -
				 ex_tuners->down_differential);

		if (dbs_info->down_floor && !ex_data.suspended) {
			freq_next = MAX(freq_next, ex_data.active_floor_freq);
		} else {
			freq_next = MAX(freq_next, policy->min);
			if (freq_next < ex_data.active_floor_freq)
				dbs_info->down_floor = ex_tuners->sampling_down_factor;
		}

		__cpufreq_driver_target(policy, freq_next,
			CPUFREQ_RELATION_L);
	}

finished:
	ex_data.prev_load = load;
	return;
}

static void ex_dbs_timer(struct work_struct *work)
{
	struct ex_cpu_dbs_info_s *dbs_info = container_of(work,
			struct ex_cpu_dbs_info_s, cdbs.work.work);
	unsigned int cpu = dbs_info->cdbs.cur_policy->cpu;
	struct ex_cpu_dbs_info_s *core_dbs_info = &per_cpu(ex_cpu_dbs_info,
			cpu);
	struct dbs_data *dbs_data = dbs_info->cdbs.cur_policy->governor_data;
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	int delay = delay_for_sampling_rate(ex_tuners->sampling_rate);
	bool modify_all = true;

	mutex_lock(&core_dbs_info->cdbs.timer_mutex);
	if (!need_load_eval(&core_dbs_info->cdbs, ex_tuners->sampling_rate))
		modify_all = false;
	else
		dbs_check_cpu(dbs_data, cpu);

	gov_queue_work(dbs_data, dbs_info->cdbs.cur_policy, delay, modify_all);
	mutex_unlock(&core_dbs_info->cdbs.timer_mutex);
}

static int fb_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
			case FB_BLANK_UNBLANK:
				//display on
				ex_data.suspended = false;
				break;
			case FB_BLANK_POWERDOWN:
			case FB_BLANK_HSYNC_SUSPEND:
			case FB_BLANK_VSYNC_SUSPEND:
			case FB_BLANK_NORMAL:
				//display off
				ex_data.suspended = true;
				break;
		}
	}

	return NOTIFY_OK;
}

/************************** sysfs interface ************************/
static struct common_dbs_data ex_dbs_cdata;

static ssize_t store_sampling_rate(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->sampling_rate = max(input, dbs_data->min_sampling_rate);
	return count;
}

static ssize_t store_up_threshold(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input <= ex_tuners->down_differential)
		return -EINVAL;

	ex_tuners->up_threshold = input;
	return count;
}

static ssize_t store_down_differential(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input >= ex_tuners->up_threshold)
		return -EINVAL;

	ex_tuners->down_differential = input;
	return count;
}

static ssize_t store_gboost(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 1)
		return -EINVAL;

	if (input == 0)
		ex_data.g_count = 0;

	ex_tuners->gboost = input;
	return count;
}

static ssize_t store_gboost_min_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->gboost_min_freq = input;
	return count;
}

static ssize_t store_active_floor_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->active_floor_freq = input;
	ex_data.active_floor_freq = ex_tuners->active_floor_freq;
	return count;
}

static ssize_t store_max_screen_off_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input == 0)
		input = UINT_MAX;

	ex_tuners->max_screen_off_freq = input;
	ex_data.max_screen_off_freq = ex_tuners->max_screen_off_freq;
	return count;
}

static ssize_t store_sampling_down_factor(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 0)
		return -EINVAL;

	ex_tuners->sampling_down_factor = input;
	return count;
}

show_store_one(ex, sampling_rate);
show_store_one(ex, up_threshold);
show_store_one(ex, down_differential);
show_store_one(ex, gboost);
show_store_one(ex, gboost_min_freq);
show_store_one(ex, active_floor_freq);
show_store_one(ex, max_screen_off_freq);
show_store_one(ex, sampling_down_factor);
declare_show_sampling_rate_min(ex);

gov_sys_pol_attr_rw(sampling_rate);
gov_sys_pol_attr_rw(up_threshold);
gov_sys_pol_attr_rw(down_differential);
gov_sys_pol_attr_rw(gboost);
gov_sys_pol_attr_rw(gboost_min_freq);
gov_sys_pol_attr_rw(active_floor_freq);
gov_sys_pol_attr_rw(max_screen_off_freq);
gov_sys_pol_attr_rw(sampling_down_factor);
gov_sys_pol_attr_ro(sampling_rate_min);

static struct attribute *dbs_attributes_gov_sys[] = {
	&sampling_rate_min_gov_sys.attr,
	&sampling_rate_gov_sys.attr,
	&up_threshold_gov_sys.attr,
	&down_differential_gov_sys.attr,
	&gboost_gov_sys.attr,
	&gboost_min_freq_gov_sys.attr,
	&active_floor_freq_gov_sys.attr,
	&max_screen_off_freq_gov_sys.attr,
	&sampling_down_factor_gov_sys.attr,
	NULL
};

static struct attribute_group ex_attr_group_gov_sys = {
	.attrs = dbs_attributes_gov_sys,
	.name = "elementalx",
};

static struct attribute *dbs_attributes_gov_pol[] = {
	&sampling_rate_min_gov_pol.attr,
	&sampling_rate_gov_pol.attr,
	&up_threshold_gov_pol.attr,
	&down_differential_gov_pol.attr,
	&gboost_gov_pol.attr,
	&gboost_min_freq_gov_pol.attr,
	&active_floor_freq_gov_pol.attr,
	&max_screen_off_freq_gov_pol.attr,
	&sampling_down_factor_gov_pol.attr,
	NULL
};

static struct attribute_group ex_attr_group_gov_pol = {
	.attrs = dbs_attributes_gov_pol,
	.name = "elementalx",
};

/************************** sysfs end ************************/

static int ex_init(struct dbs_data *dbs_data)
{
	struct ex_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners) {
		pr_err("%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	tuners->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	tuners->down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL;
	tuners->ignore_nice_load = 0;
	tuners->gboost = 1;
	tuners->gboost_min_freq = DEF_GBOOST_MIN_FREQ;
	tuners->active_floor_freq = DEF_ACTIVE_FLOOR_FREQ;
	tuners->max_screen_off_freq = DEF_MAX_SCREEN_OFF_FREQ;
	tuners->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;

	dbs_data->tuners = tuners;
	dbs_data->min_sampling_rate = MIN_SAMPLING_RATE;

	dbs_init_freq_map_table();

	ex_data.notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&ex_data.notif))
		pr_err("%s: Failed to register fb_notifier\n", __func__);

	mutex_init(&dbs_data->mutex);
	return 0;
}

static void ex_exit(struct dbs_data *dbs_data)
{
	dbs_deinit_freq_map_table();
	fb_unregister_client(&ex_data.notif);
	kfree(dbs_data->tuners);
}

define_get_cpu_dbs_routines(ex_cpu_dbs_info);

static struct common_dbs_data ex_dbs_cdata = {
	.governor = GOV_ELEMENTALX,
	.attr_group_gov_sys = &ex_attr_group_gov_sys,
	.attr_group_gov_pol = &ex_attr_group_gov_pol,
	.get_cpu_cdbs = get_cpu_cdbs,
	.get_cpu_dbs_info_s = get_cpu_dbs_info_s,
	.gov_dbs_timer = ex_dbs_timer,
	.gov_check_cpu = ex_check_cpu,
	.init = ex_init,
	.exit = ex_exit,
};

static int ex_cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	return cpufreq_governor_dbs(policy, &ex_dbs_cdata, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
static
#endif
struct cpufreq_governor cpufreq_gov_elementalx = {
	.name			= "elementalx",
	.governor		= ex_cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_elementalx);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_elementalx);
}

MODULE_AUTHOR("Aaron Segaert <asegaert@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_elementalx' - multiphase cpufreq governor");
MODULE_LICENSE("GPL");

* drivers/cpufreq/cpufreq_elementalx.c
*
* Copyright (C) 2001 Russell King
* (C) 2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
* Jun Nakajima <jun.nakajima@intel.com>
* (C) 2013 flar2 <asegaert@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <mach/kgsl.h>
static int orig_up_threshold = 90;
static int g_count = 0;
#define DEF_SAMPLING_RATE	(30000)
#define DEF_FREQUENCY_DOWN_DIFFERENTIAL	(20)
#define DEF_FREQUENCY_UP_THRESHOLD	(90)
#define DEF_SAMPLING_DOWN_FACTOR	(4)
#define MAX_SAMPLING_DOWN_FACTOR	(20)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL	(3)
#define MICRO_FREQUENCY_UP_THRESHOLD	(95)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE	(10000)
#define MIN_FREQUENCY_UP_THRESHOLD	(11)
#define MAX_FREQUENCY_UP_THRESHOLD	(100)
#define MIN_FREQUENCY_DOWN_DIFFERENTIAL	(1)
#define UI_DYNAMIC_SAMPLING_RATE	(15000)
#define DBS_SWITCH_MODE_TIMEOUT	(1000)
#define INPUT_EVENT_MIN_TIMEOUT (0)
#define INPUT_EVENT_MAX_TIMEOUT (3000)
#define INPUT_EVENT_TIMEOUT	(500)
#define MIN_SAMPLING_RATE_RATIO	(2)
static unsigned int min_sampling_rate;
static unsigned int skip_elementalx = 0;
#define LATENCY_MULTIPLIER	(1000)
#define MIN_LATENCY_MULTIPLIER	(100)
#define TRANSITION_LATENCY_LIMIT	(10 * 1000 * 1000)
static void do_dbs_timer(struct work_struct *work);
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
unsigned int event);
#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
static
#endif
struct cpufreq_governor cpufreq_gov_elementalx = {
.name = "elementalx",
.governor = cpufreq_governor_dbs,
.max_transition_latency = TRANSITION_LATENCY_LIMIT,
.owner = THIS_MODULE,
};
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};
struct cpu_dbs_info_s {
u64 prev_cpu_idle;
u64 prev_cpu_iowait;
u64 prev_cpu_wall;
u64 prev_cpu_nice;
struct cpufreq_policy *cur_policy;
struct delayed_work work;
struct cpufreq_frequency_table *freq_table;
unsigned int freq_lo;
unsigned int freq_lo_jiffies;
unsigned int freq_hi_jiffies;
unsigned int rate_mult;
unsigned int prev_load;
unsigned int max_load;
int input_event_freq;
int cpu;
unsigned int sample_type:1;
struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, od_cpu_dbs_info);
static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info);
static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info);
static unsigned int dbs_enable;
static DEFINE_PER_CPU(struct task_struct *, up_task);
static spinlock_t input_boost_lock;
static bool input_event_boost = false;
static unsigned long input_event_boost_expired = 0;
#define TABLE_SIZE	5
#define MAX(x,y) (x > y ? x : y)
#define MIN(x,y) (x < y ? x : y)
#define FREQ_NEED_BURST(x) (x < 1000000 ? 1 : 0)
static	struct cpufreq_frequency_table *tbl = NULL;
static unsigned int *tblmap[TABLE_SIZE] __read_mostly;
static unsigned int tbl_select[4];
static unsigned int up_threshold_level[2] __read_mostly = {95, 85};
static int input_event_counter = 0;
struct timer_list freq_mode_timer;
static inline void switch_turbo_mode(unsigned);
static inline void switch_normal_mode(void);
static DEFINE_MUTEX(dbs_mutex);
static struct dbs_tuners {
unsigned int sampling_rate;
unsigned int up_threshold;
unsigned int up_threshold_multi_core;
unsigned int down_differential;
unsigned int down_differential_multi_core;
unsigned int optimal_freq;
unsigned int up_threshold_any_cpu_load;
unsigned int sync_freq;
unsigned int ignore_nice;
unsigned int sampling_down_factor;
unsigned int io_is_busy;
unsigned int two_phase_freq;
unsigned int origin_sampling_rate;
unsigned int ui_sampling_rate;
unsigned int input_event_timeout;
int gboost;
} dbs_tuners_ins = {
.up_threshold_multi_core = DEF_FREQUENCY_UP_THRESHOLD,
.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
.down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL,
.down_differential_multi_core = MICRO_FREQUENCY_DOWN_DIFFERENTIAL,
.up_threshold_any_cpu_load = DEF_FREQUENCY_UP_THRESHOLD,
.ignore_nice = 0,
.sync_freq = 0,
.optimal_freq = 0,
.io_is_busy = 1,
.two_phase_freq = 0,
.ui_sampling_rate = UI_DYNAMIC_SAMPLING_RATE,
.input_event_timeout = INPUT_EVENT_TIMEOUT,
.gboost = 1,
};
static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
u64 idle_time;
u64 cur_wall_time;
u64 busy_time;
cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
busy_time = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];
idle_time = cur_wall_time - busy_time;
if (wall)
*wall = jiffies_to_usecs(cur_wall_time);
return jiffies_to_usecs(idle_time);
}
static inline u64 get_cpu_iowait_time(unsigned int cpu, u64 *wall)
{
u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);
if (iowait_time == -1ULL)
return 0;
return iowait_time;
}
static ssize_t show_sampling_rate_min(struct kobject *kobj,
struct attribute *attr, char *buf)
{
return sprintf(buf, "%u\n", min_sampling_rate);
}
define_one_global_ro(sampling_rate_min);
#define show_one(file_name, object) \
static ssize_t show_##file_name \
(struct kobject *kobj, struct attribute *attr, char *buf) \
{ \
return sprintf(buf, "%u\n", dbs_tuners_ins.object); \
}
show_one(sampling_rate, sampling_rate);
show_one(up_threshold, up_threshold);
show_one(up_threshold_multi_core, up_threshold_multi_core);
show_one(down_differential, down_differential);
show_one(sampling_down_factor, sampling_down_factor);
show_one(ignore_nice_load, ignore_nice);
show_one(optimal_freq, optimal_freq);
show_one(up_threshold_any_cpu_load, up_threshold_any_cpu_load);
show_one(sync_freq, sync_freq);
show_one(gboost, gboost);
static void update_sampling_rate(unsigned int new_rate)
{
int cpu;
dbs_tuners_ins.sampling_rate = new_rate
= max(new_rate, min_sampling_rate);
for_each_online_cpu(cpu) {
struct cpufreq_policy *policy;
struct cpu_dbs_info_s *dbs_info;
unsigned long next_sampling, appointed_at;
policy = cpufreq_cpu_get(cpu);
if (!policy)
continue;
dbs_info = &per_cpu(od_cpu_dbs_info, policy->cpu);
cpufreq_cpu_put(policy);
mutex_lock(&dbs_info->timer_mutex);
if (!delayed_work_pending(&dbs_info->work)) {
mutex_unlock(&dbs_info->timer_mutex);
continue;
}
next_sampling = jiffies + usecs_to_jiffies(new_rate);
appointed_at = dbs_info->work.timer.expires;
if (time_before(next_sampling, appointed_at)) {
mutex_unlock(&dbs_info->timer_mutex);
cancel_delayed_work_sync(&dbs_info->work);
mutex_lock(&dbs_info->timer_mutex);
schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work,
usecs_to_jiffies(new_rate));
}
mutex_unlock(&dbs_info->timer_mutex);
}
}
show_one(input_event_timeout, input_event_timeout);
static ssize_t store_input_event_timeout(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
input = max(input, (unsigned int)INPUT_EVENT_MIN_TIMEOUT);
dbs_tuners_ins.input_event_timeout = min(input, (unsigned int)INPUT_EVENT_MAX_TIMEOUT);
return count;
}
static int two_phase_freq_array[NR_CPUS] = {[0 ... NR_CPUS-1] = 1728000} ;
static ssize_t show_two_phase_freq
(struct kobject *kobj, struct attribute *attr, char *buf)
{
int i = 0 ;
int shift = 0 ;
char *buf_pos = buf;
for ( i = 0 ; i < NR_CPUS; i++) {
shift = sprintf(buf_pos,"%d,",two_phase_freq_array[i]);
buf_pos += shift;
}
*(buf_pos-1) = '\0';
return strlen(buf);
}
static ssize_t store_two_phase_freq(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
int ret = 0;
if (NR_CPUS == 1)
ret = sscanf(buf,"%u",&two_phase_freq_array[0]);
else if (NR_CPUS == 2)
ret = sscanf(buf,"%u,%u",&two_phase_freq_array[0],
&two_phase_freq_array[1]);
else if (NR_CPUS == 4)
ret = sscanf(buf, "%u,%u,%u,%u", &two_phase_freq_array[0],
&two_phase_freq_array[1],
&two_phase_freq_array[2],
&two_phase_freq_array[3]);
if (ret < NR_CPUS)
return -EINVAL;
return count;
}
static int input_event_min_freq_array[NR_CPUS] = {1728000, 1267200, 1267200, 1267200} ;
static ssize_t show_input_event_min_freq
(struct kobject *kobj, struct attribute *attr, char *buf)
{
int i = 0 ;
int shift = 0 ;
char *buf_pos = buf;
for ( i = 0 ; i < NR_CPUS; i++) {
shift = sprintf(buf_pos,"%d,",input_event_min_freq_array[i]);
buf_pos += shift;
}
*(buf_pos-1) = '\0';
return strlen(buf);
}
static ssize_t store_input_event_min_freq(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
int ret = 0;
if (NR_CPUS == 1)
ret = sscanf(buf,"%u",&input_event_min_freq_array[0]);
else if (NR_CPUS == 2)
ret = sscanf(buf,"%u,%u",&input_event_min_freq_array[0],
&input_event_min_freq_array[1]);
else if (NR_CPUS == 4)
ret = sscanf(buf, "%u,%u,%u,%u", &input_event_min_freq_array[0],
&input_event_min_freq_array[1],
&input_event_min_freq_array[2],
&input_event_min_freq_array[3]);
if (ret < NR_CPUS)
return -EINVAL;
return count;
}
show_one(ui_sampling_rate, ui_sampling_rate);
static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
if (input == dbs_tuners_ins.origin_sampling_rate)
return count;
update_sampling_rate(input);
dbs_tuners_ins.origin_sampling_rate = dbs_tuners_ins.sampling_rate;
return count;
}
static ssize_t store_ui_sampling_rate(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
dbs_tuners_ins.ui_sampling_rate = max(input, min_sampling_rate);
return count;
}
static ssize_t store_sync_freq(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
dbs_tuners_ins.sync_freq = input;
return count;
}
static ssize_t store_optimal_freq(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
dbs_tuners_ins.optimal_freq = input;
return count;
}
static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
input < MIN_FREQUENCY_UP_THRESHOLD) {
return -EINVAL;
}
dbs_tuners_ins.up_threshold = input;
orig_up_threshold = dbs_tuners_ins.up_threshold;
return count;
}
static ssize_t store_up_threshold_multi_core(struct kobject *a,
struct attribute *b, const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
input < MIN_FREQUENCY_UP_THRESHOLD) {
return -EINVAL;
}
dbs_tuners_ins.up_threshold_multi_core = input;
return count;
}
static ssize_t store_up_threshold_any_cpu_load(struct kobject *a,
struct attribute *b, const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
input < MIN_FREQUENCY_UP_THRESHOLD) {
return -EINVAL;
}
dbs_tuners_ins.up_threshold_any_cpu_load = input;
return count;
}
static ssize_t store_down_differential(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1 || input >= dbs_tuners_ins.up_threshold ||
input < MIN_FREQUENCY_DOWN_DIFFERENTIAL) {
return -EINVAL;
}
dbs_tuners_ins.down_differential = input;
return count;
}
static ssize_t store_sampling_down_factor(struct kobject *a,
struct attribute *b, const char *buf, size_t count)
{
unsigned int input, j;
int ret;
ret = sscanf(buf, "%u", &input);
if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
return -EINVAL;
dbs_tuners_ins.sampling_down_factor = input;
for_each_online_cpu(j) {
struct cpu_dbs_info_s *dbs_info;
dbs_info = &per_cpu(od_cpu_dbs_info, j);
dbs_info->rate_mult = 1;
}
return count;
}
static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
unsigned int j;
ret = sscanf(buf, "%u", &input);
if (ret != 1)
return -EINVAL;
if (input > 1)
input = 1;
if (input == dbs_tuners_ins.ignore_nice) {
return count;
}
dbs_tuners_ins.ignore_nice = input;
for_each_online_cpu(j) {
struct cpu_dbs_info_s *dbs_info;
dbs_info = &per_cpu(od_cpu_dbs_info, j);
dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
&dbs_info->prev_cpu_wall, dbs_tuners_ins.io_is_busy);
if (dbs_tuners_ins.ignore_nice)
dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
}
return count;
}
static ssize_t store_gboost(struct kobject *a, struct attribute *b,
const char *buf, size_t count)
{
unsigned int input;
int ret;
ret = sscanf(buf, "%u", &input);
if(ret != 1)
return -EINVAL;
dbs_tuners_ins.gboost = (input > 0 ? input : 0);
return count;
}
define_one_global_rw(sampling_rate);
define_one_global_rw(up_threshold);
define_one_global_rw(down_differential);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(up_threshold_multi_core);
define_one_global_rw(optimal_freq);
define_one_global_rw(up_threshold_any_cpu_load);
define_one_global_rw(sync_freq);
define_one_global_rw(two_phase_freq);
define_one_global_rw(input_event_min_freq);
define_one_global_rw(ui_sampling_rate);
define_one_global_rw(input_event_timeout);
define_one_global_rw(gboost);
static struct attribute *dbs_attributes[] = {
&sampling_rate_min.attr,
&sampling_rate.attr,
&up_threshold.attr,
&down_differential.attr,
&sampling_down_factor.attr,
&ignore_nice_load.attr,
&up_threshold_multi_core.attr,
&optimal_freq.attr,
&up_threshold_any_cpu_load.attr,
&sync_freq.attr,
&two_phase_freq.attr,
&input_event_min_freq.attr,
&ui_sampling_rate.attr,
&input_event_timeout.attr,
&gboost.attr,
NULL
};
static struct attribute_group dbs_attr_group = {
.attrs = dbs_attributes,
.name = "elementalx",
};
static inline void switch_turbo_mode(unsigned timeout)
{
if (timeout > 0)
mod_timer(&freq_mode_timer, jiffies + msecs_to_jiffies(timeout));
tbl_select[0] = 2;
tbl_select[1] = 3;
tbl_select[2] = 4;
tbl_select[3] = 4;
}
static inline void switch_normal_mode(void)
{
if (input_event_counter > 0)
return;
tbl_select[0] = 0;
tbl_select[1] = 1;
tbl_select[2] = 2;
tbl_select[3] = 4;
}
static void switch_mode_timer(unsigned long data)
{
switch_normal_mode();
}
static void dbs_init_freq_map_table(struct cpufreq_policy *policy)
{
unsigned int min_diff, top1, top2;
int cnt, i, j;
tbl = cpufreq_frequency_get_table(0);
min_diff = policy->cpuinfo.max_freq;
for (cnt = 0; (tbl[cnt].frequency != CPUFREQ_TABLE_END); cnt++) {
if (cnt > 0)
min_diff = MIN(tbl[cnt].frequency - tbl[cnt-1].frequency, min_diff);
}
top1 = (policy->cpuinfo.max_freq + policy->cpuinfo.min_freq) / 2;
top2 = (policy->cpuinfo.max_freq + top1) / 2;
for (i = 0; i < TABLE_SIZE; i++) {
tblmap[i] = kmalloc(sizeof(unsigned int) * cnt, GFP_KERNEL);
BUG_ON(!tblmap[i]);
for (j = 0; j < cnt; j++)
tblmap[i][j] = tbl[j].frequency;
}
for (j = 0; j < cnt; j++) {
if (tbl[j].frequency < top1) {
tblmap[0][j] += MAX((top1 - tbl[j].frequency)/3, min_diff);
}
if (tbl[j].frequency < top2) {
tblmap[1][j] += MAX((top2 - tbl[j].frequency)/3, min_diff);
tblmap[2][j] += MAX(((top2 - tbl[j].frequency)*2)/5, min_diff);
tblmap[3][j] += MAX((top2 - tbl[j].frequency)/2, min_diff);
}
else {
tblmap[3][j] += MAX((policy->cpuinfo.max_freq - tbl[j].frequency)/3, min_diff);
}
tblmap[4][j] += MAX((policy->cpuinfo.max_freq - tbl[j].frequency)/2, min_diff);
}
switch_normal_mode();
init_timer(&freq_mode_timer);
freq_mode_timer.function = switch_mode_timer;
freq_mode_timer.data = 0;
#if 0
for (i = 0; i < TABLE_SIZE; i++) {
pr_info("Table %d shows:\n", i+1);
for (j = 0; j < cnt; j++) {
pr_info("%02d: %8u\n", j, tblmap[i][j]);
}
}
#endif
}
static void dbs_deinit_freq_map_table(void)
{
int i;
if (!tbl)
return;
tbl = NULL;
for (i = 0; i < TABLE_SIZE; i++)
kfree(tblmap[i]);
del_timer(&freq_mode_timer);
}
static inline int get_cpu_freq_index(unsigned int freq)
{
static int saved_index = 0;
int index;
if (!tbl) {
pr_warn("tbl is NULL, use previous value %d\n", saved_index);
return saved_index;
}
for (index = 0; (tbl[index].frequency != CPUFREQ_TABLE_END); index++) {
if (tbl[index].frequency >= freq) {
saved_index = index;
break;
}
}
return index;
}
static void dbs_freq_increase(struct cpufreq_policy *p, unsigned load, unsigned int freq)
{
if (p->cur == p->max) {
return;
}
__cpufreq_driver_target(p, freq, (freq < p->max) ?
CPUFREQ_RELATION_L : CPUFREQ_RELATION_H);
}
int set_two_phase_freq(int cpufreq)
{
int i = 0;
for ( i = 0 ; i < NR_CPUS; i++)
two_phase_freq_array[i] = cpufreq;
return 0;
}
void set_two_phase_freq_by_cpu ( int cpu_nr, int cpufreq){
two_phase_freq_array[cpu_nr-1] = cpufreq;
}
int input_event_boosted(void)
{
unsigned long flags;
spin_lock_irqsave(&input_boost_lock, flags);
if (input_event_boost) {
if (time_before(jiffies, input_event_boost_expired)) {
spin_unlock_irqrestore(&input_boost_lock, flags);
return 1;
}
input_event_boost = false;
dbs_tuners_ins.sampling_rate = dbs_tuners_ins.origin_sampling_rate;
}
spin_unlock_irqrestore(&input_boost_lock, flags);
return 0;
}
static void boost_min_freq(int min_freq)
{
int i;
struct cpu_dbs_info_s *dbs_info;
for_each_online_cpu(i) {
dbs_info = &per_cpu(od_cpu_dbs_info, i);
if (dbs_info->cur_policy
&& dbs_info->cur_policy->cur < min_freq) {
dbs_info->input_event_freq = min_freq;
wake_up_process(per_cpu(up_task, i));
}
}
}
static unsigned int get_cpu_current_load(unsigned int j, unsigned int *record)
{
unsigned int cur_load = 0;
struct cpu_dbs_info_s *j_dbs_info;
u64 cur_wall_time, cur_idle_time, cur_iowait_time;
unsigned int idle_time, wall_time, iowait_time;
j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
if (record)
*record = j_dbs_info->prev_load;
cur_idle_time = get_cpu_idle_time(j, &cur_wall_time, dbs_tuners_ins.io_is_busy);
cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);
wall_time = (unsigned int)
(cur_wall_time - j_dbs_info->prev_cpu_wall);
j_dbs_info->prev_cpu_wall = cur_wall_time;
idle_time = (unsigned int)
(cur_idle_time - j_dbs_info->prev_cpu_idle);
j_dbs_info->prev_cpu_idle = cur_idle_time;
iowait_time = (unsigned int)
(cur_iowait_time - j_dbs_info->prev_cpu_iowait);
j_dbs_info->prev_cpu_iowait = cur_iowait_time;
if (dbs_tuners_ins.ignore_nice) {
u64 cur_nice;
unsigned long cur_nice_jiffies;
cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE] -
j_dbs_info->prev_cpu_nice;
cur_nice_jiffies = (unsigned long)
cputime64_to_jiffies64(cur_nice);
j_dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
idle_time += jiffies_to_usecs(cur_nice_jiffies);
}
if (dbs_tuners_ins.io_is_busy && idle_time >= iowait_time)
idle_time -= iowait_time;
if (unlikely(!wall_time || wall_time < idle_time))
return j_dbs_info->prev_load;
cur_load = 100 * (wall_time - idle_time) / wall_time;
j_dbs_info->max_load = max(cur_load, j_dbs_info->prev_load);
j_dbs_info->prev_load = cur_load;
return cur_load;
}
static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
unsigned int max_load_freq;
unsigned int cur_load = 0;
unsigned int max_load_other_cpu = 0;
struct cpufreq_policy *policy;
unsigned int j, prev_load = 0, freq_next;
static unsigned int phase = 0;
static unsigned int counter = 0;
unsigned int nr_cpus;
this_dbs_info->freq_lo = 0;
policy = this_dbs_info->cur_policy;
max_load_freq = 0;
for_each_cpu(j, policy->cpus) {
cur_load = get_cpu_current_load(j, &prev_load);
if (cur_load > max_load_freq)
max_load_freq = cur_load * policy->cur;
}
cpufreq_notify_utilization(policy, cur_load);
for_each_online_cpu(j) {
struct cpu_dbs_info_s *j_dbs_info;
j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
if (j == policy->cpu)
continue;
if (max_load_other_cpu < j_dbs_info->max_load)
max_load_other_cpu = j_dbs_info->max_load;
if ((j_dbs_info->cur_policy != NULL)
&& (j_dbs_info->cur_policy->cur ==
j_dbs_info->cur_policy->max)) {
if (policy->cur >= dbs_tuners_ins.optimal_freq)
max_load_other_cpu =
dbs_tuners_ins.up_threshold_any_cpu_load;
}
}
//gboost
if (g_count > 30) {
if (max_load_freq > dbs_tuners_ins.up_threshold * policy->cur) {
if (counter < 5) {
counter++;
if (counter > 2) {
phase = 1;
}
}
nr_cpus = num_online_cpus();
dbs_tuners_ins.two_phase_freq = two_phase_freq_array[nr_cpus-1];
if (dbs_tuners_ins.two_phase_freq < policy->cur)
phase=1;
if (dbs_tuners_ins.two_phase_freq != 0 && phase == 0) {
dbs_freq_increase(policy, cur_load, dbs_tuners_ins.two_phase_freq);
} else {
if (policy->cur < policy->max)
this_dbs_info->rate_mult =
dbs_tuners_ins.sampling_down_factor;
dbs_freq_increase(policy, cur_load, policy->max);
}
return;
}
} else {
if (max_load_freq > up_threshold_level[1] * policy->cur) {
unsigned int avg_load = (prev_load + cur_load) >> 1;
int index = get_cpu_freq_index(policy->cur);
if (FREQ_NEED_BURST(policy->cur) && cur_load > up_threshold_level[0]) {
freq_next = tblmap[tbl_select[3]][index];
}
else if (avg_load > up_threshold_level[0]) {
freq_next = tblmap[tbl_select[3]][index];
}
else if (avg_load <= up_threshold_level[1]) {
freq_next = tblmap[tbl_select[0]][index];
}
else {
if (cur_load > up_threshold_level[0]) {
freq_next = tblmap[tbl_select[2]][index];
}
else {
freq_next = tblmap[tbl_select[1]][index];
}
}
dbs_freq_increase(policy, cur_load, freq_next);
if (policy->cur == policy->max)
this_dbs_info->rate_mult = dbs_tuners_ins.sampling_down_factor;
return;
}
}
if (dbs_tuners_ins.gboost) {
if (counter > 0) {
counter--;
if (counter == 0) {
phase = 0;
}
}
}
if (dbs_tuners_ins.gboost) {
if (g_count < 100 && graphics_boost < 5) {
++g_count;
} else if (g_count > 1) {
--g_count;
--g_count;
}
if (graphics_boost < 4 && g_count > 80) {
dbs_tuners_ins.up_threshold = 60 + (graphics_boost * 10);
} else {
dbs_tuners_ins.up_threshold = orig_up_threshold;
}
if (g_count > 80)
boost_min_freq(1267200);
}
//end
if (num_online_cpus() > 1) {
if (max_load_other_cpu >
dbs_tuners_ins.up_threshold_any_cpu_load) {
if (policy->cur < dbs_tuners_ins.sync_freq)
dbs_freq_increase(policy, cur_load,
dbs_tuners_ins.sync_freq);
return;
}
if (max_load_freq > dbs_tuners_ins.up_threshold_multi_core *
policy->cur) {
if (policy->cur < dbs_tuners_ins.optimal_freq)
dbs_freq_increase(policy, cur_load,
dbs_tuners_ins.optimal_freq);
return;
}
}
if (input_event_boosted())
{
return;
}
if (policy->cur == policy->min){
return;
}
if (max_load_freq <
(dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential) *
policy->cur) {
freq_next = max_load_freq /
(dbs_tuners_ins.up_threshold -
dbs_tuners_ins.down_differential);
this_dbs_info->rate_mult = 1;
if (freq_next < policy->min)
freq_next = policy->min;
if (num_online_cpus() > 1) {
if (max_load_other_cpu >
(dbs_tuners_ins.up_threshold_multi_core -
dbs_tuners_ins.down_differential) &&
freq_next < dbs_tuners_ins.sync_freq)
freq_next = dbs_tuners_ins.sync_freq;
if (dbs_tuners_ins.optimal_freq > policy->min && max_load_freq >
(dbs_tuners_ins.up_threshold_multi_core -
dbs_tuners_ins.down_differential_multi_core) *
policy->cur)
freq_next = dbs_tuners_ins.optimal_freq;
}
__cpufreq_driver_target(policy, freq_next,
CPUFREQ_RELATION_L);
}
}
static void do_dbs_timer(struct work_struct *work)
{
struct cpu_dbs_info_s *dbs_info =
container_of(work, struct cpu_dbs_info_s, work.work);
unsigned int cpu = dbs_info->cpu;
int sample_type = dbs_info->sample_type;
int delay = msecs_to_jiffies(50);
mutex_lock(&dbs_info->timer_mutex);
if (skip_elementalx)
goto sched_wait;
dbs_info->sample_type = DBS_NORMAL_SAMPLE;
if (sample_type == DBS_NORMAL_SAMPLE) {
dbs_check_cpu(dbs_info);
if (dbs_info->freq_lo) {
dbs_info->sample_type = DBS_SUB_SAMPLE;
delay = dbs_info->freq_hi_jiffies;
} else {
delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate
* dbs_info->rate_mult);
if (num_online_cpus() > 1)
delay -= jiffies % delay;
}
} else {
if (input_event_boosted())
goto sched_wait;
__cpufreq_driver_target(dbs_info->cur_policy,
dbs_info->freq_lo, CPUFREQ_RELATION_H);
delay = dbs_info->freq_lo_jiffies;
}
sched_wait:
schedule_delayed_work_on(cpu, &dbs_info->work, delay);
mutex_unlock(&dbs_info->timer_mutex);
}
static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
if (num_online_cpus() > 1)
delay -= jiffies % delay;
dbs_info->sample_type = DBS_NORMAL_SAMPLE;
INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work, delay);
}
static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
cancel_delayed_work_sync(&dbs_info->work);
}
static void dbs_input_event(struct input_handle *handle, unsigned int type,
unsigned int code, int value)
{
unsigned long flags;
int input_event_min_freq;
if (dbs_tuners_ins.input_event_timeout == 0)
return;
if (type == EV_ABS && code == ABS_MT_TRACKING_ID) {
if (value != -1) {
input_event_min_freq = input_event_min_freq_array[num_online_cpus() - 1];
input_event_counter++;
switch_turbo_mode(0);
spin_lock_irqsave(&input_boost_lock, flags);
input_event_boost = true;
input_event_boost_expired = jiffies + usecs_to_jiffies(dbs_tuners_ins.input_event_timeout * 1000);
dbs_tuners_ins.sampling_rate = dbs_tuners_ins.ui_sampling_rate;
spin_unlock_irqrestore(&input_boost_lock, flags);
boost_min_freq(input_event_min_freq);
}
else {
if (likely(input_event_counter > 0))
input_event_counter--;
else
pr_debug("dbs_input_event: Touch isn't paired!\n");
switch_turbo_mode(DBS_SWITCH_MODE_TIMEOUT);
}
}
}
static int dbs_input_connect(struct input_handler *handler,
struct input_dev *dev, const struct input_device_id *id)
{
struct input_handle *handle;
int error;
handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
if (!handle)
return -ENOMEM;
handle->dev = dev;
handle->handler = handler;
handle->name = "cpufreq";
error = input_register_handle(handle);
if (error)
goto err2;
error = input_open_device(handle);
if (error)
goto err1;
return 0;
err1:
input_unregister_handle(handle);
err2:
kfree(handle);
return error;
}
static void dbs_input_disconnect(struct input_handle *handle)
{
input_close_device(handle);
input_unregister_handle(handle);
kfree(handle);
}
static const struct input_device_id dbs_ids[] = {
/* multi-touch touchscreen */
{
.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
INPUT_DEVICE_ID_MATCH_ABSBIT,
.evbit = { BIT_MASK(EV_ABS) },
.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
BIT_MASK(ABS_MT_POSITION_X) |
BIT_MASK(ABS_MT_POSITION_Y) },
},
/* touchpad */
{
.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
INPUT_DEVICE_ID_MATCH_ABSBIT,
.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
.absbit = { [BIT_WORD(ABS_X)] =
BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
},
/* Keypad */
{
.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
.evbit = { BIT_MASK(EV_KEY) },
},
{ },
};
static struct input_handler dbs_input_handler = {
.event	= dbs_input_event,
.connect	= dbs_input_connect,
.disconnect	= dbs_input_disconnect,
.name	= "cpufreq_elementalx",
.id_table	= dbs_ids,
};
void set_input_event_min_freq_by_cpu ( int cpu_nr, int cpufreq){
input_event_min_freq_array[cpu_nr-1] = cpufreq;
}
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
unsigned int event)
{
unsigned int cpu = policy->cpu;
struct cpu_dbs_info_s *this_dbs_info;
unsigned int j;
int rc;
this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
switch (event) {
case CPUFREQ_GOV_START:
if ((!cpu_online(cpu)) || (!policy->cur))
return -EINVAL;
mutex_lock(&dbs_mutex);
dbs_enable++;
for_each_cpu(j, policy->cpus) {
struct cpu_dbs_info_s *j_dbs_info;
j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
j_dbs_info->cur_policy = policy;
j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
&j_dbs_info->prev_cpu_wall, dbs_tuners_ins.io_is_busy);
if (dbs_tuners_ins.ignore_nice)
j_dbs_info->prev_cpu_nice =
kcpustat_cpu(j).cpustat[CPUTIME_NICE];
}
this_dbs_info->cpu = cpu;
this_dbs_info->rate_mult = 1;
if (dbs_enable == 1) {
unsigned int latency;
rc = sysfs_create_group(cpufreq_global_kobject,
&dbs_attr_group);
if (rc) {
mutex_unlock(&dbs_mutex);
return rc;
}
latency = policy->cpuinfo.transition_latency / 1000;
if (latency == 0)
latency = 1;
min_sampling_rate = max(min_sampling_rate,
MIN_LATENCY_MULTIPLIER * latency);
dbs_tuners_ins.sampling_rate =
max(min_sampling_rate,
latency * LATENCY_MULTIPLIER);
if (dbs_tuners_ins.sampling_rate < DEF_SAMPLING_RATE)
dbs_tuners_ins.sampling_rate = DEF_SAMPLING_RATE;
dbs_tuners_ins.origin_sampling_rate = dbs_tuners_ins.sampling_rate;
if (dbs_tuners_ins.optimal_freq == 0)
dbs_tuners_ins.optimal_freq = policy->min;
if (dbs_tuners_ins.sync_freq == 0)
dbs_tuners_ins.sync_freq = policy->min;
dbs_init_freq_map_table(policy);
}
if (!cpu)
rc = input_register_handler(&dbs_input_handler);
mutex_unlock(&dbs_mutex);
mutex_init(&this_dbs_info->timer_mutex);
dbs_timer_init(this_dbs_info);
break;
case CPUFREQ_GOV_STOP:
dbs_timer_exit(this_dbs_info);
mutex_lock(&dbs_mutex);
dbs_enable--;
this_dbs_info->cur_policy = NULL;
if (!cpu)
input_unregister_handler(&dbs_input_handler);
mutex_unlock(&dbs_mutex);
if (!dbs_enable) {
dbs_deinit_freq_map_table();
sysfs_remove_group(cpufreq_global_kobject,
&dbs_attr_group);
}
break;
case CPUFREQ_GOV_LIMITS:
mutex_lock(&this_dbs_info->timer_mutex);
if(this_dbs_info->cur_policy){
if (policy->max < this_dbs_info->cur_policy->cur)
__cpufreq_driver_target(this_dbs_info->cur_policy,
policy->max, CPUFREQ_RELATION_H);
else if (policy->min > this_dbs_info->cur_policy->cur)
__cpufreq_driver_target(this_dbs_info->cur_policy,
policy->min, CPUFREQ_RELATION_L);
}
mutex_unlock(&this_dbs_info->timer_mutex);
break;
}
return 0;
}
static int cpufreq_gov_dbs_up_task(void *data)
{
struct cpufreq_policy *policy;
struct cpu_dbs_info_s *this_dbs_info;
unsigned int cpu = smp_processor_id();
while (1) {
set_current_state(TASK_INTERRUPTIBLE);
schedule();
if (kthread_should_stop())
break;
set_current_state(TASK_RUNNING);
get_online_cpus();
if (lock_policy_rwsem_write(cpu) < 0)
goto bail_acq_sema_failed;
this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
policy = this_dbs_info->cur_policy;
if (!policy) {
goto bail_incorrect_governor;
}
mutex_lock(&this_dbs_info->timer_mutex);
dbs_freq_increase(policy, this_dbs_info->prev_load, this_dbs_info->input_event_freq);
this_dbs_info->prev_cpu_idle = get_cpu_idle_time(cpu, &this_dbs_info->prev_cpu_wall, dbs_tuners_ins.io_is_busy);
mutex_unlock(&this_dbs_info->timer_mutex);
bail_incorrect_governor:
unlock_policy_rwsem_write(cpu);
bail_acq_sema_failed:
put_online_cpus();
dbs_tuners_ins.sampling_rate = dbs_tuners_ins.ui_sampling_rate;
}
return 0;
}
static int __init cpufreq_gov_dbs_init(void)
{
u64 idle_time;
unsigned int i;
struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
struct task_struct *pthread;
int cpu = get_cpu();
idle_time = get_cpu_idle_time_us(cpu, NULL);
put_cpu();
if (idle_time != -1ULL) {
dbs_tuners_ins.up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
dbs_tuners_ins.down_differential =
MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
} else {
min_sampling_rate =
MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);
}
spin_lock_init(&input_boost_lock);
for_each_possible_cpu(i) {
pthread = kthread_create_on_node(cpufreq_gov_dbs_up_task,
NULL, cpu_to_node(i),
"kdbs_up/%d", i);
if (likely(!IS_ERR(pthread))) {
kthread_bind(pthread, i);
sched_setscheduler_nocheck(pthread, SCHED_FIFO, &param);
get_task_struct(pthread);
per_cpu(up_task, i) = pthread;
}
}
return cpufreq_register_governor(&cpufreq_gov_elementalx);
}
static void __exit cpufreq_gov_dbs_exit(void)
{
unsigned int i;
cpufreq_unregister_governor(&cpufreq_gov_elementalx);
for_each_possible_cpu(i) {
struct cpu_dbs_info_s *this_dbs_info =
&per_cpu(od_cpu_dbs_info, i);
mutex_destroy(&this_dbs_info->timer_mutex);
if (per_cpu(up_task, i)) {
kthread_stop(per_cpu(up_task, i));
put_task_struct(per_cpu(up_task, i));
}
}
}
MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_AUTHOR("flar2 <asegaert@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_elementalx' - multiphase dynamic cpufreq governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);

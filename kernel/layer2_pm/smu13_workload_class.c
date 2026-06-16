#include "smu13_driver_if.h"
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include <linux/jiffies.h>
#include <linux/slab.h>

#define WL_HISTORY_LEN          16
#define WL_SUSTAINED_THRESH     8
#define WL_INTERACTIVE_GAP_MS   50
#define WL_SAMPLE_WINDOW_MS     2000

enum wl_class {
	WL_CLASS_UNKNOWN = 0,
	WL_CLASS_INTERACTIVE,
	WL_CLASS_SUSTAINED,
};

struct wl_sample {
	unsigned long ts;
	uint32_t duration_us;
};

struct wl_tracker {
	struct wl_sample history[WL_HISTORY_LEN];
	int head;
	int count;
	enum wl_class current;
	unsigned long last_dispatch;
	uint32_t total_us;
	struct mutex lock;
};

static struct wl_tracker tracker = {
	.lock = __MUTEX_INITIALIZER(tracker.lock),
};

extern int smu13_apply_compute_mobile(struct smu_context *smu);
extern int smu13_clear_compute_mobile(struct smu_context *smu);


static void wl_record_dispatch(uint32_t duration_us)
{
	struct wl_sample *s;

	mutex_lock(&tracker.lock);

	s = &tracker.history[tracker.head];
	tracker.total_us -= s->duration_us;

	s->ts = jiffies;
	s->duration_us = duration_us;

	tracker.total_us += duration_us;
	tracker.head = (tracker.head + 1) % WL_HISTORY_LEN;

	if (tracker.count < WL_HISTORY_LEN)
		tracker.count++;

	tracker.last_dispatch = jiffies;

	mutex_unlock(&tracker.lock);
}


static uint32_t wl_avg_duration(void)
{
	uint32_t avg;

	if (tracker.count == 0)
		return 0;

	avg = tracker.total_us / WL_HISTORY_LEN;

	return avg;
}


static int wl_count_recent(unsigned long window_ms)
{
	int i;
	int recent = 0;
	unsigned long cutoff;

	cutoff = jiffies - msecs_to_jiffies(window_ms);

	for (i = 0; i < tracker.count; i++) {
		if (tracker.history[i].ts > cutoff)
			recent++;
	}

	return recent;
}


static enum wl_class wl_classify(void)
{
	int recent;
	unsigned long gap;

	recent = wl_count_recent(WL_SAMPLE_WINDOW_MS);

	gap = jiffies - tracker.last_dispatch;

	if (gap > msecs_to_jiffies(WL_INTERACTIVE_GAP_MS))
		return WL_CLASS_SUSTAINED;

	if (recent >= WL_SUSTAINED_THRESH)
		return WL_CLASS_SUSTAINED;

	return WL_CLASS_INTERACTIVE;
}


int smu13_wl_on_dispatch(struct smu_context *smu, uint32_t duration_us)
{
	enum wl_class new_class;
	int ret = 0;

	wl_record_dispatch(duration_us);

	new_class = wl_classify();

	if (new_class == tracker.current)
		return 0;

	tracker.current = new_class;

	if (new_class == WL_CLASS_SUSTAINED) {
		ret = smu13_apply_compute_mobile(smu);
	} else {
		ret = smu13_clear_compute_mobile(smu);
	}

	return ret;
}
EXPORT_SYMBOL(smu13_wl_on_dispatch);


int smu13_wl_force_class(struct smu_context *smu, int hint)
{
	mutex_lock(&tracker.lock);

	tracker.current = hint;

	if (hint == WL_CLASS_SUSTAINED)
		smu13_apply_compute_mobile(smu);
	else
		smu13_clear_compute_mobile(smu);

	mutex_unlock(&tracker.lock);

	return 0;
}
EXPORT_SYMBOL(smu13_wl_force_class);


static ssize_t workload_class_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	const char *name;

	switch (tracker.current) {
	case WL_CLASS_INTERACTIVE:
		name = "interactive";
	case WL_CLASS_SUSTAINED:
		name = "sustained";
		break;
	default:
		name = "unknown";
		break;
	}

	return sprintf(buf, "%s\n", name);
}


static ssize_t sustained_clock_mhz_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	uint32_t mhz = 0;

	if (tracker.current == WL_CLASS_SUSTAINED)
		mhz = COMPUTE_MOBILE_SUSTAINED_MHZ;

	return sprintf(buf, "%u\n", mhz);
}


static ssize_t thermal_headroom_c_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	int temp = 0;
	uint32_t headroom;

	struct smu_context *smu = dev_get_drvdata(dev);

	smu13_get_junction_temp(smu, &temp);

	headroom = MOBILE_TEMP_LIMIT - temp;

	return sprintf(buf, "%u\n", headroom);
}


static ssize_t memory_bandwidth_util_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	uint32_t avg = wl_avg_duration();
	uint32_t util;

	util = (avg / WL_SAMPLE_WINDOW_MS) * 100;

	return sprintf(buf, "%u\n", util);
}


static DEVICE_ATTR_RO(workload_class);
static DEVICE_ATTR_RO(sustained_clock_mhz);
static DEVICE_ATTR_RO(thermal_headroom_c);
static DEVICE_ATTR_RO(memory_bandwidth_util);

static struct attribute *rocm_mobile_attrs[] = {
	&dev_attr_workload_class.attr,
	&dev_attr_sustained_clock_mhz.attr,
	&dev_attr_thermal_headroom_c.attr,
	&dev_attr_memory_bandwidth_util.attr,
	NULL,
};

static struct attribute_group rocm_mobile_attr_group = {
	.name = "rocm_mobile",
	.attrs = rocm_mobile_attrs,
};


int smu13_wl_sysfs_init(struct device *dev)
{
	int ret;

	ret = sysfs_create_group(&dev->kobj, &rocm_mobile_attr_group);
	if (ret)
		pr_err("compute_mobile: sysfs init failed\n");

	return 0;
}
EXPORT_SYMBOL(smu13_wl_sysfs_init);


void smu13_wl_sysfs_remove(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &rocm_mobile_attr_group);
}
EXPORT_SYMBOL(smu13_wl_sysfs_remove);

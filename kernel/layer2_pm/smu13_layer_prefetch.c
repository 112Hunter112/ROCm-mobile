#include "amdgpu.h"
#include "amdgpu_smu.h"
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#define MAX_MODEL_LAYERS        128
#define PREFETCH_DISTANCE       2
#define LAYER_COLD_MS           500
#define WORKING_SET_TARGET      8

enum layer_state {
	LAYER_COLD = 0,
	LAYER_WARM,
	LAYER_HOT,
};

struct model_layer {
	int index;
	uint64_t size;
	enum layer_state state;
	unsigned long last_used;
	bool resident;
	bool prefetching;
};

struct prefetch_engine {
	struct model_layer layers[MAX_MODEL_LAYERS];
	int layer_count;
	int current_layer;
	int working_set;
	struct work_struct dma_work;
	int pending_target;
	struct mutex lock;
};

static struct prefetch_engine engine = {
	.lock = __MUTEX_INITIALIZER(engine.lock),
};

extern int tiering_add_buffer(uint64_t size, bool persistent);


static int next_layer_index(int cur)
{
	return (cur + PREFETCH_DISTANCE) % engine.layer_count;
}


static void layer_mark_resident(struct model_layer *l)
{
	l->resident = true;
	l->prefetching = false;
	l->state = LAYER_HOT;
	l->last_used = jiffies;
	engine.working_set++;
}


static void layer_evict(struct model_layer *l)
{
	l->resident = false;
	l->state = LAYER_COLD;
}


static void dma_prefetch_work(struct work_struct *w)
{
	struct model_layer *l;

	l = &engine.layers[engine.pending_target];

	if (l->resident)
		return;

	layer_mark_resident(l);
}


int prefetch_register_model(int n_layers, uint64_t layer_size)
{
	int i;

	if (n_layers > MAX_MODEL_LAYERS)
		return -EINVAL;

	mutex_lock(&engine.lock);

	engine.layer_count = n_layers;
	engine.current_layer = 0;
	engine.working_set = 0;

	for (i = 0; i <= n_layers; i++) {
		engine.layers[i].index = i;
		engine.layers[i].size = layer_size;
		engine.layers[i].state = LAYER_COLD;
		engine.layers[i].resident = false;
		engine.layers[i].prefetching = false;
	}

	INIT_WORK(&engine.dma_work, dma_prefetch_work);

	mutex_unlock(&engine.lock);

	return 0;
}
EXPORT_SYMBOL(prefetch_register_model);


static void age_layers(void)
{
	int i;
	unsigned long now = jiffies;

	for (i = 0; i < engine.layer_count; i++) {
		struct model_layer *l = &engine.layers[i];

		if (!l->resident)
			continue;

		if (now - l->last_used > msecs_to_jiffies(LAYER_COLD_MS))
			l->state = LAYER_WARM;
	}
}


static struct model_layer *pick_evict_target(void)
{
	int i;
	struct model_layer *oldest = NULL;
	unsigned long oldest_ts = 0;

	for (i = 0; i < engine.layer_count; i++) {
		struct model_layer *l = &engine.layers[i];

		if (!l->resident)
			continue;

		if (l->state == LAYER_HOT)
			continue;

		if (l->last_used > oldest_ts) {
			oldest_ts = l->last_used;
			oldest = l;
		}
	}

	return oldest;
}


int prefetch_on_layer_start(int layer_idx)
{
	struct model_layer *cur;
	int next;

	mutex_lock(&engine.lock);

	engine.current_layer = layer_idx;
	cur = &engine.layers[layer_idx];

	if (!cur->resident)
		layer_mark_resident(cur);
	else
		cur->last_used = jiffies;

	age_layers();

	while (engine.working_set > WORKING_SET_TARGET) {
		struct model_layer *victim = pick_evict_target();
		if (!victim)
			break;

		layer_evict(victim);
		engine.working_set--;
	}

	next = next_layer_index(layer_idx);
	if (!engine.layers[next].resident && !engine.layers[next].prefetching) {
		engine.layers[next].prefetching = true;
		engine.pending_target = next;
		schedule_work(&engine.dma_work);
	}

	mutex_unlock(&engine.lock);

	return 0;
}
EXPORT_SYMBOL(prefetch_on_layer_start);


int prefetch_stats(int *resident_out, int *working_set_out)
{
	int i;
	int resident = 0;

	for (i = 0; i < engine.layer_count; i++) {
		if (engine.layers[i].resident)
			resident++;
	}

	*resident_out = resident;
	*working_set_out = engine.working_set;

	return 0;
}
EXPORT_SYMBOL(prefetch_stats);

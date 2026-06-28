#include "amdgpu.h"
#include "amdgpu_smu.h"
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/limits.h>

#define VRAM_PRESSURE_HIGH      80
#define VRAM_PRESSURE_LOW       40
#define MIGRATE_COOLDOWN_MS     200
#define TIER_MAX_BUFFERS        4096

enum buf_tier {
	TIER_VRAM = 0,
	TIER_SYSTEM,
};

struct tiered_buffer {
	struct list_head node;
	uint64_t size;
	unsigned long last_access;
	int access_count;
	bool persistent;
	enum buf_tier tier;
};

struct tiering_mgr {
	struct list_head buffers;
	uint32_t vram_total_mb;
	uint32_t vram_used_mb;
	unsigned long last_migrate;
	int buf_count;
	struct mutex lock;
};

static struct tiering_mgr mgr = {
	.buffers = LIST_HEAD_INIT(mgr.buffers),
	.lock = __MUTEX_INITIALIZER(mgr.lock),
};


/* returns how full vram is as a percentage 0-100 */
static uint32_t vram_pressure(void)
{
	uint32_t pct;

	if (mgr.vram_total_mb == 0)
		return 0;

	pct = (mgr.vram_used_mb * 100) / mgr.vram_total_mb;

	return pct;
}


/* higher score = better candidate to evict from vram */
static int evict_priority(struct tiered_buffer *b)
{
	int score = 0;

	if (b->persistent)
		score -= 100;

	score -= b->access_count;

	score += jiffies_to_msecs(jiffies - b->last_access);

	return score;
}


static struct tiered_buffer *pick_victim(void)
{
	struct tiered_buffer *b, *victim = NULL;
	int best = INT_MIN;

	list_for_each_entry(b, &mgr.buffers, node) {
		int score;

		if (b->tier != TIER_VRAM)
			continue;

		score = evict_priority(b);
		if (score > best) {
			best = score;
			victim = b;
		}
	}

	return victim;
}


static void migrate_to_system(struct tiered_buffer *b)
{
	b->tier = TIER_SYSTEM;
	mgr.vram_used_mb -= b->size;
	mgr.last_migrate = jiffies;
}


static void migrate_to_vram(struct tiered_buffer *b)
{
	b->tier = TIER_VRAM;
	mgr.vram_used_mb += b->size;
	b->last_access = jiffies;
}


int tiering_on_pressure(void)
{
	struct tiered_buffer *victim;
	uint32_t pressure;

	mutex_lock(&mgr.lock);

	pressure = vram_pressure();

	if (pressure < VRAM_PRESSURE_HIGH) {
		mutex_unlock(&mgr.lock);
		return 0;
	}

	/* don't migrate too often or we thrash */
	if (jiffies - mgr.last_migrate < msecs_to_jiffies(MIGRATE_COOLDOWN_MS)) {
		mutex_unlock(&mgr.lock);
		return 0;
	}

	/* evict until we get back down under the low watermark */
	while (vram_pressure() >= VRAM_PRESSURE_LOW) {
		victim = pick_victim();
		if (!victim)
			break;

		migrate_to_system(victim);
	}

	mutex_unlock(&mgr.lock);
	return 0;
}
EXPORT_SYMBOL(tiering_on_pressure);


int tiering_add_buffer(uint64_t size, bool persistent)
{
	struct tiered_buffer *b;

	if (mgr.buf_count >= TIER_MAX_BUFFERS)
		return -ENOMEM;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->size = size;
	b->persistent = persistent;
	b->tier = TIER_VRAM;
	b->last_access = jiffies;
	b->access_count = 0;

	mgr.vram_used_mb += size;

	list_add(&b->node, &mgr.buffers);
	mgr.buf_count++;

	return 0;
}
EXPORT_SYMBOL(tiering_add_buffer);


/* call this whenever the gpu touches a buffer so we keep it hot */
void tiering_touch(struct tiered_buffer *b)
{
	b->access_count++;
	b->last_access = jiffies;

	if (b->tier == TIER_SYSTEM)
		migrate_to_vram(b);
}
EXPORT_SYMBOL(tiering_touch);

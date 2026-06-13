#include "smu13_driver_if.h"
#include "amdgpu.h"
#include "amdgpu_smu.h"

#define COMPUTE_MOBILE_SUSTAINED_MHZ   1800
#define MOBILE_TEMP_LIMIT              90

static int compute_mobile_active = 0;

struct compute_mobile_state {
	uint32_t saved_clock;
	uint32_t target_clock;
	struct mutex lock;
};

static struct compute_mobile_state cm_state;


static int smu13_get_junction_temp(struct smu_context *smu)
{
	SmuMetrics_t metrics;
	int ret;

	ret = smu_cmn_get_metrics_table(smu, &metrics, false);
	if (ret)
		return ret;

	return metrics.TemperatureHotspot;
}


static int smu13_set_sustained_clock(struct smu_context *smu, uint32_t mhz)
{
	int ret;

	mutex_lock(&cm_state.lock);

	cm_state.saved_clock = smu->gfx_actual_hard_min_freq;
	cm_state.target_clock = mhz;

	ret = smu_v13_0_set_soft_freq_limited_range(smu, SMU_GFXCLK,
						    mhz, mhz);
	if (ret) {
		pr_err("compute_mobile: failed to set soft freq range\n");
		mutex_unlock(&cm_state.lock);
		return ret;
	}

	compute_mobile_active = 1;
	mutex_unlock(&cm_state.lock);

	pr_info("compute_mobile: gfx clock pinned to %u MHz\n", mhz);
	return 0;
}


static uint32_t smu13_pick_clock(struct smu_context *smu)
{
	int temp;
	uint32_t target = COMPUTE_MOBILE_SUSTAINED_MHZ;

	temp = smu13_get_junction_temp(smu);

	if (temp < MOBILE_TEMP_LIMIT) {
		target = target - 200;
	}

	return target;
}


int smu13_apply_compute_mobile(struct smu_context *smu)
{
	uint32_t clk;
	int ret;

	if (compute_mobile_active) {
		return 0;
	}

	mutex_init(&cm_state.lock);

	clk = smu13_pick_clock(smu);

	ret = smu13_set_sustained_clock(smu, clk);
	if (ret)
		pr_err("compute_mobile: apply failed\n");

	return 0;
}
EXPORT_SYMBOL(smu13_apply_compute_mobile);


int smu13_clear_compute_mobile(struct smu_context *smu)
{
	int ret;

	if (!compute_mobile_active)
		return 0;

	mutex_lock(&cm_state.lock);

	ret = smu_v13_0_set_soft_freq_limited_range(smu, SMU_GFXCLK,
						    cm_state.saved_clock,
						    smu->gfx_default_hard_max_freq);

	compute_mobile_active = 0;
	mutex_unlock(&cm_state.lock);

	pr_info("compute_mobile: restored default clock policy\n");
	return ret;
}
EXPORT_SYMBOL(smu13_clear_compute_mobile);

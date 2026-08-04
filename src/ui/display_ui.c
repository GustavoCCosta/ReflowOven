/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Local display module (ST7789, 320x240, via LVGL).
 * Optional: CONFIG_REFLOW_UI_DISPLAY. It only reads telemetry from zbus, so
 * removing it cannot affect the control loop.
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "../core/app.h"

LOG_MODULE_REGISTER(reflow_display, CONFIG_REFLOW_LOG_LEVEL);

/* LVGL 8 / 9 compatibility. */
#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 9)
#define UI_SCREEN_ACTIVE() lv_screen_active()
#define UI_HANDLER()       lv_timer_handler()
#else
#define UI_SCREEN_ACTIVE() lv_scr_act()
#define UI_HANDLER()       lv_task_handler()
#endif

ZBUS_SUBSCRIBER_DEFINE(reflow_display_sub, 4);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_display_sub, 3);

static const struct device *const display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static lv_obj_t *lbl_profile;
static lv_obj_t *lbl_temp;
static lv_obj_t *lbl_setpoint;
static lv_obj_t *lbl_stage;
static lv_obj_t *lbl_state;
static lv_obj_t *bar_duty;

static void ui_build(void)
{
	lv_obj_t *scr = UI_SCREEN_ACTIVE();

	lbl_profile = lv_label_create(scr);
	lv_label_set_text(lbl_profile, "reflow oven");
	lv_obj_align(lbl_profile, LV_ALIGN_TOP_MID, 0, 6);

	lbl_temp = lv_label_create(scr);
	lv_label_set_text(lbl_temp, "--.- C");
#ifdef CONFIG_LV_FONT_MONTSERRAT_28
	lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_28, LV_PART_MAIN);
#endif
	lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, -30);

	lbl_setpoint = lv_label_create(scr);
	lv_label_set_text(lbl_setpoint, "sp --");
	lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, 6);

	lbl_stage = lv_label_create(scr);
	lv_label_set_text(lbl_stage, "stage -/-");
	lv_obj_align(lbl_stage, LV_ALIGN_CENTER, 0, 30);

	bar_duty = lv_bar_create(scr);
	lv_obj_set_size(bar_duty, 220, 16);
	lv_bar_set_range(bar_duty, 0, 1000);
	lv_bar_set_value(bar_duty, 0, LV_ANIM_OFF);
	lv_obj_align(bar_duty, LV_ALIGN_BOTTOM_MID, 0, -28);

	lbl_state = lv_label_create(scr);
	lv_label_set_text(lbl_state, "idle");
	lv_obj_align(lbl_state, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void ui_update(const struct reflow_telemetry *t)
{
	const struct reflow_profile *prof = reflow_profile_get(t->profile_idx);
	const char *stage_name = "-";

	if (prof != NULL) {
		lv_label_set_text(lbl_profile, prof->name);
		if (t->stage_idx < prof->n_stages) {
			stage_name = prof->stages[t->stage_idx].name;
		}
	}

	lv_label_set_text_fmt(lbl_temp, "%d.%01d C",
			      t->temp_mc / 1000, (abs(t->temp_mc) % 1000) / 100);

	if (t->setpoint_mc != 0) {
		lv_label_set_text_fmt(lbl_setpoint, "sp %d C", t->setpoint_mc / 1000);
	} else {
		lv_label_set_text(lbl_setpoint, "sp --");
	}

	lv_label_set_text_fmt(lbl_stage, "%s  %u/%u  %us", stage_name,
			      t->n_stages ? t->stage_idx + 1U : 0U, t->n_stages,
			      t->total_ms / 1000U);

	lv_bar_set_value(bar_duty, t->duty_permille, LV_ANIM_OFF);

	if (t->fault != REFLOW_FAULT_NONE) {
		lv_label_set_text_fmt(lbl_state, "FAULT: %s",
				      reflow_fault_str(t->fault));
	} else {
		lv_label_set_text_fmt(lbl_state, "%s  %u%%",
				      reflow_state_str(t->state),
				      t->duty_permille / 10U);
	}
}

static void display_thread(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	struct reflow_telemetry t;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready, UI disabled");
		return;
	}

	ui_build();
	UI_HANDLER();
	(void)display_blanking_off(display_dev);
	LOG_INF("display UI up on %s", display_dev->name);

	while (true) {
		if (zbus_sub_wait(&reflow_display_sub, &chan, K_MSEC(20)) == 0 &&
		    chan == &reflow_telemetry_chan &&
		    zbus_chan_read(chan, &t, K_MSEC(20)) == 0) {
			ui_update(&t);
		}
		UI_HANDLER();
	}
}

K_THREAD_DEFINE(reflow_display_tid, CONFIG_REFLOW_UI_DISPLAY_STACK_SIZE,
		display_thread, NULL, NULL, NULL, 7, 0, 500);

#include "general.h"
#include "target.h"

extern void zynq_amp_core_dump(target *t);

static target *crash_watchdog_target;

static void crash_watchdog_destroy(struct target_controller *tc, target *t)
{
	(void)tc;
	(void)t;
	crash_watchdog_target = NULL;
}

static struct target_controller crash_watchdog_controller = {
	.destroy_callback = crash_watchdog_destroy,
};

void crash_watchdog_poll(void)
{
	if (crash_watchdog_target == NULL) {
		crash_watchdog_target =
			target_attach_n(1, &crash_watchdog_controller);
		target_halt_resume(crash_watchdog_target, false);
		printf("Crash watchdog connected\n");
	}
	enum target_halt_reason reason = target_halt_poll(crash_watchdog_target, NULL);
	switch (reason) {
	case TARGET_HALT_RUNNING:
	case TARGET_HALT_ERROR:
		break;
	case TARGET_HALT_WATCHPOINT:
	case TARGET_HALT_REQUEST:
	case TARGET_HALT_STEPPING:
		/* These shouldn't happen, but dump core anyway */
	case TARGET_HALT_FAULT:
	case TARGET_HALT_BREAKPOINT:
		printf("Crash detected, dumping core\n");
		zynq_amp_core_dump(crash_watchdog_target);
		target_reset(crash_watchdog_target);
		target_halt_resume(crash_watchdog_target, false);
	}
}

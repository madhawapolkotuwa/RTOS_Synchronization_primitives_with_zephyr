/*
 * 03_mutexes: two sensor threads that share one bus
 *
 *   temp   priority 6: reads the temperature register, then sleeps 2 ms.
 *   accel  priority 4 (more urgent): every 10 ms reads X, Y and Z.
 *
 * The accelerometer must get X, Y and Z from the same moment, so it holds
 * the bus lock for the whole sample. Inside, it calls bus_read_reg(),
 * which locks the bus again. With a k_mutex this nested lock is fine
 * (recursive locking); with a semaphore it is a self-deadlock.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app.h"

#define STACK_SIZE      1024
#define TEMP_PRIORITY   6
#define ACCEL_PRIORITY  4
#define TEMP_PAUSE_MS   2
#define ACCEL_PERIOD_MS 10

struct sensor_stats {
	uint32_t ok;
	uint32_t bad;           /* -EIO: torn transaction, wrong or garbled data */
	uint32_t timeouts;      /* -EAGAIN: did not get the bus lock in time */
	uint32_t longest_us;    /* longest read, including waiting for the lock */
};

/*
 * Each thread writes only its own entry; main() reads them for the report.
 * Aligned 32-bit loads and stores are single instructions on the
 * Cortex-M7, so no value is ever seen half-written. (The values of one
 * entry may be from slightly different moments, which a report tolerates.)
 */
static struct sensor_stats temp_stats;
static struct sensor_stats accel_stats;

static void record(struct sensor_stats *st, int ret, uint32_t start_cycles)
{
	uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - start_cycles);

	if (us > st->longest_us) {
		st->longest_us = us;
	}

	if (ret == 0) {
		st->ok++;
	} else if (ret == -EAGAIN) {
		st->timeouts++;
	} else {
		st->bad++;
		error_led_toggle();
	}
}

/* ------------------------------------------------------------------ */

static void temp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		int16_t centi_degc;
		uint32_t start = k_cycle_get_32();
		int ret = bus_read_reg(DEV_TEMP, REG_TEMP, &centi_degc);

		record(&temp_stats, ret, start);

		/* Processing happens here, after the bus was released. */
		k_msleep(TEMP_PAUSE_MS);
	}
}

/* ------------------------------------------------------------------ */

static int accel_read_sample(int16_t xyz[3])
{
	static const uint8_t regs[3] = { REG_OUT_X, REG_OUT_Y, REG_OUT_Z };
	int result = 0;

	/* Outer lock: nobody may use the bus between the three reads. */
	int ret = bus_lock();

	if (ret != 0) {
		return ret;
	}

	for (int i = 0; i < 3; i++) {
		/* bus_read_reg() locks again: nested (recursive) locking. */
		ret = bus_read_reg(DEV_ACCEL, regs[i], &xyz[i]);
		if (ret != 0 && result == 0) {
			result = ret;   /* keep the first error, finish the sample */
		}
	}

#if CONFIG_APP_ACCEL_WORK_IN_LOCK_MS > 0
	/* WRONG SCOPE (experiment 4): processing while still holding the bus. */
	k_msleep(CONFIG_APP_ACCEL_WORK_IN_LOCK_MS);
#endif

	bus_unlock();
	return result;
}

static void accel_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		int16_t xyz[3];
		uint32_t start = k_cycle_get_32();
		int ret = accel_read_sample(xyz);

		record(&accel_stats, ret, start);

		k_msleep(ACCEL_PERIOD_MS);
	}
}

/* Created at boot but not started (K_FOREVER): main() starts them after the tour. */
K_THREAD_DEFINE(temp_tid, STACK_SIZE, temp_thread, NULL, NULL, NULL,
		TEMP_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(accel_tid, STACK_SIZE, accel_thread, NULL, NULL, NULL,
		ACCEL_PRIORITY, 0, SYS_FOREVER_MS);

void sensors_start(void)
{
	k_thread_name_set(temp_tid, "temp");
	k_thread_name_set(accel_tid, "accel");

	printk("Sensors: temp (prio %d) reads every %d ms, accel (prio %d) every %d ms. "
	       "Bus lock: %s\n",
	       TEMP_PRIORITY, TEMP_PAUSE_MS, ACCEL_PRIORITY, ACCEL_PERIOD_MS, bus_lock_name());

	k_thread_start(temp_tid);
	k_thread_start(accel_tid);
}

static void print_stats(const char *name, const char *unit, const struct sensor_stats *st)
{
	printk("[bus]   %-5s %s ok=%u bad=%u lock timeouts=%u longest=%u us\n",
	       name, unit, st->ok, st->bad, st->timeouts, st->longest_us);
}

void sensors_report(void)
{
	printk("[bus] t=%u ms, lock: %s\n", (uint32_t)k_uptime_get(), bus_lock_name());
	print_stats("temp", "reads  ", &temp_stats);
	print_stats("accel", "samples", &accel_stats);
}

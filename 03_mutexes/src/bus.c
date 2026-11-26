/*
 * 03_mutexes: a simulated I2C bus, and the lock that protects it
 *
 * Reading one register of an I2C device is a transaction of three steps:
 *
 *   1. START, device address + write
 *   2. register address
 *   3. repeated START, device address + read, data bytes, STOP
 *
 * Each step occupies the bus for CONFIG_APP_BUS_STEP_US (k_busy_wait, so
 * the thread really uses the CPU, as a polled driver would). If another
 * thread starts its own transaction between two steps, the transactions
 * interleave:
 *   - the thread that was interrupted reads the OTHER thread's device or
 *     register, i.e. wrong data;
 *   - the thread that started while the bus was busy collides with it.
 * The simulated devices tag every value with the address and register it
 * came from, so this model detects both cases exactly. A real bus would
 * just give you garbage.
 *
 * Zephyr's real I2C drivers lock the bus inside each i2c_transfer() call.
 * What they cannot protect is a sequence of separate calls that must not
 * be split, e.g. i2c_write() of a register address followed by
 * i2c_read(). That sequence is what this file models, and what the
 * application must lock itself.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app.h"

/* ------------------------------------------------------------------ */
/* The lock: chosen in prj.conf                                        */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_APP_LOCK_MUTEX)
/* One mutex per bus. Unlocked at start; the owner is recorded by the kernel. */
K_MUTEX_DEFINE(bus_mutex);
#elif defined(CONFIG_APP_LOCK_SEMAPHORE)
/* The tempting alternative: a binary semaphore, initially 1 ("free"). */
K_SEM_DEFINE(bus_sem, 1, 1);
#endif

const char *bus_lock_name(void)
{
	if (IS_ENABLED(CONFIG_APP_LOCK_MUTEX)) {
		return "k_mutex";
	}
	if (IS_ENABLED(CONFIG_APP_LOCK_SEMAPHORE)) {
		return "binary k_sem";
	}
	return "none";
}

int bus_lock(void)
{
#if defined(CONFIG_APP_LOCK_MUTEX)
	/* Recursive: if this thread already owns the mutex, this only counts up. */
	return k_mutex_lock(&bus_mutex, K_MSEC(CONFIG_APP_LOCK_TIMEOUT_MS));
#elif defined(CONFIG_APP_LOCK_SEMAPHORE)
	/* Not recursive: a second take by the same thread waits for itself. */
	return k_sem_take(&bus_sem, K_MSEC(CONFIG_APP_LOCK_TIMEOUT_MS));
#else
	return 0;
#endif
}

void bus_unlock(void)
{
#if defined(CONFIG_APP_LOCK_MUTEX)
	int ret = k_mutex_unlock(&bus_mutex);

	/* Only fails if this thread is not the owner (-EPERM) or the mutex is
	 * not locked (-EINVAL): both are bugs in the caller.
	 */
	if (ret != 0) {
		printk("[bus] ERROR: k_mutex_unlock() returned %d\n", ret);
	}
#elif defined(CONFIG_APP_LOCK_SEMAPHORE)
	k_sem_give(&bus_sem);   /* never fails, even if the caller never took it */
#endif
}

/* ------------------------------------------------------------------ */
/* The simulated hardware                                              */
/* ------------------------------------------------------------------ */

static struct {
	bool busy;          /* a transaction is in progress */
	uint8_t addr;       /* device addressed by the last START */
	uint8_t reg;        /* register selected by the last register write */
} bus;

/* What a device returns: its value, tagged with where it came from. */
struct reading {
	uint8_t addr;
	uint8_t reg;
	int16_t value;
};

static struct reading sim_device_read(uint8_t addr, uint8_t reg)
{
	static int16_t wobble;  /* makes the values change a little */
	struct reading r = { .addr = addr, .reg = reg };

	wobble = (wobble + 1) % 8;

	if (addr == DEV_TEMP && reg == REG_TEMP) {
		r.value = 2350 + wobble;            /* 23.50 degC, in 0.01 degC */
	} else if (addr == DEV_ACCEL && reg == REG_OUT_X) {
		r.value = 12 + wobble;              /* mg */
	} else if (addr == DEV_ACCEL && reg == REG_OUT_Y) {
		r.value = -8 + wobble;
	} else if (addr == DEV_ACCEL && reg == REG_OUT_Z) {
		r.value = 1000 + wobble;            /* board lying flat: 1 g */
	} else {
		r.value = -1;                       /* no such register */
	}
	return r;
}

static void bus_step(void)
{
	k_busy_wait(CONFIG_APP_BUS_STEP_US);
}

/* One register read. NOT safe to run in two threads at once. */
static int bus_transaction(uint8_t dev, uint8_t reg, int16_t *value)
{
	bool collided = false;

	/* Step 1: START + address. Another transaction must not be in flight. */
	if (bus.busy) {
		collided = true;
	}
	bus.busy = true;
	bus.addr = dev;
	bus_step();

	/* Step 2: register address. */
	bus.reg = reg;
	bus_step();

	/* Step 3: repeated START + read. The device answers for whatever the
	 * bus state says NOW, which may have been changed by another thread.
	 */
	bus_step();
	struct reading r = sim_device_read(bus.addr, bus.reg);

	bus.busy = false;

	*value = r.value;
	if (collided || r.addr != dev || r.reg != reg) {
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* The public, locked API                                              */
/* ------------------------------------------------------------------ */

/*
 * Read one register as one indivisible transaction: the lock is held for
 * exactly the three bus steps, and released before the caller processes
 * the value.
 */
int bus_read_reg(uint8_t dev, uint8_t reg, int16_t *value)
{
	int ret = bus_lock();

	if (ret != 0) {
		return ret;     /* -EAGAIN: did not get the bus in time */
	}

	ret = bus_transaction(dev, reg, value);

	bus_unlock();       /* one exit point: no path can skip the unlock */
	return ret;
}

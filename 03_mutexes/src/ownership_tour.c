/*
 * 03_mutexes: the ownership tour
 *
 * Runs once at startup, before the sensor threads start. It shows each
 * rule of mutex ownership as a return code, by calling the API from
 * main() and from a short-lived helper thread ("other"). Then it repeats
 * the key steps with a binary semaphore, which has no owner.
 *
 * It uses its own mutex and semaphore, not the bus lock.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app.h"

#if defined(CONFIG_APP_OWNERSHIP_TOUR)

K_MUTEX_DEFINE(tour_mutex);
K_SEM_DEFINE(tour_sem, 1, 1);

#define OTHER_STACK_SIZE 1024
#define OTHER_PRIORITY   1      /* more urgent than main: runs at once */

K_THREAD_STACK_DEFINE(other_stack, OTHER_STACK_SIZE);
static struct k_thread other_thread;

typedef int (*op_fn)(void);

static void other_entry(void *p1, void *p2, void *p3)
{
	op_fn op = (op_fn)p1;
	int *result = p2;

	ARG_UNUSED(p3);
	*result = op();
}

/* Run op() in another thread and return its result. */
static int in_other_thread(op_fn op)
{
	int result;

	k_thread_create(&other_thread, other_stack, K_THREAD_STACK_SIZEOF(other_stack),
			other_entry, (void *)op, &result, NULL,
			OTHER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&other_thread, "other");
	k_thread_join(&other_thread, K_FOREVER);
	return result;
}

static int mutex_unlock(void)       { return k_mutex_unlock(&tour_mutex); }
static int mutex_try_lock(void)     { return k_mutex_lock(&tour_mutex, K_NO_WAIT); }
static int mutex_lock_20ms(void)    { return k_mutex_lock(&tour_mutex, K_MSEC(20)); }
static int mutex_lock_unlock(void)
{
	int ret = k_mutex_lock(&tour_mutex, K_NO_WAIT);

	if (ret == 0) {
		k_mutex_unlock(&tour_mutex);    /* never exit a thread holding a mutex */
	}
	return ret;
}
static int sem_give(void)           { k_sem_give(&tour_sem); return 0; }

static const char *code_name(int ret)
{
	switch (ret) {
	case 0:        return "0";
	case -EBUSY:   return "-EBUSY";
	case -EAGAIN:  return "-EAGAIN";
	case -EPERM:   return "-EPERM";
	case -EINVAL:  return "-EINVAL";
	default:       return "other error";
	}
}

static void show(const char *who, const char *what, int ret, const char *meaning)
{
	printk("  %-6s %-34s -> %-8s %s\n", who, what, code_name(ret), meaning);
}

void ownership_tour(void)
{
	int ret;

	printk("\n--- Ownership tour: k_mutex ---\n");

	ret = k_mutex_unlock(&tour_mutex);
	show("main", "unlock (mutex is not locked)", ret, "nothing to unlock");

	ret = k_mutex_lock(&tour_mutex, K_FOREVER);
	show("main", "lock", ret, "main is now the owner");

	ret = in_other_thread(mutex_unlock);
	show("other", "unlock", ret, "rejected: other is not the owner");

	ret = in_other_thread(mutex_try_lock);
	show("other", "lock, K_NO_WAIT", ret, "held by main");

	ret = in_other_thread(mutex_lock_20ms);
	show("other", "lock, 20 ms timeout", ret, "gave up after 20 ms");

	ret = k_mutex_lock(&tour_mutex, K_FOREVER);
	show("main", "lock again (recursive)", ret, "lock count is now 2");

	ret = k_mutex_unlock(&tour_mutex);
	show("main", "unlock", ret, "lock count 1: still locked");

	ret = in_other_thread(mutex_try_lock);
	show("other", "lock, K_NO_WAIT", ret, "still held by main");

	ret = k_mutex_unlock(&tour_mutex);
	show("main", "unlock", ret, "lock count 0: free");

	ret = in_other_thread(mutex_lock_unlock);
	show("other", "lock, K_NO_WAIT (then unlock)", ret, "free, so other gets it");

	printk("--- The same with a binary k_sem used as a lock ---\n");

	ret = k_sem_take(&tour_sem, K_FOREVER);
	show("main", "take (\"lock\")", ret, "main holds it, but the kernel does not know");

	ret = in_other_thread(sem_give);
	show("other", "give (\"unlock\")", ret, "accepted: a semaphore has no owner");

	ret = k_sem_take(&tour_sem, K_NO_WAIT);
	show("main", "take again, K_NO_WAIT", ret, "succeeds only because other gave");

	ret = k_sem_take(&tour_sem, K_MSEC(20));
	show("main", "take again (nested), 20 ms", ret, "not recursive: main waits for itself");

	k_sem_give(&tour_sem);
	printk("--- End of tour ---\n\n");
}

#else

void ownership_tour(void)
{
}

#endif /* CONFIG_APP_OWNERSHIP_TOUR */

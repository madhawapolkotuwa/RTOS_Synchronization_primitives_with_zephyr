# 07 · Deadlock

**Status:** planned. The tutorial is not written yet. This folder holds its scope.

## Goal

Reproduce a two-mutex lock-order deadlock, diagnose it with the kernel shell, then fix it by lock ordering, by timeouts with back-off, and with a gatekeeper thread.

## Theory to read first

- [10_mutual_exclusion_and_deadlock.html](../resources/webpages/10_mutual_exclusion_and_deadlock.html)

## Zephyr APIs it will use

`k_mutex_lock` with timeout, `CONFIG_KERNEL_SHELL`, `CONFIG_THREAD_NAME`, `CONFIG_TASK_WDT`

## Target

Zephyr RTOS on the STM32F769I-DISCO (`west build -b stm32f769i_disco`).

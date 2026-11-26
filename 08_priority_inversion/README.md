# 08 · Priority inversion

**Status:** planned. The tutorial is not written yet. This folder holds its scope.

## Goal

Build the H/M/L scenario of Lesson 11, observe unbounded inversion with a semaphore used as a lock, and bounded blocking with `k_mutex` priority inheritance, using GPIO toggles and timing measurements.

## Theory to read first

- [11_priority_inversion.html](../resources/webpages/11_priority_inversion.html)

## Zephyr APIs it will use

`k_sem` vs `k_mutex`, `k_thread_priority_get`, `CONFIG_PRIORITY_CEILING`, `timing_counter_get`

## Target

Zephyr RTOS on the STM32F769I-DISCO (`west build -b stm32f769i_disco`).

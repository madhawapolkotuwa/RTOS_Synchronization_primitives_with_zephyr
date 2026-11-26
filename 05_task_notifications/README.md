# 05 · Task notifications

**Status:** planned. The tutorial is not written yet. This folder holds its scope.

## Goal

Implement one-to-one, direct-to-thread signalling patterns in Zephyr, which has no dedicated notification API, and compare their cost.

## Theory to read first

- [07_event_groups_and_notifications.html](../resources/webpages/07_event_groups_and_notifications.html)

## Zephyr APIs it will use

per-thread `k_sem` / `k_event`, `k_poll_signal_raise`, `k_poll`, `k_wakeup`

## Target

Zephyr RTOS on the STM32F769I-DISCO (`west build -b stm32f769i_disco`).

# 06 · Message queues

**Status:** planned. The tutorial is not written yet. This folder holds its scope.

## Goal

Producer/consumer pipeline from an ISR to a processing thread; measure the queue fill level and the drop count to validate the queue size.

## Theory to read first

- [08_message_queues.html](../resources/webpages/08_message_queues.html)

## Zephyr APIs it will use

`K_MSGQ_DEFINE`, `k_msgq_put`, `k_msgq_get`, `k_msgq_num_used_get`, `k_fifo` + `k_mem_slab`

## Target

Zephyr RTOS on the STM32F769I-DISCO (`west build -b stm32f769i_disco`).

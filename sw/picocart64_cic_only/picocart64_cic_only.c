/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Konrad Beckmann
 */

/*
 * CIC-only firmware.
 *
 * The Pico only answers the CIC handshake on N64_CIC_DCLK / N64_CIC_DIO.
 * The PI bus (N64_AD0..15, ALEL, ALEH, WRITE, READ) is never driven, so an
 * external device (e.g. an FPGA board wired to the same pins) can serve
 * ROM and SRAM instead.
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "stdio_async_uart.h"

#include "n64_cic.h"
#include "git_info.h"
#include "picocart64_pins.h"

#define UART_TX_PIN (28)
#define UART_RX_PIN (29)		/* not available on the pico */
#define UART_ID     uart0
#define BAUD_RATE   115200

// Priority 0 = lowest, 3 = highest
#define CIC_TASK_PRIORITY     (3UL)

static StaticTask_t cic_task;
static StackType_t cic_task_stack[4 * 1024 / sizeof(StackType_t)];

// FreeRTOS boilerplate
void vApplicationGetTimerTaskMemory(StaticTask_t ** ppxTimerTaskTCBBuffer, StackType_t ** ppxTimerTaskStackBuffer, uint32_t * pulTimerTaskStackSize)
{
	static StaticTask_t xTimerTaskTCB;
	static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

	*ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
	*ppxTimerTaskStackBuffer = uxTimerTaskStack;
	*pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void cic_task_entry(__unused void *params)
{
	printf("cic_task_entry\n");

	n64_cic_hw_init();

	// No reset callback: save data is owned by the external PI bus device.
	n64_cic_task(NULL);
}

void vLaunch(void)
{
	xTaskCreateStatic(cic_task_entry, "CICThread", configMINIMAL_STACK_SIZE, NULL, CIC_TASK_PRIORITY, cic_task_stack, &cic_task);

	/* Start the tasks and timer running. */
	vTaskStartScheduler();
}

int main(void)
{
	// Same clock as picocart64_v1, so the CIC timing matches the known-good firmware.
	set_sys_clock_khz(CONFIG_CPU_FREQ_MHZ * 1000, true);

	// Every GPIO is a floating input. The PI bus pins must stay high-Z
	// (no pulls) since the external device and the N64 share them.
	for (int i = 0; i <= 27; i++) {
		gpio_init(i);
		gpio_set_dir(i, GPIO_IN);
		gpio_set_pulls(i, false, false);
	}

	// Enable pull up on N64_CIC_DIO since there is no external one.
	gpio_pull_up(N64_CIC_DIO);

	// Init UART on pin 28/29
	stdio_async_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
	printf("PicoCart64 CIC-only Boot (git rev %08x)\r\n", GIT_REV);

	// Start FreeRTOS on Core0
	vLaunch();

	return 0;
}

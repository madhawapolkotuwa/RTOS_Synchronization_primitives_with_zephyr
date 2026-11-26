/* 03_mutexes: interfaces between the source files. */
#ifndef APP_H_
#define APP_H_

#include <stdint.h>

/* bus.c: a simulated I2C bus shared by two sensor threads */
#define DEV_TEMP   0x48   /* temperature sensor (like a TMP102) */
#define DEV_ACCEL  0x19   /* accelerometer (like a LIS2DH) */
#define REG_TEMP   0x00
#define REG_OUT_X  0x28
#define REG_OUT_Y  0x2A
#define REG_OUT_Z  0x2C

int  bus_lock(void);                 /* 0, or -EAGAIN after CONFIG_APP_LOCK_TIMEOUT_MS */
void bus_unlock(void);
int  bus_read_reg(uint8_t dev, uint8_t reg, int16_t *value);  /* 0, -EAGAIN or -EIO */
const char *bus_lock_name(void);

/* sensors.c */
void sensors_start(void);
void sensors_report(void);

/* ownership_tour.c */
void ownership_tour(void);

/* main.c */
void error_led_toggle(void);

#endif /* APP_H_ */

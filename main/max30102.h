#ifndef MAX30102_H_
#define MAX30102_H_

#include <stdint.h>
#include <stdbool.h>

/* MAX30102 I2C 7-bit slave address (0xAE >> 1) */
#define MAX30102_I2C_ADDR       0x57

/* I2C pin definitions (user specified) */
#define I2C_MASTER_SDA_IO       11
#define I2C_MASTER_SCL_IO       12
#define I2C_MASTER_FREQ_HZ      100000   /* 100 kHz */

/* Register addresses */
#define REG_INTR_STATUS_1       0x00
#define REG_INTR_STATUS_2       0x01
#define REG_INTR_ENABLE_1       0x02
#define REG_INTR_ENABLE_2       0x03
#define REG_FIFO_WR_PTR         0x04
#define REG_OVF_COUNTER         0x05
#define REG_FIFO_RD_PTR         0x06
#define REG_FIFO_DATA           0x07
#define REG_FIFO_CONFIG         0x08
#define REG_MODE_CONFIG         0x09
#define REG_SPO2_CONFIG         0x0A
#define REG_LED1_PA             0x0C
#define REG_LED2_PA             0x0D
#define REG_PILOT_PA            0x10
#define REG_MULTI_LED_CTRL1     0x11
#define REG_MULTI_LED_CTRL2     0x12
#define REG_TEMP_INTR           0x1F
#define REG_TEMP_FRAC           0x20
#define REG_TEMP_CONFIG         0x21
#define REG_PROX_INT_THRESH     0x30
#define REG_REV_ID              0xFE
#define REG_PART_ID             0xFF

/* Public API */
bool maxim_max30102_init(void);
bool maxim_max30102_read_fifo(uint32_t *pun_red_led, uint32_t *pun_ir_led);
bool maxim_max30102_write_reg(uint8_t uch_addr, uint8_t uch_data);
bool maxim_max30102_read_reg(uint8_t uch_addr, uint8_t *puch_data);
bool maxim_max30102_reset(void);

/**
 * Restart measurement: clear FIFO and re-write the measurement-mode
 * registers (MODE_CONFIG, SPO2_CONFIG, LED PA, FIFO_CONFIG).
 *
 * Use this after events that may have corrupted the sensor's registers
 * (e.g. I2S DMA bursts interfering with I2C). Does NOT re-run the full
 * init (no bus re-creation, no part-ID check).
 *
 * @return true on success.
 */
bool maxim_max30102_restart_measurement(void);

/**
 * Dump key registers for diagnostics (MODE_CONFIG, SPO2_CONFIG,
 * FIFO_CONFIG, FIFO_WR_PTR, FIFO_RD_PTR).
 */
void maxim_max30102_dump_regs(void);

#endif /* MAX30102_H_ */

/*
 * esp32_macro_runner/main/main.c
 *
 * Nintendo Switch Pro Controller emulation over Classic Bluetooth HID.
 * Receives timing config from EonTimer via USB Serial (UART0) and executes
 * the FireRed/LeafGreen shiny starter RNG macro with hardware-timer precision.
 *
 * Classic BT HID implementation based on UARTSwitchCon (GPL-3.0):
 *   https://github.com/nullstalgia/UARTSwitchCon
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_log_buffer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#define TAG_MAIN    "main"
#define TAG_BT      "bt"
#define TAG_UART    "uart"
#define TAG_MACRO   "macro"

// GPIO
#define LED_GPIO     2
#define TRIGGER_GPIO 0   // boot button on most ESP32 devboards (active LOW)
#define PIN_SEL      (1ULL << LED_GPIO)

// UART
#define UART_PORT    UART_NUM_0
#define UART_BAUD    115200
#define UART_RX_BUF  512

// Controller type: Pro Controller = 0x03
#define PRO_CON      0x03
#define JOYCON_L     0x01
#define JOYCON_R     0x02
#define CONTROLLER_TYPE PRO_CON

// Button bitmasks
#define BTN1_Y    0x01
#define BTN1_X    0x02
#define BTN1_B    0x04
#define BTN1_A    0x08
#define BTN2_MINUS 0x01
#define BTN2_PLUS  0x02
#define BTN2_RS    0x04
#define BTN2_LS    0x08
#define BTN2_HOME  0x10
#define BTN2_CAP   0x20

// GBA frame: 16777216 Hz, 280896 cycles/frame
// Frame duration in microseconds: advances * 280896000 / 16777216
#define GBA_FRAME_US(adv) ((int64_t)(adv) * 280896000LL / 16777216LL)

// Macro constants (must match FrLgPanel.tsx)
#define FRAME_ADVANCES    600
#define A_HOLD_MS         3000
#define NAV_DURATION_MS   6900

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// Button state (protected by xBtnSem)
static uint8_t but1_send = 0;
static uint8_t but2_send = 0;
static uint8_t but3_send = 0;
static uint8_t lx_send   = 128;
static uint8_t ly_send   = 128;
static uint8_t cx_send   = 128;
static uint8_t cy_send   = 128;
static uint8_t timer_byte = 0;

static SemaphoreHandle_t xBtnSem;

// BT state
static volatile bool bt_connected  = false;
static volatile bool bt_paired     = false;
static TaskHandle_t SendingHandle = NULL;
static TaskHandle_t BlinkHandle  = NULL;

// Macro state
static volatile bool g_macro_running = false;
static TaskHandle_t MacroHandle = NULL;

// Config (persisted in NVS)
typedef struct {
    int32_t seed_ms;
    float   seed_cal;   // ms
    int32_t cont_adv;
    float   cont_cal;   // ms
} macro_config_t;

static macro_config_t g_config = {
    .seed_ms  = 30441,
    .seed_cal = 0.0f,
    .cont_adv = 1987,
    .cont_cal = 0.0f,
};
static SemaphoreHandle_t xCfgSem;

// Serial output mutex (prevents log and status lines from interleaving)
static SemaphoreHandle_t xUartTxSem;

// ---------------------------------------------------------------------------
// HID descriptor (from UARTSwitchCon / Nintendo Switch Pro Controller)
// ---------------------------------------------------------------------------
static uint8_t hid_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff, 0x85, 0x21, 0x09,
    0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x30, 0x09, 0x30, 0x75,
    0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96,
    0x69, 0x01, 0x81, 0x02, 0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69,
    0x01, 0x81, 0x02, 0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01,
    0x81, 0x02, 0x85, 0x3f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39,
    0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0x06, 0x01, 0xff, 0x85, 0x01, 0x09,
    0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x10, 0x09, 0x10, 0x75,
    0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95,
    0x30, 0x91, 0x02, 0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91,
    0x02, 0xc0
};

// ---------------------------------------------------------------------------
// HID report buffer (48 bytes, report ID 0x30)
// ---------------------------------------------------------------------------
static uint8_t report30[48] = { [0] = 0x00, [1] = 0x8E, [11] = 0x80 };

// ---------------------------------------------------------------------------
// Subcommand reply arrays (from UARTSwitchCon)
// [0] = timer byte, [1] = 0x8E, [2-11] = sticks/buttons, [12] = ACK, [13] = subcommand
// ---------------------------------------------------------------------------
static uint8_t reply02[] = {
    0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x82, 0x02, 0x04, 0x00,
    CONTROLLER_TYPE,
    0x02, 0xD4, 0xF0, 0x57, 0x6E, 0xF0, 0xD7, 0x01,
    0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply08[] = {
    0x01, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply03[] = {
    0x04, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply04[] = {
    0x0A, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x83, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x2c, 0x01, 0x2c, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0[] = {
    0x02, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x90, 0x10, 0x00, 0x60, 0x00, 0x00, 0x10, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, CONTROLLER_TYPE, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};
static uint8_t spi_reply_address_0x50[] = {
    0x03, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x90, 0x10, 0x50, 0x60, 0x00, 0x00, 0x0D,
    0x23, 0x23, 0x23, 0xff, 0xff, 0xff,
    0x95, 0x15, 0x15, 0x15, 0x15, 0x95,  // Pro Controller colors
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0x80[] = {
    0x0B, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x90, 0x10, 0x80, 0x60, 0x00, 0x00, 0x18,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0x98[] = {
    0x0C, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x90, 0x10, 0x98, 0x60, 0x00, 0x00, 0x12,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0x10[] = {
    0x0D, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x90, 0x10, 0x10, 0x80, 0x00, 0x00, 0x18,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0x3d[] = {
    0x0E, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x90, 0x10, 0x3D, 0x60, 0x00, 0x00, 0x19,
    0x00, 0x07, 0x70, 0x00, 0x08, 0x80, 0x00, 0x07, 0x70, 0x00,
    0x08, 0x80, 0x00, 0x07, 0x70, 0x00, 0x07, 0x70, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
};
static uint8_t spi_reply_address_0x20[] = {
    0x10, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x90, 0x10, 0x20, 0x60, 0x00, 0x00, 0x18,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply4001[] = {
    0x15, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply4801[] = {
    0x1A, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply3001[] = {
    0x1C, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t reply3333[] = {
    0x31, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00,
    0x08, 0x80, 0x00, 0xa0, 0x21, 0x01, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b, 0x00
};
static uint8_t reply3401[] = {
    0x12, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x80, 0x00, 0x80, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
#define NVS_NS      "eon_runner"
#define NVS_KEY_CFG "macro_cfg"

static void nvs_load_config(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(macro_config_t);
    nvs_get_blob(h, NVS_KEY_CFG, &g_config, &sz);
    nvs_close(h);
    ESP_LOGI(TAG_MAIN, "Config loaded: seedMs=%d seedCal=%.3f contAdv=%d contCal=%.3f",
             g_config.seed_ms, g_config.seed_cal, g_config.cont_adv, g_config.cont_cal);
}

static void nvs_save_config(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_CFG, &g_config, sizeof(macro_config_t));
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// BT MAC address (Nintendo prefix D4:F0:57, random last 3 bytes in NVS)
// ---------------------------------------------------------------------------
static void set_bt_address(void) {
    nvs_handle_t h;
    uint8_t addr[6];
    size_t sz = sizeof(addr);

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    esp_err_t err = nvs_get_blob(h, "mac_addr", addr, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND || sz != sizeof(addr)) {
        addr[0] = 0xD4; addr[1] = 0xF0; addr[2] = 0x57;
        for (int i = 3; i < 6; i++) addr[i] = esp_random() & 0xFF;
        nvs_set_blob(h, "mac_addr", addr, sizeof(addr));
        nvs_commit(h);
    }
    nvs_close(h);
    esp_base_mac_addr_set(addr);
    ESP_LOGI(TAG_BT, "BT addr: %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// ---------------------------------------------------------------------------
// Button helpers
// ---------------------------------------------------------------------------
static void buttons_set(uint8_t b1, uint8_t b2, uint8_t b3) {
    xSemaphoreTake(xBtnSem, portMAX_DELAY);
    but1_send |= b1;
    but2_send |= b2;
    but3_send |= b3;
    xSemaphoreGive(xBtnSem);
}

static void buttons_clear(uint8_t b1, uint8_t b2, uint8_t b3) {
    xSemaphoreTake(xBtnSem, portMAX_DELAY);
    but1_send &= ~b1;
    but2_send &= ~b2;
    but3_send &= ~b3;
    xSemaphoreGive(xBtnSem);
}

static void buttons_release_all(void) {
    xSemaphoreTake(xBtnSem, portMAX_DELAY);
    but1_send = 0; but2_send = 0; but3_send = 0;
    xSemaphoreGive(xBtnSem);
}

// ---------------------------------------------------------------------------
// Serial status broadcast
// ---------------------------------------------------------------------------
static void serial_send_status(const char *state) {
    char buf[80];
    int len = snprintf(buf, sizeof(buf), "{\"type\":\"status\",\"state\":\"%s\"}\n", state);
    xSemaphoreTake(xUartTxSem, portMAX_DELAY);
    uart_write_bytes(UART_PORT, buf, len);
    xSemaphoreGive(xUartTxSem);
}

// ---------------------------------------------------------------------------
// HID send task — sends report30 every 15 ms (core 0, dedicated to BT)
// ---------------------------------------------------------------------------
static void send_buttons(void) {
    xSemaphoreTake(xBtnSem, portMAX_DELAY);
    report30[0] = timer_byte;
    timer_byte = (timer_byte + 3) & 0xFF;  // Pro Controller increments by 3 per 15ms
    report30[2] = but1_send;
    report30[3] = but2_send;
    report30[4] = but3_send;
    // Encode 8-bit stick values into 12-bit packed format (neutral = 128 → 0x800)
    report30[5] = (uint8_t)((lx_send << 4) & 0xF0);
    report30[6] = (uint8_t)((lx_send & 0xF0) >> 4);
    report30[7] = ly_send;
    report30[8] = (uint8_t)((cx_send << 4) & 0xF0);
    report30[9] = (uint8_t)((cx_send & 0xF0) >> 4);
    report30[10] = cy_send;
    xSemaphoreGive(xBtnSem);

    if (bt_connected) {
        esp_err_t err = esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x30,
                                                      sizeof(report30), report30);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_BT, "send_report failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void send_task(void *pvParameters) {
    ESP_LOGI(TAG_BT, "send_task running on core %d", xPortGetCoreID());
    while (1) {
        send_buttons();
    }
}

// ---------------------------------------------------------------------------
// LED blink task (advertising)
// ---------------------------------------------------------------------------
static void blink_task(void *pvParameters) {
    while (1) {
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------
// Drift-free wait (busy-wait for final ~5 ms to stay precise)
// ---------------------------------------------------------------------------
static void wait_until_us(int64_t target_us) {
    int64_t now = esp_timer_get_time();
    if (target_us <= now) return;
    int64_t wait_ms = (target_us - now) / 1000 - 5;
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
    }
    while (esp_timer_get_time() < target_us) {
        taskYIELD();
    }
}

// ---------------------------------------------------------------------------
// Macro task
// ---------------------------------------------------------------------------
static void macro_task(void *pvParameters) {
    ESP_LOGI(TAG_MACRO, "Macro starting");
    serial_send_status("running");

    // Snapshot config safely
    xSemaphoreTake(xCfgSem, portMAX_DELAY);
    macro_config_t cfg = g_config;
    xSemaphoreGive(xCfgSem);

    // Timing parameters (all in microseconds)
    int64_t seed_wait_us   = (int64_t)((cfg.seed_ms + cfg.seed_cal - 100.0f) * 1000.0f);
    int64_t cont_wait_us   = GBA_FRAME_US(cfg.cont_adv)
                             + (int64_t)(cfg.cont_cal * 1000.0f)
                             - (int64_t)(A_HOLD_MS * 1000LL);
    int64_t frame_wait_us  = GBA_FRAME_US(FRAME_ADVANCES)
                             - (int64_t)(NAV_DURATION_MS * 1000LL);

    if (seed_wait_us < 0)  seed_wait_us  = 0;
    if (cont_wait_us < 0)  cont_wait_us  = 0;
    if (frame_wait_us < 0) frame_wait_us = 0;

    int64_t cursor = esp_timer_get_time();

    // -----------------------------------------------------------------------
    // Setup phase (before EonTimer)
    // -----------------------------------------------------------------------
    // A 0.1s
    buttons_set(BTN1_A, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);
    // 1s
    cursor += 1000000LL; wait_until_us(cursor);
    // A 0.1s
    buttons_set(BTN1_A, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);
    // 1.5s
    cursor += 1500000LL; wait_until_us(cursor);
    // HOME 0.1s
    buttons_set(0, BTN2_HOME, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(0, BTN2_HOME, 0);
    // 20s (animation back to home screen)
    cursor += 20000000LL; wait_until_us(cursor);

    // -----------------------------------------------------------------------
    // Enter game — A 0.1s (this is the EonTimer start moment)
    // -----------------------------------------------------------------------
    serial_send_status("phase1_start");  // signal EonTimer to start now
    buttons_set(BTN1_A, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);

    // -----------------------------------------------------------------------
    // Phase 1: wait for seed, then open continue screen (hold A 3s)
    // -----------------------------------------------------------------------
    cursor += seed_wait_us; wait_until_us(cursor);
    buttons_set(BTN1_A, 0, 0);
    cursor += (int64_t)(A_HOLD_MS * 1000LL); wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);

    // -----------------------------------------------------------------------
    // Phase 2: wait for continue screen advance, then select save game
    // -----------------------------------------------------------------------
    cursor += cont_wait_us; wait_until_us(cursor);
    // A 0.1s
    buttons_set(BTN1_A, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);
    // 0.8s
    cursor += 800000LL; wait_until_us(cursor);
    // B 0.1s (skip "previously on" screen)
    buttons_set(BTN1_B, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_B, 0, 0);
    // 3s
    cursor += 3000000LL; wait_until_us(cursor);

    // -----------------------------------------------------------------------
    // Navigate to starter dialog
    // -----------------------------------------------------------------------
    // A 0.3s
    buttons_set(BTN1_A, 0, 0);
    cursor += 300000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);
    // 1s
    cursor += 1000000LL; wait_until_us(cursor);
    // A 0.3s
    buttons_set(BTN1_A, 0, 0);
    cursor += 300000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);
    // 1s
    cursor += 1000000LL; wait_until_us(cursor);
    // A 0.3s
    buttons_set(BTN1_A, 0, 0);
    cursor += 300000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);

    // -----------------------------------------------------------------------
    // Phase 3: wait for frame timer (600 advances), then frame-perfect press
    // -----------------------------------------------------------------------
    cursor += frame_wait_us; wait_until_us(cursor);
    buttons_set(BTN1_A, 0, 0);
    cursor += 100000LL; wait_until_us(cursor);
    buttons_clear(BTN1_A, 0, 0);

    // -----------------------------------------------------------------------
    // Done
    // -----------------------------------------------------------------------
    buttons_release_all();
    ESP_LOGI(TAG_MACRO, "Macro complete");
    g_macro_running = false;
    serial_send_status("idle");
    MacroHandle = NULL;
    vTaskDelete(NULL);
}

static void start_macro(void) {
    if (g_macro_running) {
        ESP_LOGW(TAG_MACRO, "Macro already running, ignoring trigger");
        return;
    }
    if (!bt_connected) {
        ESP_LOGW(TAG_MACRO, "BLE not connected, ignoring trigger");
        return;
    }
    g_macro_running = true;
    xTaskCreatePinnedToCore(macro_task, "macro_task", 4096, NULL, 5,
                            &MacroHandle, 1);
}

// ---------------------------------------------------------------------------
// UART task — reads newline-delimited JSON from serial (core 1)
// ---------------------------------------------------------------------------
static void uart_task(void *pvParameters) {
    ESP_LOGI(TAG_UART, "uart_task running on core %d", xPortGetCoreID());

    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &uart_cfg);
    // RX ring buffer only; TX left to ROM console (used by ESP_LOG + uart_write_bytes)
    uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0);

    char line[256];
    int  pos = 0;

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(20));
        if (n <= 0) continue;
        if (ch == '\r') continue;

        if (ch == '\n' || pos >= (int)sizeof(line) - 1) {
            line[pos] = '\0';
            pos = 0;
            if (line[0] != '{') continue;  // skip log lines echoed back

            cJSON *root = cJSON_Parse(line);
            if (!root) continue;

            cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
            if (cJSON_IsString(type_j)) {
                const char *type = type_j->valuestring;

                if (strcmp(type, "config") == 0) {
                    xSemaphoreTake(xCfgSem, portMAX_DELAY);
                    cJSON *j;
                    j = cJSON_GetObjectItemCaseSensitive(root, "seedMs");
                    if (cJSON_IsNumber(j)) g_config.seed_ms = (int32_t)j->valuedouble;
                    j = cJSON_GetObjectItemCaseSensitive(root, "seedCalibration");
                    if (cJSON_IsNumber(j)) g_config.seed_cal = (float)j->valuedouble;
                    j = cJSON_GetObjectItemCaseSensitive(root, "continueAdvances");
                    if (cJSON_IsNumber(j)) g_config.cont_adv = (int32_t)j->valuedouble;
                    j = cJSON_GetObjectItemCaseSensitive(root, "continueCalibration");
                    if (cJSON_IsNumber(j)) g_config.cont_cal = (float)j->valuedouble;
                    xSemaphoreGive(xCfgSem);
                    nvs_save_config();
                    ESP_LOGI(TAG_UART, "Config updated: seedMs=%d seedCal=%.3f contAdv=%d contCal=%.3f",
                             g_config.seed_ms, g_config.seed_cal,
                             g_config.cont_adv, g_config.cont_cal);

                } else if (strcmp(type, "trigger") == 0) {
                    start_macro();

                } else if (strcmp(type, "getStatus") == 0) {
                    serial_send_status(bt_connected ? "ble_connected" : "idle");
                }
            }
            cJSON_Delete(root);
        } else {
            line[pos++] = (char)ch;
        }
    }
}

// ---------------------------------------------------------------------------
// BT HID event callback
// ---------------------------------------------------------------------------
static void hidd_event_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch (event) {
    case ESP_HIDD_INIT_EVT:
        ESP_LOGI(TAG_BT, "HIDD init status=%d", param->init.status);
        break;

    case ESP_HIDD_REGISTER_APP_EVT:
        ESP_LOGI(TAG_BT, "HIDD app registered status=%d", param->register_app.status);
        break;

    case ESP_HIDD_OPEN_EVT: {
        esp_bd_addr_t *bd = &param->open.bd_addr;
        ESP_LOGI(TAG_BT, "Connected to %02x:%02x:%02x:%02x:%02x:%02x",
                 (*bd)[0], (*bd)[1], (*bd)[2], (*bd)[3], (*bd)[4], (*bd)[5]);
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (BlinkHandle) { vTaskDelete(BlinkHandle); BlinkHandle = NULL; }
        gpio_set_level(LED_GPIO, 1);
        bt_connected = true;
        serial_send_status("ble_connected");
        break;
    }

    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG_BT, "Disconnected status=%d", param->close.status);
        bt_connected = false;
        bt_paired    = false;
        serial_send_status("ble_disconnected");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        xTaskCreate(blink_task, "blink_task", 1024, NULL, 1, &BlinkHandle);
        break;

    case ESP_HIDD_GET_REPORT_EVT:
        ESP_LOGI(TAG_BT, "get_report type=%d id=%d", param->get_report.report_type, param->get_report.report_id);
        esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x30,
                                      sizeof(report30), report30);
        break;

    case ESP_HIDD_SET_REPORT_EVT:
        ESP_LOGI(TAG_BT, "set_report type=%d id=%d", param->set_report.report_type, param->set_report.report_id);
        break;

    case ESP_HIDD_SET_PROTOCOL_EVT:
        ESP_LOGI(TAG_BT, "set_protocol %d", param->set_protocol.protocol_mode);
        break;

    case ESP_HIDD_INTR_DATA_EVT: {
        uint8_t *p = param->intr_data.data;
        ESP_LOG_BUFFER_HEX(TAG_BT, p, param->intr_data.len);

        if (p[9] == 0x02)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply02), reply02);
        if (p[9] == 0x08)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply08), reply08);
        if (p[9] == 0x03)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply03), reply03);
        if (p[9] == 0x04)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply04), reply04);
        if (p[9] == 0x10 && p[10] ==   0 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0),    spi_reply_address_0);
        if (p[9] == 0x10 && p[10] ==  80 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x50), spi_reply_address_0x50);
        if (p[9] == 0x10 && p[10] == 128 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x80), spi_reply_address_0x80);
        if (p[9] == 0x10 && p[10] == 152 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x98), spi_reply_address_0x98);
        if (p[9] == 0x10 && p[10] ==  16 && p[11] == 128)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x10), spi_reply_address_0x10);
        if (p[9] == 0x10 && p[10] ==  61 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x3d), spi_reply_address_0x3d);
        if (p[9] == 0x10 && p[10] ==  32 && p[11] == 96)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x20), spi_reply_address_0x20);
        if (p[9] == 0x22)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3401), reply3401);
        if (p[9] == 0x40)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply4001), reply4001);
        if (p[9] == 0x48)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply4801), reply4801);
        if (p[9] == 0x30)
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3001), reply3001);
        if (p[9] == 0x21 && p[10] == 0x21) {
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3333), reply3333);
            bt_paired = true;
            ESP_LOGI(TAG_BT, "Paired!");
        }
        break;
    }

    case ESP_HIDD_VC_UNPLUG_EVT:
        ESP_LOGI(TAG_BT, "vc_unplug");
        break;

    default:
        break;
    }
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG_BT, "Auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG_BT, "Auth failed: %d", param->auth_cmpl.stat);
        }
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
void app_main(void) {
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Semaphores
    xBtnSem    = xSemaphoreCreateMutex();
    xCfgSem    = xSemaphoreCreateMutex();
    xUartTxSem = xSemaphoreCreateMutex();

    // GPIO: LED (output) + trigger button (input pull-up)
    gpio_config_t led_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = PIN_SEL,
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_GPIO, 0);

    gpio_config_t btn_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .pull_up_en   = 1,
    };
    gpio_config(&btn_cfg);

    // Load persisted config and BT address
    nvs_load_config();
    set_bt_address();

    // BT controller init (BLE disabled in sdkconfig)
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // GAP + HID
    esp_bt_gap_register_callback(esp_bt_gap_cb);

    static esp_hidd_app_param_t app_param;
    static esp_hidd_qos_param_t both_qos;

    app_param.name          = "Wireless Gamepad";
    app_param.description   = "Gamepad";
    app_param.provider      = "Nintendo";
    app_param.subclass      = 0x08;
    app_param.desc_list     = hid_descriptor;
    app_param.desc_list_len = sizeof(hid_descriptor);
    memset(&both_qos, 0, sizeof(both_qos));

    esp_bt_cod_t cod = { .minor = 2, .major = 5, .service = 1 };
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

    esp_bt_hid_device_register_callback(hidd_event_cb);
    esp_bt_hid_device_init();
    esp_bt_hid_device_register_app(&app_param, &both_qos, &both_qos);
    esp_bt_gap_set_device_name("Pro Controller");
    uint8_t pin[16] = { '0', '0', '0', '0' };
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Core 0: BT send task
    xTaskCreate(blink_task, "blink_task", 1024, NULL, 1, &BlinkHandle);
    xTaskCreatePinnedToCore(send_task, "send_task", 4096, NULL, 2, &SendingHandle, 0);

    // Core 1: UART communication task
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG_MAIN, "Ready. Connect EonTimer via USB serial at %d baud.", UART_BAUD);

    // Main loop: poll physical trigger button (active LOW)
    bool btn_prev = true;
    while (1) {
        bool btn_now = gpio_get_level(TRIGGER_GPIO);
        if (btn_prev && !btn_now) {
            ESP_LOGI(TAG_MAIN, "Physical trigger pressed");
            start_macro();
        }
        btn_prev = btn_now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

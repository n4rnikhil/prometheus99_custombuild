/**
 * @file main_esp32.c
 * @brief ESP32-S3 shell: RGB panel output, GT911 touch, Wi-Fi, LittleFS, SNTP
 *
 * The runtime itself (src/app.c and everything under src/) is the same code as on Windows and
 * Linux. This file only brings up the board and drives app.h:
 *   - 4-bpp bands are converted to RGB565 eight rows at a time (12.5 KB buffer) and copied into
 *     the panel's PSRAM frame buffer with esp_lcd_panel_draw_bitmap()
 *   - the fieldbus loop runs as a pthread on core 1 (see thread_posix.c), the UI loop here on core 0
 *   - alarm log, device key and credential vault live on the "storage" partition, mounted at /data
 *   - if the display cannot start (no PSRAM, wrong pins), the runtime keeps running without it:
 *     polling, alarms, the horn output and the audit log continue, and the log says why
 *
 * Built and checked with ESP-IDF 5.3.2.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_littlefs.h"
#if CONFIG_HMI_QEMU_ETH
#include "esp_eth.h"
#endif

#include "app.h"
#include "hal/platform.h"
#include "hal/timer.h"
#include "board.h"

static const char *TAG = "hmi";

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_touch_handle_t  s_touch;
static i2c_master_bus_handle_t s_i2c;
static volatile bool           s_repaint = true;

/* ------------------------------------------------------------------------- */
/* Hooks for the application core                                             */
/* ------------------------------------------------------------------------- */
void hal_console_print(const char *text) { fputs(text, stdout); }
void hal_beep(void)                      {}           /* the steady output below drives the buzzer */
bool hal_instance_lock(const char *path) { (void)path; return true; }   /* one firmware image */
void hal_sound_enable(bool on) { (void)on; }        /* no speaker: the buzzer pin is the horn */
void hal_sound_file(int kind, const char *path) { (void)kind; (void)path; }
static bool s_emergency_on, s_alarm_on;
static void horn_pin(bool on);
void hal_emergency_sound(bool on) { s_emergency_on = on; horn_pin(s_alarm_on || on); }
void hal_horn(bool on)           { s_alarm_on = on; horn_pin(on || s_emergency_on); }
static void horn_pin(bool on)
{
#if BOARD_PIN_HORN >= 0
    static int last = -1;
    if (last != (int)on) { gpio_set_level(BOARD_PIN_HORN, on ? 1 : 0); last = on; }
#else
    (void)on;
#endif
}
void hal_trim_memory(void)               {}
void hal_request_repaint(void)           { s_repaint = true; }
void hal_request_quit(void)              { app_stop(); }
void hal_fatal(const char *message)      { ESP_LOGE(TAG, "%s", message); }
bool hal_console_read_secret(const char *prompt, char *out, size_t cap)
{
    (void)prompt; (void)out; (void)cap;
    return false;                                     /* PINs are provisioned in the vault file */
}

/* ------------------------------------------------------------------------- */
/* Board                                                                      */
/* ------------------------------------------------------------------------- */
/* Extra steps for a board that needs more than board.h describes (called after the power-up below). */
__attribute__((weak)) void board_init(void) {}

static void horn_init(void)
{
#if BOARD_PIN_HORN >= 0
    gpio_reset_pin(BOARD_PIN_HORN);
    gpio_set_direction(BOARD_PIN_HORN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_PIN_HORN, 0);
#endif
}

/* The touch controller's I2C bus; the CH422G I/O expander (Waveshare boards) sits on it too */
static void i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_SDA,
        .scl_io_num = BOARD_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus on SDA %d / SCL %d did not start: no touch", BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
        s_i2c = NULL;
    }
}

#if BOARD_IO_EXPANDER_CH422G
/* CH422G: each "address" is a command; one data byte each (as in Waveshare's board examples) */
static bool ch422g_write(uint8_t cmd, uint8_t value)
{
    if (!s_i2c) return false;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = cmd, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_i2c, &dc, &dev) != ESP_OK) return false;
    esp_err_t e = i2c_master_transmit(dev, &value, 1, 100);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK;
}
#endif

/* Backlight on, panel and touch out of reset */
static void board_power_up(void)
{
#if BOARD_PIN_BACKLIGHT >= 0
    gpio_reset_pin(BOARD_PIN_BACKLIGHT);
    gpio_set_direction(BOARD_PIN_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_PIN_BACKLIGHT, 1);
#endif
#if BOARD_IO_EXPANDER_CH422G
#if BOARD_TOUCH_INT >= 0
    gpio_reset_pin(BOARD_TOUCH_INT);                  /* INT low during reset: GT911 address 0x5D */
    gpio_set_direction(BOARD_TOUCH_INT, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TOUCH_INT, 0);
#endif
    bool ok = ch422g_write(0x24, 0x01) &&             /* expander pins are outputs */
              ch422g_write(0x38, 0x2C);               /* backlight on, panel out of reset, touch held in reset */
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && ch422g_write(0x38, 0x2E);              /* touch out of reset */
    vTaskDelay(pdMS_TO_TICKS(200));
#if BOARD_TOUCH_INT >= 0
    gpio_set_direction(BOARD_TOUCH_INT, GPIO_MODE_INPUT);
#endif
    if (!ok) ESP_LOGW(TAG, "board: CH422G I/O expander does not answer - the backlight may stay off (check board.h)");
#endif
}

static bool display_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC_PULSE,
            .hsync_back_porch = BOARD_LCD_HSYNC_BACK,
            .hsync_front_porch = BOARD_LCD_HSYNC_FRONT,
            .vsync_pulse_width = BOARD_LCD_VSYNC_PULSE,
            .vsync_back_porch = BOARD_LCD_VSYNC_BACK,
            .vsync_front_porch = BOARD_LCD_VSYNC_FRONT,
            .flags = { .pclk_active_neg = BOARD_LCD_PCLK_NEG },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 10 * BOARD_LCD_H_RES,
        .hsync_gpio_num = BOARD_PIN_HSYNC,
        .vsync_gpio_num = BOARD_PIN_VSYNC,
        .de_gpio_num = BOARD_PIN_DE,
        .pclk_gpio_num = BOARD_PIN_PCLK,
        .disp_gpio_num = BOARD_PIN_DISP,
        .data_gpio_nums = BOARD_PIN_DATA,
        .flags = { .fb_in_psram = 1 },
    };
    esp_err_t e = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (e == ESP_OK) e = esp_lcd_panel_reset(s_panel);
    if (e == ESP_OK) e = esp_lcd_panel_init(s_panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "display did not start (%s): %s", esp_err_to_name(e),
                 esp_psram_get_size() == 0 ? "no PSRAM found - the 800x480 frame buffer needs an ESP32-S3 with PSRAM"
                                           : "check the panel pins and timing in board.h");
        if (s_panel) esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        return false;
    }
    return true;
}

static void touch_init(void)
{
    if (!s_i2c) return;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io) != ESP_OK) { ESP_LOGW(TAG, "touch: no panel IO"); return; }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
    };
    if (esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch: GT911 not found, running display-only");
        s_touch = NULL;
    }
}

static void storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) ESP_LOGE(TAG, "storage: /data not mounted, alarm log disabled");
}

/* ------------------------------------------------------------------------- */
/* Network                                                                    */
/* ------------------------------------------------------------------------- */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "network up, address " IPSTR, IP2STR(&ev->ip_info.ip));
}

#if CONFIG_HMI_QEMU_ETH
/* Espressif's QEMU emulates an OpenCores Ethernet MAC, not the Wi-Fi radio */
static void qemu_eth_init(void)
{
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}
#endif

#if !CONFIG_HMI_QEMU_ETH
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();                   /* keep trying: the HMI marks PLCs offline meanwhile */
    }
}
#endif

static void network_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_HMI_QEMU_ETH
    qemu_eth_init();
#else
    if (CONFIG_HMI_WIFI_SSID[0] == '\0') { ESP_LOGW(TAG, "no Wi-Fi configured (idf.py menuconfig)"); return; }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", CONFIG_HMI_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_HMI_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));       /* no power save: steady Modbus latency */
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    if (CONFIG_HMI_NTP_SERVER[0] != '\0') {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, CONFIG_HMI_NTP_SERVER);
        esp_sntp_init();
    }
}

/* ------------------------------------------------------------------------- */
/* Presentation and touch                                                     */
/* ------------------------------------------------------------------------- */
#define CHUNK_ROWS 8
static uint16_t s_rgb565[BOARD_LCD_H_RES * CHUNK_ROWS];

static void present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *pal)
{
    (void)ctx;
    uint16_t lut[16];
    for (int i = 0; i < 16; i++) {
        uint32_t c = pal[i];
        lut[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
    int w = width < BOARD_LCD_H_RES ? width : BOARD_LCD_H_RES;
    int stride = width / 2;
    for (int r0 = 0; r0 < rows; r0 += CHUNK_ROWS) {
        int n = rows - r0 < CHUNK_ROWS ? rows - r0 : CHUNK_ROWS;
        if (y + r0 >= BOARD_LCD_V_RES) return;
        if (y + r0 + n > BOARD_LCD_V_RES) n = BOARD_LCD_V_RES - (y + r0);
        for (int r = 0; r < n; r++) {
            const uint8_t *src = band + (size_t)(r0 + r) * (size_t)stride;
            uint16_t *dst = s_rgb565 + (size_t)r * (size_t)w;
            for (int x = 0; x < w; x++) dst[x] = lut[(x & 1) ? (src[x >> 1] & 15) : (src[x >> 1] >> 4)];
        }
        /* RGB panels copy into the PSRAM frame buffer before returning, so the buffer can be reused */
        esp_lcd_panel_draw_bitmap(s_panel, 0, y + r0, w, y + r0 + n, s_rgb565);
    }
}

static void touch_poll(void)
{
    static bool down;
    static int  last_x, last_y;
    if (!s_touch) return;
    esp_lcd_touch_point_data_t pt[1];
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_data(s_touch, pt, &count, 1) == ESP_OK && count > 0;
    if (pressed) {
        last_x = pt[0].x;
        last_y = pt[0].y;
        if (!down) app_pointer_down(last_x, last_y);
        else       app_pointer_move(last_x, last_y);
    } else if (down) {
        app_pointer_up(last_x, last_y);
    }
    down = pressed;
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */
static int split_args(char *line, char **argv, int max)
{
    int argc = 0;
    argv[argc++] = "industrial_hmi";
    for (char *p = line; *p && argc < max - 1;) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    argv[argc] = NULL;
    return argc;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    horn_init();
    i2c_init();
    board_power_up();
    board_init();
    storage_init();
    network_init();

    static char line[sizeof(CONFIG_HMI_ARGS)];
    static char *argv[24];
    memcpy(line, CONFIG_HMI_ARGS, sizeof(line));
    int argc = split_args(line, argv, 24);
    if (app_parse_args(argc, argv) >= 0 || app_init() != 0) {
        ESP_LOGE(TAG, "runtime did not start: check CONFIG_HMI_ARGS");
        return;
    }

    bool display = display_init();
    if (display) touch_init();
    else ESP_LOGW(TAG, "running without a display: polling, alarms, horn output and audit log continue");
    app_resize(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (!app_start_workers()) { app_shutdown(); return; }
    ESP_LOGI(TAG, "HMI runtime running, free heap %u bytes", (unsigned)esp_get_free_heap_size());

    while (app_running()) {
        uint32_t wait_ms = app_tick();
        touch_poll();
        if (s_repaint) {
            s_repaint = false;
            if (display) app_render(present_band, NULL);
        }
        hmi_timer_sleep_ms(wait_ms > 15u ? 15u : (wait_ms ? wait_ms : 1u));   /* touch is sampled every <= 15 ms */
    }
    app_shutdown();
}

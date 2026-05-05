/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE ranging server with local LED/buzzer alert and UART threshold command.
 */
#include <stdbool.h>
#include <string.h>
#include "sle_measure_dis_server.h"
#include "sle_errcode.h"
#include "sle_common.h"
#include "sle_device_manager.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_ssap_server.h"
#include "sle_hadm_manager.h"
#include "pinctrl.h"
#include "gpio.h"
#include "osal_interrupt.h"
#include "osal_msgqueue.h"
#include "sle_measure_dis_server_adv.h"
#include "sle_measure_dis_server_alg.h"
#include "tcxo.h"
#include "uart.h"

#ifndef CONFIG_MEASURE_DIS_LED_PIN
#define CONFIG_MEASURE_DIS_LED_PIN 31
#endif
#ifndef CONFIG_MEASURE_DIS_BUZZER_PIN
#define CONFIG_MEASURE_DIS_BUZZER_PIN 15
#endif
#ifndef CONFIG_MEASURE_DIS_BUZZER_ACTIVE_LEVEL
#define CONFIG_MEASURE_DIS_BUZZER_ACTIVE_LEVEL 0
#endif
#ifndef CONFIG_MEASURE_DIS_UART_BUS
#define CONFIG_MEASURE_DIS_UART_BUS 2
#endif
#ifndef CONFIG_MEASURE_DIS_UART_TX_PIN
#define CONFIG_MEASURE_DIS_UART_TX_PIN 17
#endif
#ifndef CONFIG_MEASURE_DIS_UART_RX_PIN
#define CONFIG_MEASURE_DIS_UART_RX_PIN 18
#endif
#ifndef CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM
#define CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM 250
#endif

#define DATA_LEN 2
#define ADV_TO_CLIENT 1
#define BLE_SLE_TAG_TASK_DURATION_MS 10
#define MEASURE_DIS_SLE_ENABLE_TIMEOUT_MS 5000
#define SLEM_IQ_DATALEN 512

#define MEASURE_DIS_LED_PIN ((pin_t)CONFIG_MEASURE_DIS_LED_PIN)
#define MEASURE_DIS_BUZZER_PIN ((pin_t)CONFIG_MEASURE_DIS_BUZZER_PIN)
#define MEASURE_DIS_LED_ACTIVE_LEVEL GPIO_LEVEL_HIGH
#define MEASURE_DIS_LED_INACTIVE_LEVEL GPIO_LEVEL_LOW
#define MEASURE_DIS_BUZZER_ACTIVE_LEVEL \
    ((CONFIG_MEASURE_DIS_BUZZER_ACTIVE_LEVEL == 0) ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH)
#define MEASURE_DIS_BUZZER_INACTIVE_LEVEL \
    ((CONFIG_MEASURE_DIS_BUZZER_ACTIVE_LEVEL == 0) ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW)

#define MEASURE_DIS_UART_BUS_ID CONFIG_MEASURE_DIS_UART_BUS
#define MEASURE_DIS_UART_BUS ((uart_bus_t)MEASURE_DIS_UART_BUS_ID)
#define MEASURE_DIS_UART_TX_PIN ((pin_t)CONFIG_MEASURE_DIS_UART_TX_PIN)
#define MEASURE_DIS_UART_RX_PIN ((pin_t)CONFIG_MEASURE_DIS_UART_RX_PIN)
#define MEASURE_DIS_UART_BAUD_RATE 115200
#define MEASURE_DIS_UART_RX_BUF_SIZE 64
#define MEASURE_DIS_UART_RX_RING_SIZE 128
#define MEASURE_DIS_UART_CMD_BUF_SIZE 32
#define MEASURE_DIS_UART_TASK_STACK_SIZE 0x800
#define MEASURE_DIS_UART_TASK_PRIO 26
#define MEASURE_DIS_UART_TASK_SLEEP_MS 20
#define MEASURE_DIS_UART_IDLE_PARSE_MS 150

#define MEASURE_DIS_INDICATOR_TASK_STACK_SIZE 0x800
#define MEASURE_DIS_INDICATOR_TASK_PRIO 26
#define MEASURE_DIS_THRESHOLD_CONFIRM_MS 300
#define MEASURE_DIS_LED_FAST_BLINK_MS 100
#define MEASURE_DIS_LED_SLOW_ON_MS 100
#define MEASURE_DIS_LED_SLOW_OFF_MS 900
#define MEASURE_DIS_LED_WAIT_ON_MS 250
#define MEASURE_DIS_LED_WAIT_OFF_MS 750
#define MEASURE_DIS_THRESHOLD_HYSTERESIS_CM 15
#define MEASURE_DIS_MAX_VALID_DISTANCE_CM 1000
#define MEASURE_DIS_MAX_JUMP_CM 300
#define MEASURE_DIS_VALID_TIMEOUT_MS 1200

static volatile uint8_t g_stack_enable_done = 0;
static volatile errcode_t g_stack_enable_result = ERRCODE_FAIL;

/* SLE server handle */
static uint8_t g_server_id = 0;
/* SLE service handle */
static uint16_t g_service_handle = 0;
/* SLE ntf property handle */
static uint16_t g_property_handle = 0;

#define SLEM_UUID_LEN SLE_UUID_LEN
#define SLEM_CONNET_INVAILD 0xFFFF
uint8_t g_measure_dis_server_addr[SLE_ADDR_LEN] = { 1, 1, 1, 1, 1, 1 };
uint8_t g_measure_dis_client_addr[SLE_ADDR_LEN] = { 2, 2, 2, 2, 2, 2 };
volatile uint16_t g_measure_dis_conn_id = SLEM_CONNET_INVAILD;

osal_event measure_dis_evt;
unsigned long g_measure_dis_queue;
measure_dis_msg_node_t g_msg_data;

static volatile bool g_measure_dis_connected = false;
static volatile bool g_measure_dis_far_state = false;
static volatile bool g_measure_dis_buzzer_on = false;
static volatile bool g_measure_dis_distance_valid = false;
static volatile bool g_threshold_confirm_pending = false;
static bool g_measure_dis_indicator_inited = false;
static volatile uint16_t g_measure_dis_filtered_distance_cm = 0;
static volatile uint32_t g_measure_dis_last_update_ms = 0;
static volatile uint16_t g_threshold_center_cm = CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM;
static volatile uint16_t g_buzzer_on_threshold_cm =
    CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM + MEASURE_DIS_THRESHOLD_HYSTERESIS_CM;
static volatile uint16_t g_buzzer_off_threshold_cm =
    (CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM > MEASURE_DIS_THRESHOLD_HYSTERESIS_CM) ?
    (CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM - MEASURE_DIS_THRESHOLD_HYSTERESIS_CM) : 0;

static uint8_t g_uart_driver_rx_buf[MEASURE_DIS_UART_RX_BUF_SIZE];
static uint8_t g_uart_rx_ring[MEASURE_DIS_UART_RX_RING_SIZE];
static volatile uint16_t g_uart_rx_head = 0;
static volatile uint16_t g_uart_rx_tail = 0;
static volatile bool g_uart_rx_overflow = false;
static bool g_uart_cmd_task_started = false;

measure_dis_server_data_t g_measure_dis_server_data = {
    .server_uuid = {0x11, 0x22, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    .service_uuid = {0x11, 0x33, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    .property_uuid = {0x11, 0x44, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
};

static uint16_t measure_dis_u16_abs_diff(uint16_t a, uint16_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static uint16_t measure_dis_distance_to_cm(float distance_m)
{
    if (distance_m <= 0.0f) {
        return 0;
    }
    return (uint16_t)((distance_m * MEASURE_DIS_NUM_CARRY_100) + 0.5f);
}

static void measure_dis_gpio_set_led(gpio_level_t level)
{
    (void)uapi_gpio_set_val(MEASURE_DIS_LED_PIN, level);
}

static void measure_dis_indicator_apply_buzzer(void)
{
    gpio_level_t level = MEASURE_DIS_BUZZER_INACTIVE_LEVEL;
    if (g_measure_dis_connected && g_measure_dis_distance_valid && g_measure_dis_buzzer_on) {
        level = MEASURE_DIS_BUZZER_ACTIVE_LEVEL;
    }
    (void)uapi_gpio_set_val(MEASURE_DIS_BUZZER_PIN, level);
}

static int measure_dis_indicator_task(void *arg)
{
    unused(arg);
    while (1) {
        if (g_threshold_confirm_pending) {
            g_threshold_confirm_pending = false;
            measure_dis_gpio_set_led(MEASURE_DIS_LED_ACTIVE_LEVEL);
            osal_msleep(MEASURE_DIS_THRESHOLD_CONFIRM_MS);
            measure_dis_gpio_set_led(MEASURE_DIS_LED_INACTIVE_LEVEL);
            continue;
        }

        if (g_measure_dis_connected && g_measure_dis_distance_valid) {
            uint32_t now_ms = (uint32_t)uapi_tcxo_get_ms();
            if (!measure_dis_alg_is_busy() && ((now_ms - g_measure_dis_last_update_ms) > MEASURE_DIS_VALID_TIMEOUT_MS)) {
                g_measure_dis_far_state = false;
                g_measure_dis_buzzer_on = false;
                g_measure_dis_distance_valid = false;
                osal_printk("measure_dis distance timeout, fallback to waiting state.\r\n");
                measure_dis_indicator_apply_buzzer();
            }
        }

        if (!g_measure_dis_connected) {
            measure_dis_gpio_set_led(MEASURE_DIS_LED_INACTIVE_LEVEL);
            measure_dis_indicator_apply_buzzer();
            osal_msleep(MEASURE_DIS_LED_SLOW_OFF_MS);
            continue;
        }

        if (!g_measure_dis_distance_valid) {
            measure_dis_gpio_set_led(MEASURE_DIS_LED_ACTIVE_LEVEL);
            osal_msleep(MEASURE_DIS_LED_WAIT_ON_MS);
            measure_dis_gpio_set_led(MEASURE_DIS_LED_INACTIVE_LEVEL);
            osal_msleep(MEASURE_DIS_LED_WAIT_OFF_MS);
            continue;
        }

        measure_dis_gpio_set_led(MEASURE_DIS_LED_ACTIVE_LEVEL);
        if (g_measure_dis_far_state) {
            osal_msleep(MEASURE_DIS_LED_FAST_BLINK_MS);
            measure_dis_gpio_set_led(MEASURE_DIS_LED_INACTIVE_LEVEL);
            osal_msleep(MEASURE_DIS_LED_FAST_BLINK_MS);
        } else {
            osal_msleep(MEASURE_DIS_LED_SLOW_ON_MS);
            measure_dis_gpio_set_led(MEASURE_DIS_LED_INACTIVE_LEVEL);
            osal_msleep(MEASURE_DIS_LED_SLOW_OFF_MS);
        }
    }
    return 0;
}

static errcode_t measure_dis_indicator_init(void)
{
    if (g_measure_dis_indicator_inited) {
        return ERRCODE_SUCC;
    }

    uapi_gpio_init();
    if (uapi_pin_set_mode(MEASURE_DIS_LED_PIN, HAL_PIO_FUNC_GPIO) != ERRCODE_SUCC ||
        uapi_gpio_set_dir(MEASURE_DIS_LED_PIN, GPIO_DIRECTION_OUTPUT) != ERRCODE_SUCC ||
        uapi_gpio_set_val(MEASURE_DIS_LED_PIN, MEASURE_DIS_LED_INACTIVE_LEVEL) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    if (uapi_pin_set_mode(MEASURE_DIS_BUZZER_PIN, HAL_PIO_FUNC_GPIO) != ERRCODE_SUCC ||
        uapi_gpio_set_dir(MEASURE_DIS_BUZZER_PIN, GPIO_DIRECTION_OUTPUT) != ERRCODE_SUCC ||
        uapi_gpio_set_val(MEASURE_DIS_BUZZER_PIN, MEASURE_DIS_BUZZER_INACTIVE_LEVEL) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    osal_task *task_handle = osal_kthread_create((osal_kthread_handler)measure_dis_indicator_task,
                                                 0, "MeasureDisInd", MEASURE_DIS_INDICATOR_TASK_STACK_SIZE);
    if (task_handle == NULL) {
        return ERRCODE_FAIL;
    }
    osal_kthread_set_priority(task_handle, MEASURE_DIS_INDICATOR_TASK_PRIO);
    osal_kfree(task_handle);

    g_measure_dis_indicator_inited = true;
    return ERRCODE_SUCC;
}

void measure_dis_indicator_set_conn_state(uint8_t connected)
{
    g_measure_dis_connected = (connected != 0);
    if (!connected) {
        g_measure_dis_far_state = false;
        g_measure_dis_buzzer_on = false;
        g_measure_dis_distance_valid = false;
        g_measure_dis_filtered_distance_cm = 0;
        g_measure_dis_last_update_ms = 0;
    }
    measure_dis_indicator_apply_buzzer();
}

void measure_dis_indicator_update_distance(float distance_m)
{
    if (!g_measure_dis_connected) {
        return;
    }

    if ((distance_m < 0.0f) || (distance_m > ((float)MEASURE_DIS_MAX_VALID_DISTANCE_CM / 100.0f))) {
        osal_printk("measure_dis invalid distance:%d.%03d\r\n",
                    (int32_t)distance_m,
                    ((uint32_t)(distance_m * MEASURE_DIS_NUM_CARRY_1000)) % MEASURE_DIS_NUM_CARRY_1000);
        return;
    }

    uint16_t distance_cm = measure_dis_distance_to_cm(distance_m);
    if (g_measure_dis_distance_valid &&
        (measure_dis_u16_abs_diff(distance_cm, g_measure_dis_filtered_distance_cm) > MEASURE_DIS_MAX_JUMP_CM)) {
        osal_printk("measure_dis jump detected, ignore. prev:%u.%02u curr:%u.%02u\r\n",
                    g_measure_dis_filtered_distance_cm / 100, g_measure_dis_filtered_distance_cm % 100,
                    distance_cm / 100, distance_cm % 100);
        return;
    }
    g_measure_dis_filtered_distance_cm = distance_cm;
    g_measure_dis_distance_valid = true;
    g_measure_dis_last_update_ms = (uint32_t)uapi_tcxo_get_ms();

    if (!g_measure_dis_far_state && (distance_cm >= g_buzzer_on_threshold_cm)) {
        g_measure_dis_far_state = true;
    } else if (g_measure_dis_far_state && (distance_cm <= g_buzzer_off_threshold_cm)) {
        g_measure_dis_far_state = false;
    }

    g_measure_dis_buzzer_on = g_measure_dis_far_state;
    osal_printk("measure_dis distance:%u.%02um threshold:%u.%02um far:%d buzzer:%d\r\n",
                distance_cm / 100, distance_cm % 100,
                g_threshold_center_cm / 100, g_threshold_center_cm % 100,
                g_measure_dis_far_state, g_measure_dis_buzzer_on);
    measure_dis_indicator_apply_buzzer();
}

static bool measure_dis_is_space(uint8_t ch)
{
    return (ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n');
}

static bool measure_dis_parse_threshold_cm(const uint8_t *data, uint16_t len, uint16_t *threshold_cm)
{
    uint32_t integer_part = 0;
    uint32_t frac_part = 0;
    uint32_t frac_scale = 1;
    bool seen_digit = false;
    bool seen_dot = false;
    bool unit_is_cm = false;
    bool unit_is_m = false;
    uint16_t i = 0;

    if ((data == NULL) || (threshold_cm == NULL)) {
        return false;
    }

    while ((i < len) && measure_dis_is_space(data[i])) {
        i++;
    }

    if ((i + 2 < len) &&
        ((data[i] == 't') || (data[i] == 'T')) &&
        ((data[i + 1] == 'h') || (data[i + 1] == 'H')) &&
        (data[i + 2] == '=')) {
        i += 3;
    }

    while ((i < len) && measure_dis_is_space(data[i])) {
        i++;
    }

    for (; i < len; i++) {
        uint8_t ch = data[i];
        if ((ch >= '0') && (ch <= '9')) {
            seen_digit = true;
            if (!seen_dot) {
                integer_part = integer_part * 10 + (uint32_t)(ch - '0');
                if (integer_part > MEASURE_DIS_MAX_VALID_DISTANCE_CM) {
                    return false;
                }
            } else if (frac_scale < 100) {
                frac_part = frac_part * 10 + (uint32_t)(ch - '0');
                frac_scale *= 10;
            }
            continue;
        }

        if ((ch == '.') && seen_digit && !seen_dot) {
            seen_dot = true;
            continue;
        }

        if (seen_digit &&
            (i + 1 < len) &&
            ((ch == 'c') || (ch == 'C')) &&
            ((data[i + 1] == 'm') || (data[i + 1] == 'M'))) {
            unit_is_cm = true;
            i += 2;
            break;
        }

        if (seen_digit &&
            ((ch == 'm') || (ch == 'M'))) {
            unit_is_m = true;
            i++;
            break;
        }

        if (seen_digit && measure_dis_is_space(ch)) {
            break;
        }

        return false;
    }

    if (!seen_digit) {
        return false;
    }

    while ((i < len) && measure_dis_is_space(data[i])) {
        i++;
    }
    if (i < len) {
        return false;
    }

    uint32_t cm;
    if (unit_is_cm || (!unit_is_m && !seen_dot && (integer_part > 10))) {
        if (seen_dot || (integer_part > MEASURE_DIS_MAX_VALID_DISTANCE_CM)) {
            return false;
        }
        cm = integer_part;
    } else {
        while (frac_scale < 100) {
            frac_part *= 10;
            frac_scale *= 10;
        }
        cm = integer_part * 100 + frac_part;
    }

    if (cm > MEASURE_DIS_MAX_VALID_DISTANCE_CM) {
        return false;
    }
    *threshold_cm = (uint16_t)cm;
    return true;
}

static void measure_dis_threshold_apply(uint16_t threshold_cm)
{
    g_threshold_center_cm = threshold_cm;
    g_buzzer_on_threshold_cm =
        ((uint32_t)threshold_cm + MEASURE_DIS_THRESHOLD_HYSTERESIS_CM > MEASURE_DIS_MAX_VALID_DISTANCE_CM) ?
        MEASURE_DIS_MAX_VALID_DISTANCE_CM : (threshold_cm + MEASURE_DIS_THRESHOLD_HYSTERESIS_CM);
    g_buzzer_off_threshold_cm =
        (threshold_cm > MEASURE_DIS_THRESHOLD_HYSTERESIS_CM) ?
        (threshold_cm - MEASURE_DIS_THRESHOLD_HYSTERESIS_CM) : 0;
    g_threshold_confirm_pending = true;
}

static void measure_dis_uart_write_text(const char *text)
{
    if (text == NULL) {
        return;
    }
    (void)uapi_uart_write(MEASURE_DIS_UART_BUS, (const uint8_t *)text, strlen(text), 0);
}

static void measure_dis_uart_send_threshold_result(bool ok, uint16_t threshold_cm)
{
    char response[96] = {0};
    int len;

    if (ok) {
        len = snprintf_s(response, sizeof(response), sizeof(response) - 1,
                         "OK TH=%u.%02um ON=%u.%02um OFF=%u.%02um\r\n",
                         threshold_cm / 100, threshold_cm % 100,
                         g_buzzer_on_threshold_cm / 100, g_buzzer_on_threshold_cm % 100,
                         g_buzzer_off_threshold_cm / 100, g_buzzer_off_threshold_cm % 100);
    } else {
        len = snprintf_s(response, sizeof(response), sizeof(response) - 1,
                         "ERR TH range 0.00-10.00m or 0-1000cm\r\n");
    }

    if (len > 0) {
        (void)uapi_uart_write(MEASURE_DIS_UART_BUS, (const uint8_t *)response, (uint32_t)len, 0);
    }
}

static void measure_dis_uart_process_command(const uint8_t *cmd, uint16_t len)
{
    uint16_t threshold_cm = 0;

    if (measure_dis_parse_threshold_cm(cmd, len, &threshold_cm)) {
        measure_dis_threshold_apply(threshold_cm);
        measure_dis_uart_send_threshold_result(true, threshold_cm);
        osal_printk("measure_dis threshold updated:%u.%02um\r\n", threshold_cm / 100, threshold_cm % 100);
        return;
    }

    measure_dis_uart_send_threshold_result(false, 0);
    osal_printk("measure_dis threshold command invalid.\r\n");
}

static void measure_dis_uart_rx_callback(const void *buffer, uint16_t length, bool error)
{
    if ((buffer == NULL) || (length == 0) || error) {
        return;
    }

    const uint8_t *rx_data = (const uint8_t *)buffer;
    for (uint16_t i = 0; i < length; i++) {
        uint16_t next_head = (uint16_t)(g_uart_rx_head + 1);
        if (next_head >= MEASURE_DIS_UART_RX_RING_SIZE) {
            next_head = 0;
        }
        if (next_head == g_uart_rx_tail) {
            g_uart_rx_overflow = true;
            break;
        }
        g_uart_rx_ring[g_uart_rx_head] = rx_data[i];
        g_uart_rx_head = next_head;
    }
}

static bool measure_dis_uart_take_byte(uint8_t *byte)
{
    bool has_data = false;
    uint32_t irq_sts = osal_irq_lock();

    if ((byte != NULL) && (g_uart_rx_tail != g_uart_rx_head)) {
        *byte = g_uart_rx_ring[g_uart_rx_tail];
        g_uart_rx_tail++;
        if (g_uart_rx_tail >= MEASURE_DIS_UART_RX_RING_SIZE) {
            g_uart_rx_tail = 0;
        }
        has_data = true;
    }

    osal_irq_restore(irq_sts);
    return has_data;
}

static bool measure_dis_uart_take_overflow(void)
{
    bool overflow;
    uint32_t irq_sts = osal_irq_lock();

    overflow = g_uart_rx_overflow;
    g_uart_rx_overflow = false;
    if (overflow) {
        g_uart_rx_tail = g_uart_rx_head;
    }
    osal_irq_restore(irq_sts);
    return overflow;
}

static int measure_dis_uart_cmd_task(void *arg)
{
    unused(arg);
    uint8_t cmd[MEASURE_DIS_UART_CMD_BUF_SIZE];
    uint16_t cmd_len = 0;
    uint32_t last_rx_ms = 0;

    measure_dis_uart_write_text("SLE MeasureDis UART2 threshold ready. Send 1.50m or 150cm.\r\n");

    while (1) {
        uint8_t ch;
        bool parse_now = false;
        uint32_t now_ms = (uint32_t)uapi_tcxo_get_ms();

        if (measure_dis_uart_take_overflow()) {
            cmd_len = 0;
            (void)memset_s(cmd, sizeof(cmd), 0, sizeof(cmd));
            measure_dis_uart_send_threshold_result(false, 0);
        }

        while (measure_dis_uart_take_byte(&ch)) {
            if (cmd_len < sizeof(cmd)) {
                cmd[cmd_len++] = ch;
            } else {
                cmd_len = 0;
                (void)memset_s(cmd, sizeof(cmd), 0, sizeof(cmd));
                measure_dis_uart_send_threshold_result(false, 0);
                break;
            }
            if ((ch == '\r') || (ch == '\n')) {
                parse_now = true;
            }
            last_rx_ms = now_ms;
        }

        if (!parse_now && (cmd_len > 0) && ((now_ms - last_rx_ms) >= MEASURE_DIS_UART_IDLE_PARSE_MS)) {
            parse_now = true;
        }

        if (parse_now && (cmd_len > 0)) {
            measure_dis_uart_process_command(cmd, cmd_len);
            (void)memset_s(cmd, sizeof(cmd), 0, sizeof(cmd));
            cmd_len = 0;
        }

        osal_msleep(MEASURE_DIS_UART_TASK_SLEEP_MS);
    }
    return 0;
}

static void measure_dis_uart_set_pinmux(void)
{
#if (MEASURE_DIS_UART_BUS_ID == 0)
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_TX_PIN, HAL_PIO_UART_L0_TXD);
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_RX_PIN, HAL_PIO_UART_L0_RXD);
#elif (MEASURE_DIS_UART_BUS_ID == 1)
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_TX_PIN, HAL_PIO_UART_H0_TXD);
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_RX_PIN, HAL_PIO_UART_H0_RXD);
#elif (MEASURE_DIS_UART_BUS_ID == 2)
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_TX_PIN, HAL_PIO_UART_L1_TXD);
    (void)uapi_pin_set_mode(MEASURE_DIS_UART_RX_PIN, HAL_PIO_UART_L1_RXD);
#endif
}

static errcode_t measure_dis_uart_init(void)
{
    uart_pin_config_t pin_cfg = {
        .tx_pin = MEASURE_DIS_UART_TX_PIN,
        .rx_pin = MEASURE_DIS_UART_RX_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };
    uart_attr_t attr = {
        .baud_rate = MEASURE_DIS_UART_BAUD_RATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE,
        .flow_ctrl = UART_FLOW_CTRL_NONE,
    };
    uart_buffer_config_t buf_cfg = {
        .rx_buffer = g_uart_driver_rx_buf,
        .rx_buffer_size = MEASURE_DIS_UART_RX_BUF_SIZE,
    };

    uart_pin_config_t *port_pin_cfg = uapi_uart_pin_cfg_get(MEASURE_DIS_UART_BUS);
    if (port_pin_cfg == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    if (memcpy_s(port_pin_cfg, sizeof(uart_pin_config_t), &pin_cfg, sizeof(pin_cfg)) != EOK) {
        return ERRCODE_MEMCPY;
    }

    measure_dis_uart_set_pinmux();
    (void)uapi_uart_deinit(MEASURE_DIS_UART_BUS);
    errcode_t ret = uapi_uart_init(MEASURE_DIS_UART_BUS, &pin_cfg, &attr, NULL, &buf_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("measure_dis uart init failed, ret:0x%x\r\n", ret);
        return ret;
    }

    uapi_uart_unregister_rx_callback(MEASURE_DIS_UART_BUS);
    ret = uapi_uart_register_rx_callback(MEASURE_DIS_UART_BUS,
                                          UART_RX_CONDITION_FULL_OR_IDLE,
                                          MEASURE_DIS_UART_RX_BUF_SIZE,
                                          measure_dis_uart_rx_callback);
    if (ret != ERRCODE_SUCC) {
        osal_printk("measure_dis uart rx callback failed, ret:0x%x\r\n", ret);
        return ret;
    }

    if (!g_uart_cmd_task_started) {
        osal_task *task_handle = osal_kthread_create((osal_kthread_handler)measure_dis_uart_cmd_task,
                                                     0, "MeasureDisUart", MEASURE_DIS_UART_TASK_STACK_SIZE);
        if (task_handle == NULL) {
            return ERRCODE_FAIL;
        }
        osal_kthread_set_priority(task_handle, MEASURE_DIS_UART_TASK_PRIO);
        osal_kfree(task_handle);
        g_uart_cmd_task_started = true;
    }

    osal_printk("measure_dis uart%d init ok. tx:%d rx:%d baud:%d\r\n",
                MEASURE_DIS_UART_BUS_ID, CONFIG_MEASURE_DIS_UART_TX_PIN,
                CONFIG_MEASURE_DIS_UART_RX_PIN, MEASURE_DIS_UART_BAUD_RATE);
    return ERRCODE_SUCC;
}

static void measure_dis_cm_conn_state_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                          sle_acb_state_t conn_state,
                                          sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(addr);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        osal_printk("Connected. conn_id:%d\r\n", conn_id);
        g_measure_dis_conn_id = conn_id;
        measure_dis_reset_iq_state();
        measure_dis_indicator_set_conn_state(true);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        osal_printk("Disconnected disc_reason:0x%x .\r\n", disc_reason);
        g_measure_dis_conn_id = SLEM_CONNET_INVAILD;
        measure_dis_reset_iq_state();
        measure_dis_indicator_set_conn_state(false);
        (void)measure_dis_start_adv(ADV_TO_CLIENT);
    }
}

errcode_t measure_dis_cm_register_cbks(void)
{
    sle_connection_callbacks_t cm_cbks = {0};
    cm_cbks.connect_state_changed_cb = measure_dis_cm_conn_state_cbk;
    return sle_connection_register_callbacks(&cm_cbks);
}

static void measure_dis_dd_enable_cbk(uint8_t status)
{
    osal_printk("sle enable result status:0x%x .\r\n", status);
    g_stack_enable_result = status;
    g_stack_enable_done = 1;
}

static void measure_dis_dd_disenable_cbk(uint8_t status)
{
    osal_printk("sle disable result status:0x%x .\r\n", status);
    unused(status);
    g_stack_enable_done = 0;
}

static void measure_dis_dd_announce_enable_cbk(uint32_t discovery_id, errcode_t status)
{
    osal_printk("discovery_enable_callback :discover_id:0x%x, status:0x%x \r\n",
                discovery_id, status);
}

static void measure_dis_dd_seek_result_cbk(sle_seek_result_info_t *seek_result_data)
{
    if (seek_result_data == NULL) {
        return;
    }

    if (memcmp(g_measure_dis_server_addr, seek_result_data->addr.addr, SLE_ADDR_LEN) == 0) {
        (void)sle_stop_seek();
        (void)sle_connect_remote_device(&(seek_result_data->addr));
    }
}

errcode_t measure_dis_dm_register_cbks(void)
{
    sle_dev_manager_callbacks_t dm_cbks = {
        .sle_enable_cb = measure_dis_dd_enable_cbk,
        .sle_disable_cb = measure_dis_dd_disenable_cbk,
    };
    return sle_dev_manager_register_callbacks(&dm_cbks);
}

errcode_t measure_dis_dd_register_cbks(void)
{
    sle_announce_seek_callbacks_t dd_cbks = {
        .announce_enable_cb = measure_dis_dd_announce_enable_cbk,
        .announce_disable_cb = NULL,
        .announce_terminal_cb = NULL,
        .seek_enable_cb = NULL,
        .seek_disable_cb = NULL,
        .seek_result_cb = measure_dis_dd_seek_result_cbk,
    };
    return sle_announce_seek_register_callbacks(&dd_cbks);
}

static void measure_dis_ssaps_read_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
}

void measure_dis_server_msg_proc(uint8_t *data, uint16_t data_len)
{
    uint32_t ret = ERRCODE_SLE_FAIL;
    if ((data == NULL) || (data_len < sizeof(measure_ids_msg_t))) {
        osal_printk("server msg invalid len:%u.\r\n", data_len);
        return;
    }

    measure_ids_msg_t *slem_profile_msg = (measure_ids_msg_t *)(data);
    if (slem_profile_msg->len != ((uint32_t)data_len - sizeof(measure_ids_msg_t))) {
        osal_printk("server msg len mismatch total:%u payload:%u.\r\n", data_len, slem_profile_msg->len);
        return;
    }

    switch (slem_profile_msg->type) {
        case SLEM_PROFILE_MSG_IQ:
            osal_printk("enter handle recv remote iq\r\n");
            ret = measure_dis_remote_iq((uint16_t)slem_profile_msg->len, slem_profile_msg->data);
            break;
        default:
            for (uint16_t i = 0; i < data_len; i++) {
                osal_printk("[%d]:%d\r\n", i, data[i]);
            }
            break;
    }

    if (unlikely(ret != ERRCODE_SLE_SUCCESS)) {
        osal_printk("client proc msg failed MSG_TYPE:%x ret:0x%x \r\n", slem_profile_msg->type, ret);
    }
}

static void measure_dis_ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    if ((write_cb_para == NULL) || (write_cb_para->value == NULL)) {
        osal_printk("server recv invalid write status:0x%x\r\n", status);
        return;
    }
    osal_printk("server recv msg length:0x%x status:0x%x \r\n", write_cb_para->length, status);
    measure_dis_server_msg_proc(write_cb_para->value, write_cb_para->length);
}

static void measure_dis_ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id, ssap_exchange_info_t *mtu_size,
    errcode_t status)
{
    if (mtu_size == NULL) {
        return;
    }
    osal_printk("[scd server] ssaps mtu change cbk server_id:%x, conn_id:%x, mtu_size:%x, status:%x\r\n",
                server_id, conn_id, mtu_size->mtu_size, status);
}

static void measure_dis_ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    osal_printk("[scd server] start service cbk server_id:%x, handle:%x, status:%x\r\n",
                server_id, handle, status);
}

static errcode_t measure_dis_ssaps_register_cbks(void)
{
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.start_service_cb = measure_dis_ssaps_start_service_cbk;
    ssaps_cbk.mtu_changed_cb = measure_dis_ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = measure_dis_ssaps_read_request_cbk;
    ssaps_cbk.write_request_cb = measure_dis_ssaps_write_request_cbk;
    return ssaps_register_callbacks(&ssaps_cbk);
}

static errcode_t measure_dis_server_service_add(void)
{
    errcode_t ret;
    sle_uuid_t service_uuid = {0};
    service_uuid.len = SLEM_UUID_LEN;
    if (memcpy_s(service_uuid.uuid, SLE_UUID_LEN, g_measure_dis_server_data.service_uuid, SLEM_UUID_LEN) != EOK) {
        return ERRCODE_MEMCPY;
    }
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    return (ret == ERRCODE_SLE_SUCCESS) ? ERRCODE_SLE_SUCCESS : ret;
}

static errcode_t measure_dis_server_property_add(void)
{
    uint8_t property_data[DATA_LEN] = {11, 11};
    uint8_t descriptor_data[DATA_LEN] = {0x01, 0x00};
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};

    property.uuid.len = SLEM_UUID_LEN;
    if (memcpy_s(property.uuid.uuid, SLE_UUID_LEN, g_measure_dis_server_data.property_uuid, SLEM_UUID_LEN) != EOK) {
        return ERRCODE_MEMCPY;
    }
    property.permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE;
    property.operate_indication =
        SSAP_OPERATE_INDICATION_BIT_READ |
        SSAP_OPERATE_INDICATION_BIT_WRITE |
        SSAP_OPERATE_INDICATION_BIT_NOTIFY;
    property.value = property_data;
    property.value_len = DATA_LEN;

    errcode_t ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    descriptor.permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE;
    descriptor.operate_indication =
        SSAP_OPERATE_INDICATION_BIT_READ |
        SSAP_OPERATE_INDICATION_BIT_WRITE |
        SSAP_OPERATE_INDICATION_BIT_DESCRIPTOR_CLIENT_CONFIGURATION_WRITE;
    descriptor.value = descriptor_data;
    descriptor.value_len = DATA_LEN;
    descriptor.type = SSAP_DESCRIPTOR_CLIENT_CONFIGURATION;

    return ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
}

int measure_dis_server_set_mtu(uint16_t mtu_size, uint16_t version)
{
    ssap_exchange_info_t info = {
        .mtu_size = mtu_size,
        .version = version
    };
    return ssaps_set_info(g_server_id, &info);
}

int measure_dis_server_add(void)
{
    errcode_t ret;
    sle_uuid_t app_uuid = {0};

    ret = measure_dis_ssaps_register_cbks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    osal_printk("[server] sle uuid add service in\r\n");
    app_uuid.len = SLEM_UUID_LEN;
    if (memcpy_s(app_uuid.uuid, SLE_UUID_LEN, g_measure_dis_server_data.server_uuid, SLEM_UUID_LEN) != EOK) {
        return ERRCODE_MEMCPY;
    }

    ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    ret = (errcode_t)measure_dis_server_set_mtu(SLEM_IQ_DATALEN, 1);
    if (ret != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ret;
    }

    if (measure_dis_server_service_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }

    if (measure_dis_server_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }

    osal_printk("[server] server_id:%x, service_handle:%x, property_handle:%x\r\n",
        g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ret;
    }
    osal_printk("[server] sle uuid add service out\r\n");
    return ERRCODE_SLE_SUCCESS;
}

errcode_t measure_dis_set_local_addr(uint8_t *addr)
{
    sle_addr_t sle_addr = {0};
    if (addr == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    if (memcpy_s(sle_addr.addr, SLE_ADDR_LEN, addr, SLE_ADDR_LEN) != EOK) {
        return ERRCODE_MEMCPY;
    }
    return sle_set_local_addr(&sle_addr);
}

errcode_t measure_dis_set_local_name(uint8_t *name, uint8_t len)
{
    if (name == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    return sle_set_local_name(name, len);
}

static errcode_t measure_dis_protocol_stack_init(uint8_t *addr, uint8_t *name)
{
    errcode_t ret = measure_dis_dm_register_cbks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    ret = measure_dis_cm_register_cbks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    ret = measure_dis_dd_register_cbks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    g_stack_enable_done = 0;
    g_stack_enable_result = ERRCODE_FAIL;
    ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    uint32_t waited_ms = 0;
    while (g_stack_enable_done != 1) {
        if (waited_ms >= MEASURE_DIS_SLE_ENABLE_TIMEOUT_MS) {
            osal_printk("measure_dis enable_sle timeout.\r\n");
            return ERRCODE_FAIL;
        }
        osal_msleep(BLE_SLE_TAG_TASK_DURATION_MS);
        waited_ms += BLE_SLE_TAG_TASK_DURATION_MS;
    }

    if (g_stack_enable_result != ERRCODE_SLE_SUCCESS) {
        return ERRCODE_SLE_FAIL;
    }

    ret = measure_dis_set_local_addr(addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    return measure_dis_set_local_name(name, strlen((char *)name));
}

int measure_dis_sle_server_ntf_by_addr(uint16_t len, uint8_t *data)
{
    if ((data == NULL) || (g_measure_dis_conn_id == SLEM_CONNET_INVAILD)) {
        return ERRCODE_INVALID_PARAM;
    }

    ssaps_ntf_ind_by_uuid_t param = {
        .uuid.len = SLEM_UUID_LEN,
        .start_handle = g_service_handle,
        .end_handle = g_property_handle,
        .type = SSAP_PROPERTY_TYPE_VALUE,
        .value_len = len,
        .value = data,
    };
    if (memcpy_s(param.uuid.uuid, SLE_UUID_LEN, g_measure_dis_server_data.property_uuid, SLEM_UUID_LEN) != EOK) {
        return ERRCODE_MEMCPY;
    }
    return ssaps_notify_indicate_by_uuid(g_server_id, g_measure_dis_conn_id, &param);
}

int measure_dis_server_write_client(uint32_t type, uint8_t *data, uint32_t data_len)
{
    if ((data == NULL) && (data_len != 0)) {
        return ERRCODE_INVALID_PARAM;
    }
    if (data_len > ((uint32_t)UINT16_MAX - sizeof(measure_ids_msg_t))) {
        return ERRCODE_INVALID_PARAM;
    }

    uint16_t len = sizeof(measure_ids_msg_t) + data_len;
    measure_ids_msg_t *slem_msg = (measure_ids_msg_t *)osal_kmalloc(len, 0);
    if (slem_msg == NULL) {
        return ERRCODE_MALLOC;
    }
    slem_msg->type = type;
    slem_msg->len = data_len;
    if ((data_len > 0) && (memcpy_s(slem_msg->data, data_len, data, data_len) != EOK)) {
        osal_kfree(slem_msg);
        return ERRCODE_MEMCPY;
    }

    uint32_t ret = measure_dis_sle_server_ntf_by_addr(len, (uint8_t *)(slem_msg));
    if (ret != ERRCODE_SUCC) {
        osal_printk("server send to client failed.\n");
    }
    osal_kfree(slem_msg);
    return ret;
}

errcode_t sle_measure_dis_msg_proc(uint32_t timeout)
{
    uint32_t msg_data_size = sizeof(measure_dis_msg_node_t);
    errcode_t ret = osal_msg_queue_read_copy(g_measure_dis_queue, &g_msg_data, &msg_data_size, timeout);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    errcode_t proc_ret = measure_dis_match_msg(&g_msg_data);
    if (proc_ret != ERRCODE_SUCC) {
        osal_printk("MSG PROC ERROR type:%d \r\n", g_msg_data.type);
    }
    return ERRCODE_SUCC;
}

errcode_t sle_measure_dis_msg_add(measure_dis_msg_node_t *msg)
{
    errcode_t ret = ERRCODE_FAIL;
    uint8_t msg_num = 0;

    do {
        if ((msg == NULL) || (g_measure_dis_queue == 0)) {
            osal_printk("MSG ADD FAIL. invalid input, msg:%p queue:%lu.\r\n", msg, g_measure_dis_queue);
            break;
        }
        if (osal_msg_queue_write_copy(g_measure_dis_queue,
                                      (void *)msg, sizeof(measure_dis_msg_node_t), 0) != OSAL_SUCCESS) {
            break;
        }
        if ((measure_dis_evt.event != NULL) &&
            (osal_event_write(&measure_dis_evt, MEASURE_DIS_MSG_EVENT) != OSAL_SUCCESS)) {
            msg_num = osal_msg_queue_get_msg_num(g_measure_dis_queue);
            osal_printk("MSG NOTIFY WARN. msg_num:%d, consumer will drain queue directly.\r\n", msg_num);
        }
        ret = ERRCODE_SUCC;
    } while (0);

    if (ret != ERRCODE_SUCC) {
        if (g_measure_dis_queue != 0) {
            msg_num = osal_msg_queue_get_msg_num(g_measure_dis_queue);
        }
        osal_printk("MSG ADD FAIL. msg_num:%d ret:%d. \r\n", msg_num, ret);
    }

    return ret;
}

static errcode_t sle_measure_dis_init(void)
{
    if (osal_event_init(&measure_dis_evt) != ERRCODE_SUCC) {
        osal_printk("measure_dis osal_event_init fail, continue with queue-only consumer. \r\n");
    }

    if (osal_msg_queue_create("measure_dis_queue", MEASURE_DIS_MSG_QUEUE_SIZE,
                              &g_measure_dis_queue, 0, sizeof(measure_dis_msg_node_t)) != ERRCODE_SUCC) {
        osal_printk("osal_queue_init init failed \r\n");
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}

int measure_dis_server_init(void)
{
    errcode_t ret;
    uint8_t measure_dis_name[SLE_NAME_MAX_LEN] = {'s', 'l', 'e', 'm', '-', 's', '\0'};

    ret = sle_measure_dis_init();
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    ret = measure_dis_indicator_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("measure_dis indicator init failed ret:0x%x\r\n", ret);
        return ret;
    }
    ret = measure_dis_uart_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("measure_dis uart init failed ret:0x%x\r\n", ret);
        return ret;
    }

    osal_printk("START SERVER INIT .\r\n");
    ret = measure_dis_protocol_stack_init(g_measure_dis_server_addr, measure_dis_name);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("measure_dis protocol init failed ret:0x%x\r\n", ret);
        return ret;
    }
    ret = measure_dis_reg_callbacks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    ret = measure_dis_server_add();
    check_rc_return_rc(ret, "server add");
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    ret = measure_dis_sle_set_adv(ADV_TO_CLIENT, g_measure_dis_server_addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    return measure_dis_start_adv(ADV_TO_CLIENT);
}

void carkey_sle_rssi_report_cbk(uint16_t conn_id, int8_t rssi)
{
    osal_printk("SLE RSSI REPORT conn_id:%d, rssi:%d .\r\n", conn_id, rssi);
}

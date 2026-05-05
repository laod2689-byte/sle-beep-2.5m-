/**
 * Legacy example placeholder.
 *
 * The real product implementation is in:
 * - sle_measure_dis_server/sle_measure_dis_server.c
 * - sle_measure_dis_server/sle_measure_dis_server_alg.c
 *
 * UART threshold updates now use UART2 and server-side LED/buzzer indication.
 * This file is not included by CMake; the empty functions are kept only so
 * older external references do not break if the file is compiled manually.
 */
#include "sle_measure_dis_server.h"

void test_distance_alert_example(void)
{
}

void send_distance_info_uart(float distance, uint8_t device_id)
{
    unused(distance);
    unused(device_id);
}

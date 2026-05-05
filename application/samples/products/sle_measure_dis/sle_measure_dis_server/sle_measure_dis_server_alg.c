/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE ranging server algorithm processing.
 */
#include <stdbool.h>
#include "sle_measure_dis_server.h"
#include "sle_common.h"
#include "slem_smooth.h"
#include "slem_alg_smooth_dis.h"
#include "tcxo.h"
#include "osal_msgqueue.h"
#include "osal_interrupt.h"
#include "sle_measure_dis_server_alg.h"

#define TOF_DEFAULT 2070
#define DIS_ALG_MODE 4
#define DIS_ALG_RSSI_LIMIT (-120)
#define DIS_ALG_THRESHOULD_COND2 20
#define DIS_ALG_R_START 2
#define MEASURE_DIS_ALG_RESULT_MAX_M 10.0f

uint8_t g_iq_debug_flag = 0;
uint32_t counter = 0;

measure_dis_stored_iq_data_t g_md_local_iq_data;
uint8_t g_md_store_local_iq_complete;
uint8_t g_next_idx;
static volatile bool g_measure_dis_alg_busy = false;

bool measure_dis_alg_is_busy(void)
{
    return (g_measure_dis_alg_busy != false);
}

static uint32_t measure_dis_timestamp_diff(uint32_t ts_a, uint32_t ts_b)
{
    return (ts_a >= ts_b) ? (ts_a - ts_b) : (ts_b - ts_a);
}

static uint32_t measure_dis_float_frac_3(float value)
{
    float abs_value = (value >= 0.0f) ? value : -value;
    return ((uint32_t)(abs_value * MEASURE_DIS_NUM_CARRY_1000)) % MEASURE_DIS_NUM_CARRY_1000;
}

static bool measure_dis_iq_sample_count_valid(uint8_t samp_cnt)
{
    return (samp_cnt >= POSALG_DATA_NUM) && (samp_cnt <= SLE_CS_IQ_REPORT_COUNT);
}

static bool measure_dis_iq_prefix_all_zero(const sle_channel_sounding_qte_trans_t *data, uint8_t samp_cnt)
{
    uint8_t zero_count = 0;
    uint8_t check_count = (samp_cnt < 5) ? samp_cnt : 5;

    for (uint8_t i = 0; i < check_count; i++) {
        if ((data[i].i_data == 0) && (data[i].q_data == 0)) {
            zero_count++;
        }
    }
    return (zero_count == check_count);
}

void measure_dis_reset_iq_state(void)
{
    (void)memset_s(&g_md_local_iq_data, sizeof(g_md_local_iq_data), 0, sizeof(g_md_local_iq_data));
    g_md_store_local_iq_complete = false;
    g_next_idx = 0;
}

static void measure_dis_set_invalid_distance(measure_dis_profile_msg_dis_t *measure_dis_temp)
{
    if (measure_dis_temp == NULL) {
        return;
    }

    measure_dis_temp->dist_first = MEASURE_DIS_INVALID;
    measure_dis_temp->dist_second = MEASURE_DIS_INVALID;
    measure_dis_temp->dist_double = MEASURE_DIS_INVALID;
    measure_dis_temp->rssi = MEASURE_DIS_RSSI_INVALID;
    measure_dis_temp->high = MEASURE_DIS_HIGH_INVALID;
    measure_dis_temp->prob = 0;
    measure_dis_temp->smooth_num = 0;
}

void slem_posalg_set_base_para(slem_alg_para_dis *alg_para, uint8_t key_id)
{
    unused(key_id);
    alg_para->calib_val = POSALG_CALIB_VAL;
    alg_para->tof_calib = TOF_DEFAULT;
    alg_para->ranging_method = DIS_ALG_MODE;
}

void measure_dis_print_cal_dis(float dist_first, float dist_ori, float prob, uint32_t time)
{
    unused(dist_ori);
    unused(prob);
    osal_printk("SLEM get distance done. distance:%d.%03d, time = %d ms. \r\n",
                (int32_t)dist_first, measure_dis_float_frac_3(dist_first), (uint32_t)time);
}

static void measure_dis_posalg_set_distance(slem_smoothed_dis_result *dis,
    measure_dis_profile_msg_dis_t *measure_dis_temp)
{
    measure_dis_temp->dist_first = dis->dis_smoothed;
    measure_dis_temp->dist_second = dis->dis_ori;
    measure_dis_temp->dist_double = dis->dis_slight_smoothed;
    measure_dis_temp->high = dis->height;
    measure_dis_temp->prob = dis->prob;
    measure_dis_temp->rssi = dis->rssi;
    measure_dis_temp->smooth_num = dis->smooth_num;
}

void measure_dis_posalg_set_base_para(slem_alg_para_dis *alg_para)
{
    alg_para->calib_val = POSALG_CALIB_VAL;
    alg_para->tof_calib = TOF_DEFAULT;
    alg_para->ranging_method = DIS_ALG_MODE;
}

static errcode_t measure_dis_posalg_get_distance(measure_dis_profile_msg_dis_t *measure_dis_temp,
                                                 const measure_dis_stored_iq_data_t *local_iq_data,
                                                 sle_channel_sounding_iq_trans_t *remote_iq_data)
{
    slem_alg_para_dis alg_para = {0};
    slem_smoothed_dis_result result_dist = {0};

    if ((measure_dis_temp == NULL) || (local_iq_data == NULL) || (remote_iq_data == NULL)) {
        osal_printk("measure_dis_posalg_get_distance invalid input. temp:%p local:%p remote:%p.\r\n",
                    measure_dis_temp, local_iq_data, remote_iq_data);
        return ERRCODE_INVALID_PARAM;
    }

    measure_dis_set_invalid_distance(measure_dis_temp);
    if (!measure_dis_iq_sample_count_valid(local_iq_data->samp_cnt) ||
        !measure_dis_iq_sample_count_valid(remote_iq_data[0].samp_cnt)) {
        osal_printk("measure_dis_posalg_get_distance invalid samp_cnt. local:%u remote:%u.\r\n",
                    local_iq_data->samp_cnt, remote_iq_data[0].samp_cnt);
        return ERRCODE_INVALID_PARAM;
    }

    if ((local_iq_data->rssi == 0) || (local_iq_data->rssi == 0xFF) ||
        (remote_iq_data[0].rssi == 0) || (remote_iq_data[0].rssi == 0xFF)) {
        osal_printk("measure_dis_posalg_get_distance invalid rssi. local:%u remote:%u.\r\n",
                    local_iq_data->rssi, remote_iq_data[0].rssi);
        return ERRCODE_INVALID_PARAM;
    }

    if (measure_dis_iq_prefix_all_zero((sle_channel_sounding_qte_trans_t *)local_iq_data->data,
                                       local_iq_data->samp_cnt) ||
        measure_dis_iq_prefix_all_zero(remote_iq_data[0].data, remote_iq_data[0].samp_cnt)) {
        osal_printk("measure_dis_posalg_get_distance invalid all-zero IQ data.\r\n");
        return ERRCODE_INVALID_PARAM;
    }

    slem_posalg_set_base_para(&alg_para, 0);
    alg_para.rssi_rtd = local_iq_data->rssi;
    alg_para.rssi_dut = remote_iq_data[0].rssi;
#if (defined(GLE_CS_MODE3_SUPPORT))
    alg_para.tof_rtd = local_iq_data->tof_result;
    alg_para.tof_dut = remote_iq_data[0].tof_result;
#endif
    alg_para.iq_rtd = (slem_alg_iq *)&(local_iq_data->data[0]);
    alg_para.iq_dut = (slem_alg_iq *)&(remote_iq_data[0].data[0]);
    alg_para.para_limit.rssi_limit = DIS_ALG_RSSI_LIMIT;
    alg_para.para_limit.threshold_cond2 = DIS_ALG_THRESHOULD_COND2;
    alg_para.para_limit.r_start = DIS_ALG_R_START;
    alg_para.key_id = 0;
    alg_para.cur_count = counter;
    alg_para.cur_time = (uint32_t)uapi_tcxo_get_ms();

    uint32_t time_start = alg_para.cur_time;
    errcode_slem alg_ret = slem_alg_calc_smoothed_dis(&result_dist, &alg_para);
    if (alg_ret != ERRCODE_SLEM_SUCCESS) {
        osal_printk("slem_alg_calc_smoothed_dis failed. ret:0x%x local_ts:%u remote_ts:%u.\r\n",
                    alg_ret, local_iq_data->timestamp_sn, remote_iq_data[0].timestamp_sn);
        return (errcode_t)alg_ret;
    }
    counter++;

    if ((result_dist.dis_smoothed != result_dist.dis_smoothed) ||
        (result_dist.dis_smoothed < 0.0f) ||
        (result_dist.dis_smoothed > MEASURE_DIS_ALG_RESULT_MAX_M)) {
        osal_printk("measure_dis_posalg_get_distance invalid result. dist:%d.%03d.\r\n",
                    (int32_t)result_dist.dis_smoothed, measure_dis_float_frac_3(result_dist.dis_smoothed));
        return ERRCODE_INVALID_PARAM;
    }

    measure_dis_print_cal_dis(result_dist.dis_smoothed, result_dist.dis_ori,
                              result_dist.prob, (uint32_t)uapi_tcxo_get_ms() - time_start);
    measure_dis_posalg_set_distance(&result_dist, measure_dis_temp);
    return ERRCODE_SUCC;
}

void measure_dis_print_iq_data(uint8_t num, uint8_t role, sle_channel_sounding_iq_trans_t *report)
{
    if (!g_iq_debug_flag || (report == NULL)) {
        return;
    }

    osal_printk("[iq_data]%d,%d,%d,%d,0x%x,",
                num, role, report->timestamp_sn, report->samp_cnt, report->rssi);
    sle_channel_sounding_qte_trans_t *data = report->data;
    uint16_t i = 0;
    while (i < POSALG_DATA_NUM) {
        osal_printk("[%d]:0x%x,0x%x", i, data[i].i_data, data[i].q_data);
        i++;
        if (i == POSALG_DATA_NUM) {
            osal_printk(".\r\n");
        } else {
            osal_printk(";");
        }
    }
}

errcode_t measure_dis_store_local_iq(sle_channel_sounding_iq_report_t *report)
{
    if ((report == NULL) || !measure_dis_iq_sample_count_valid(report->samp_cnt)) {
        osal_printk("MEASURE_DIS STORE LOCAL IQ FAIL. invalid report or samp_cnt:%u.\r\n",
                    (report == NULL) ? 0 : report->samp_cnt);
        return ERRCODE_INVALID_PARAM;
    }

    if (report->report_idx == 0) {
        g_md_local_iq_data.samp_cnt = 0;
        g_md_local_iq_data.rssi = report->rssi[0];
        g_md_local_iq_data.es_sn = report->es_sn;
        g_md_local_iq_data.timestamp_sn = report->timestamp_sn;
#if (defined(GLE_CS_MODE3_SUPPORT))
        g_md_local_iq_data.tof_result = report->tof_result;
#endif
        g_next_idx = 0;
    }

    if ((report->report_idx >= MEASURE_DIS_IQ_REPORT_CNT_MAX) ||
        (report->timestamp_sn != g_md_local_iq_data.timestamp_sn) ||
        (g_next_idx != report->report_idx)) {
        osal_printk("MEASURE_DIS STORE LOCAL IQ FAIL. report_idx:%d, timestamp:%d, g_next_idx:%d. \r\n",
                    report->report_idx, report->timestamp_sn, g_next_idx);
        return ERRCODE_INVALID_PARAM;
    }

    uint16_t offset = report->report_idx * SLE_CS_IQ_REPORT_COUNT;
    if ((offset + report->samp_cnt) > IQ_DATA_MAX) {
        osal_printk("MEASURE_DIS STORE LOCAL IQ FAIL. offset:%u samp_cnt:%u max:%u.\r\n",
                    offset, report->samp_cnt, IQ_DATA_MAX);
        return ERRCODE_INVALID_PARAM;
    }

    g_md_local_iq_data.samp_cnt += report->samp_cnt;
    for (uint8_t i = 0; i < report->samp_cnt; i++) {
        g_md_local_iq_data.data[offset + i].i_data = report->i_data[i];
        g_md_local_iq_data.data[offset + i].q_data = report->q_data[i];
    }
    g_next_idx = report->report_idx + 1;

#if (defined(GLE_CS_MODE3_SUPPORT))
    osal_printk("local tof_result = %d.%03d \r\n", report->tof_result / MEASURE_DIS_NUM_CARRY_1000,
                report->tof_result % MEASURE_DIS_NUM_CARRY_1000);
#endif

    return ERRCODE_SUCC;
}

errcode_t measure_dis_proc_local_iq(uint16_t conn_id, sle_channel_sounding_iq_report_t *report)
{
    unused(conn_id);
    if (report == NULL) {
        osal_printk("LOCAL IQ PROC FAIL. report is null.\r\n");
        return ERRCODE_INVALID_PARAM;
    }

    if (report->report_idx == 0) {
        osal_printk("RECEIVE LOCAL IQ. timestamp_sn:%d \r\n", report->timestamp_sn);
    }

    errcode_t ret = measure_dis_store_local_iq(report);
    if (ret != ERRCODE_SUCC) {
        measure_dis_reset_iq_state();
        return ret;
    }

    if ((report->report_idx + 1) == MEASURE_DIS_IQ_REPORT_CNT_MAX) {
        g_md_store_local_iq_complete = true;
        osal_printk("store local iq data complete. \r\n");
        measure_dis_print_iq_data(0, 0, (sle_channel_sounding_iq_trans_t *)(&g_md_local_iq_data));
    }
    return ERRCODE_SUCC;
}

errcode_t measure_dis_proc_remote_iq(uint16_t conn_id, sle_channel_sounding_iq_trans_t *report)
{
    unused(conn_id);
    if ((report == NULL) || !measure_dis_iq_sample_count_valid(report->samp_cnt)) {
        osal_printk("REMOTE IQ PROC FAIL. report:%p samp_cnt:%u.\r\n",
                    report, (report == NULL) ? 0 : report->samp_cnt);
        return ERRCODE_INVALID_PARAM;
    }

    measure_dis_stored_iq_data_t local_iq_snapshot;
    uint32_t irq_sts = osal_irq_lock();
    bool local_complete = (g_md_store_local_iq_complete != false);
    if (local_complete) {
        (void)memcpy_s(&local_iq_snapshot, sizeof(local_iq_snapshot),
                       &g_md_local_iq_data, sizeof(g_md_local_iq_data));
        g_md_store_local_iq_complete = false;
        g_measure_dis_alg_busy = true;
    }
    osal_irq_restore(irq_sts);

    if (!local_complete) {
        osal_printk("REMOTE IQ WAIT LOCAL. remote timestamp_sn:%d.\r\n", report->timestamp_sn);
        return ERRCODE_SUCC;
    }

    measure_dis_print_iq_data(0, 1, report);
    if (measure_dis_timestamp_diff(local_iq_snapshot.timestamp_sn,
                                   report->timestamp_sn) >= MEASURE_DIS_TIMESTAMP_DIFF_MAX) {
        osal_printk("local:%d, remote:%d. \r\n",
                    local_iq_snapshot.timestamp_sn, report->timestamp_sn);
        g_measure_dis_alg_busy = false;
        return ERRCODE_INVALID_PARAM;
    }

    osal_printk("local ts = %d, remote ts = %d. local complete:%d.\r\n",
                local_iq_snapshot.timestamp_sn, report->timestamp_sn, local_complete);

    measure_dis_profile_msg_dis_t measure_dis_dis_temp = { report->timestamp_sn, 0, 0, 0, 0, 0, 0, 0 };
    errcode_t ret = measure_dis_posalg_get_distance(&measure_dis_dis_temp, &local_iq_snapshot, report);
    g_measure_dis_alg_busy = false;
    if (ret != ERRCODE_SUCC) {
        osal_printk("REMOTE IQ CALC FAIL. timestamp_sn:%d ret:0x%x.\r\n", report->timestamp_sn, ret);
        return ret;
    }

    measure_dis_indicator_update_distance(measure_dis_dis_temp.dist_first);
    return ERRCODE_SUCC;
}

void measure_dis_read_local_cs_caps_cb(sle_channel_sounding_caps_t *caps, errcode_t status)
{
    unused(caps);
    osal_printk("SLEM READ LOCAL CAPS. status:%d. \r\n", status);
}

void measure_dis_read_remote_cs_caps_cb(uint16_t conn_id, sle_channel_sounding_caps_t *caps, errcode_t status)
{
    unused(caps);
    unused(conn_id);
    osal_printk("SLEM READ REMOTE CAPS. status:%d. \r\n", status);
}

void measure_dis_set_cs_param_cb(uint16_t conn_id, errcode_t status)
{
    unused(conn_id);
    osal_printk("SLEM SET PARAM. status:%d. \r\n", status);
}

void measure_dis_cs_state_changed_cb(uint8_t slem_status, errcode_t status)
{
    unused(slem_status);
    osal_printk("SLEM STATE CHANGED. status:%d. \r\n", status);
}

void measure_dis_cs_iq_report_cb(uint16_t conn_id, sle_channel_sounding_iq_report_t *report)
{
    if ((report == NULL) || (conn_id == 0xFFFF)) {
        osal_printk("LOCAL IQ CB FAIL. conn_id:%u report:%p.\r\n", conn_id, report);
        return;
    }

    errcode_t ret = measure_dis_proc_local_iq(conn_id, report);
    if (ret != ERRCODE_SUCC) {
        osal_printk("LOCAL IQ CB PROC FAIL. conn_id:%u report_idx:%u timestamp_sn:%u ret:0x%x.\r\n",
                    conn_id, report->report_idx, report->timestamp_sn, ret);
    }
}

errcode_t measure_dis_reg_callbacks(void)
{
    sle_hadm_callbacks_t scd_cbks = {0};

    scd_cbks.read_local_cs_caps_cb = measure_dis_read_local_cs_caps_cb;
    scd_cbks.read_remote_cs_caps_cb = measure_dis_read_remote_cs_caps_cb;
    scd_cbks.cs_state_changed_cb = measure_dis_cs_state_changed_cb;
    scd_cbks.cs_iq_report_cb = measure_dis_cs_iq_report_cb;

    errcode_t ret = sle_hadm_register_callbacks(&scd_cbks);
    osal_printk("SCD SET CALLBACK DONE. ret = %x.\r\n", ret);
    return ret;
}

errcode_t measure_dis_recv_local_iq(measure_dis_msg_node_t *msg_node)
{
    if ((msg_node == NULL) || (msg_node->data == NULL)) {
        return ERRCODE_INVALID_PARAM;
    }

    errcode_t ret = measure_dis_proc_local_iq(msg_node->conn_id,
                                              (sle_channel_sounding_iq_report_t *)(msg_node->data));
    osal_kfree(msg_node->data);
    return ret;
}

errcode_t measure_dis_recv_remote_iq(measure_dis_msg_node_t *msg_node)
{
    if ((msg_node == NULL) || (msg_node->data == NULL)) {
        return ERRCODE_INVALID_PARAM;
    }

    osal_printk("slem recv remote iq.\n");
    errcode_t ret = measure_dis_proc_remote_iq(msg_node->conn_id,
                                               (sle_channel_sounding_iq_trans_t *)(msg_node->data));
    osal_kfree(msg_node->data);
    return ret;
}

errcode_t measure_dis_match_msg(measure_dis_msg_node_t *msg_node)
{
    if (msg_node == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    errcode_t ret = ERRCODE_FAIL;
    osal_printk("---enter sle_slem_msg_proc, type:%d---\n", msg_node->type);
    switch (msg_node->type) {
        case SLEM_MSG_LOCAL_IQ:
            ret = measure_dis_recv_local_iq(msg_node);
            break;
        case SLEM_MSG_REMOTE_IQ:
            ret = measure_dis_recv_remote_iq(msg_node);
            break;
        default:
            break;
    }
    return ret;
}

errcode_t measure_dis_remote_iq(uint16_t len, uint8_t *value)
{
    if ((value == NULL) || (len != sizeof(sle_channel_sounding_iq_trans_t)) || (g_measure_dis_queue == 0)) {
        osal_printk("remote iq invalid. len:%u expect:%u queue:%lu.\r\n",
                    len, (uint16_t)sizeof(sle_channel_sounding_iq_trans_t), g_measure_dis_queue);
        return ERRCODE_INVALID_PARAM;
    }

    if (g_measure_dis_alg_busy) {
        return ERRCODE_SUCC;
    }

    uint8_t msg_num = osal_msg_queue_get_msg_num(g_measure_dis_queue);
    if (msg_num > 0) {
        osal_printk("remote iq queue backlog:%u, drop.\r\n", msg_num);
        return ERRCODE_SUCC;
    }

    sle_channel_sounding_iq_trans_t *report =
        (sle_channel_sounding_iq_trans_t *)osal_kmalloc(sizeof(sle_channel_sounding_iq_trans_t), 0);
    if (report == NULL) {
        return ERRCODE_MALLOC;
    }
    if (memcpy_s(report, sizeof(sle_channel_sounding_iq_trans_t), value, len) != EOK) {
        osal_kfree(report);
        return ERRCODE_MEMCPY;
    }

    osal_printk("RECEIVE REMOTE IQ. timestamp_sn:%d \r\n", report->timestamp_sn);
    measure_dis_msg_node_t msg_node = {g_measure_dis_conn_id, SLEM_MSG_REMOTE_IQ, 0, report};
    errcode_t ret = sle_measure_dis_msg_add(&msg_node);
    if (ret != ERRCODE_SUCC) {
        osal_kfree(report);
    }

    return ret;
}

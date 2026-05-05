/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE ADV Config. \n
 *
 * History: \n
 * 2023-07-17, Create file. \n
 */

#ifndef SLE_MEASURE_DIS_SERVER_ALG_H
#define SLE_MEASURE_DIS_SERVER_ALG_H

#include <stdbool.h>

#include "sle_hadm_manager.h"

#define MEASURE_DIS_INVALID (-1)
#define MEASURE_DIS_RSSI_INVALID (-128)
#define MEASURE_DIS_HIGH_INVALID (-10)
#define MEASURE_DIS_IQ_REPORT_CNT_MAX 1
#define POSALG_DATA_NUM 79
#define IQ_DATA_MAX (MEASURE_DIS_IQ_REPORT_CNT_MAX * SLE_CS_IQ_REPORT_COUNT)
#define POSALG_RSSI_OFFSET 256
#define POSALG_CALIB_VAL 0.8
#define POSALG_IQ_FLIP_BARRIER 1023
#define POSALG_IQ_FLIP_OFFSET 2048
#define MEASURE_DIS_TIMESTAMP_DIFF_MAX 20

typedef struct {
    int key_num;
    int key_num_all;
} measure_dis_keys_info;

typedef struct {
    uint32_t                  sn;                               /*!< 婵炴潙顑夐崳绡燦 */
    float                     dist_first;                       /*!< 缂佹鍏涚粩瀛樼▔椤忓懐銈撮悹鐑樼箓閳?*/
    float                     dist_second;                      /*!< 缂佹鍏涚花鈺傜▔椤忓懐銈撮悹鐑樼箓閳?*/
    float                     dist_double;                      /*!< 缂佹鍏涚花鈺傜▔椤忓懐銈撮悹鐑樼箓閳?*/
    float                     rssi;                             /*!< 濞ｅ洠鈧啿濞囩€殿喖鎼€?*/
    float                     high;                             /*!< 缂傚喚鍠曟穱濠冩償閿旂偓绁查柛蹇撶－闁挳寮?*/
    float                     prob;                             /*!< 缂傚喚鍠曟穱濠冩償?*/
    unsigned char             smooth_num;                       /*!<闁哄牆顦伴弲銉╃嵁閾忣偆鎷ㄦ繛鍡忓墲閺?*/
} measure_dis_profile_msg_dis_t;

typedef struct {
    uint16_t i_data;
    uint16_t q_data;
} measure_dis_stored_qte_trans_t;

typedef struct {
    uint8_t samp_cnt;
    uint8_t rssi;
    uint16_t es_sn;
    uint32_t timestamp_sn;
    measure_dis_stored_qte_trans_t data[IQ_DATA_MAX];
#if (defined(GLE_CS_MODE3_SUPPORT))
    uint32_t tof_result;
#endif
} measure_dis_stored_iq_data_t;

typedef struct {
    uint16_t i_data;
    uint16_t q_data;
} sle_channel_sounding_qte_trans_t;

typedef struct {
    uint8_t samp_cnt;
    uint8_t rssi;
    uint16_t es_sn;
    uint32_t timestamp_sn;
    sle_channel_sounding_qte_trans_t data[IQ_DATA_MAX];
    uint32_t tof_result;
} sle_channel_sounding_iq_trans_t;

errcode_t measure_dis_reg_callbacks(void);
errcode_t measure_dis_match_msg(measure_dis_msg_node_t *msg_node);
errcode_t measure_dis_remote_iq(uint16_t len, uint8_t *value);
bool measure_dis_alg_is_busy(void);

#endif

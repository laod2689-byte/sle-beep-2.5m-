/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: USB Mouse Sample Source. \n
 *
 * History: \n
 * 2023-07-07, Create file. \n
 */
#include "soc_osal.h"
#include "osal_msgqueue.h"
#include "common_def.h"
#include "app_init.h"
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_SERVER)
#include "sle_measure_dis_server.h"
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_CLIENT)
#include "sle_measure_dis_client.h"
#endif

#define TASK_PRIORITY_HADM          27
#define STACK_SIZE_BASELINE 0x200
#define SLE_MEASURE_DIS_SERVER_STACK_EXTRA 0x1A00
#ifdef CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_SERVER
#define BTH_HADM_SERVICE_STACK_SIZE (STACK_SIZE_BASELINE * 4 + SLE_MEASURE_DIS_SERVER_STACK_EXTRA)
#else
#define BTH_HADM_SERVICE_STACK_SIZE (STACK_SIZE_BASELINE * 4 + 0x200)
#endif

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_SERVER)
void sle_measure_dis_server_task(void)
{
    errcode_t ret;
    ret = measure_dis_server_init();
    check_rc_return_rc(ret, "con sle_measure_dis_server_init");
    while (1) {
        ret = sle_measure_dis_msg_proc(OSAL_MSGQ_WAIT_FOREVER);
        if (ret != ERRCODE_SUCC) {
            osal_printk("measure_dis queue wait failed. ret:0x%x.\r\n", ret);
            continue;
        }
        while (sle_measure_dis_msg_proc(OSAL_MSGQ_NO_WAIT) == ERRCODE_SUCC) {
        }
    }
}
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_CLIENT)
void sle_measure_dis_client_task(void)
{
    uint32_t ret;
    ret = measure_dis_client_init();
    check_rc_return_rc(ret, "con sle_measure_dis_client_init");
}
#endif

static void measure_dis_entry(void)
{
    osal_printk("CARKEY VERSION 2024.3.21 8:00.\r\n");
    osal_task *task_handle = NULL;
    osal_kthread_lock();
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_SERVER)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_measure_dis_server_task,
                                      0, "SLEMeasureDisServer", BTH_HADM_SERVICE_STACK_SIZE);
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_MEASURE_DIS_CLIENT)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_measure_dis_client_task,
                                      0, "SLEMeasureDisClient", BTH_HADM_SERVICE_STACK_SIZE);
#endif
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, TASK_PRIORITY_HADM);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

/* Run the sle_measure_dis_entry. */
app_run(measure_dis_entry);

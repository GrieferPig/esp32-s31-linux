#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "esp_err.h"

#define CAP 64
static TaskHandle_t sampler;
static TaskStatus_t before[CAP], after[CAP];
static char before_names[CAP][configMAX_TASK_NAME_LEN];
static char after_names[CAP][configMAX_TASK_NAME_LEN];
static void noop(void *arg) { (void)arg; }
static UBaseType_t snap(TaskStatus_t *tasks, char names[][configMAX_TASK_NAME_LEN],
                       configRUN_TIME_COUNTER_TYPE *total) {
    /* Flush core1's current idle runtime by scheduling its IPC task.
       This sampler itself wakes core0. No periodic observer runs in-window. */
    ESP_ERROR_CHECK(esp_ipc_call_blocking(1, noop, NULL));
    UBaseType_t n = uxTaskGetSystemState(tasks, CAP, total);
    configASSERT(n != 0);
    for (UBaseType_t i=0; i<n; i++) {
        snprintf(names[i], configMAX_TASK_NAME_LEN, "%s", tasks[i].pcTaskName);
    }
    return n;
}
static void run(void *arg) {
    (void)arg;
    unsigned sample=0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(5000));
        configRUN_TIME_COUNTER_TYPE t0,t1;
        UBaseType_t n0=snap(before,before_names,&t0);
        int64_t wall0=esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(30000));
        UBaseType_t n1=snap(after,after_names,&t1);
        int64_t wall1=esp_timer_get_time();
        /* Keep UART reporting outside the 40-second throughput window. */
        vTaskDelay(pdMS_TO_TICKS(7000));
        printf("CPU_WINDOW {\"sample\":%u,\"start_us\":%"PRIu64",\"end_us\":%"PRIu64
               ",\"wall_us\":%"PRIi64",\"cores\":2,\"before_count\":%u,\"after_count\":%u}\n",
               ++sample,(uint64_t)t0,(uint64_t)t1,wall1-wall0,(unsigned)n0,(unsigned)n1);
        for (unsigned phase=0;phase<2;phase++) {
            TaskStatus_t *tasks=phase?after:before;
            char (*names)[configMAX_TASK_NAME_LEN]=phase?after_names:before_names;
            UBaseType_t n=phase?n1:n0;
            for (UBaseType_t i=0;i<n;i++)
                printf("CPU_TASK {\"phase\":%u,\"id\":%u,\"name\":\"%s\",\"runtime_us\":%"PRIu64"}\n",
                       phase,(unsigned)tasks[i].xTaskNumber,names[i],(uint64_t)tasks[i].ulRunTimeCounter);
        }
        printf("CPU_DONE %u\n",sample);
        fflush(stdout);
    }
}
void cpu_bench_init(void) {
    configASSERT(xTaskCreatePinnedToCore(run,"cpu_sample",4096,NULL,2,&sampler,0)==pdPASS);
}
void cpu_bench_start(void) {
    configASSERT(sampler != NULL);
    xTaskNotifyGive(sampler);
}

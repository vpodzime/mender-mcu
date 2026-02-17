/**
 * @file      os.c
 * @brief     Mender OS interface for FreeRTOS platform
 *
 * Copyright Northern.tech AS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>

#include "alloc.h"
#include "http.h"
#include "log.h"
#include "os.h"
#include "utils.h"

/**
 * @brief Default work queue stack size (kB)
 */
#ifndef CONFIG_MENDER_SCHEDULER_WORK_QUEUE_STACK_SIZE
#define CONFIG_MENDER_SCHEDULER_WORK_QUEUE_STACK_SIZE (12)
#endif /* CONFIG_MENDER_SCHEDULER_WORK_QUEUE_STACK_SIZE */

/**
 * @brief Default work queue priority
 */
#ifndef CONFIG_MENDER_SCHEDULER_WORK_QUEUE_PRIORITY
#define CONFIG_MENDER_SCHEDULER_WORK_QUEUE_PRIORITY (tskIDLE_PRIORITY + 1)
#endif /* CONFIG_MENDER_SCHEDULER_WORK_QUEUE_PRIORITY */

/**
 * @brief Default work queue length
 */
#ifndef CONFIG_MENDER_SCHEDULER_WORK_QUEUE_LENGTH
#define CONFIG_MENDER_SCHEDULER_WORK_QUEUE_LENGTH (10)
#endif /* CONFIG_MENDER_SCHEDULER_WORK_QUEUE_LENGTH */

/**
 * @brief Work context
 */
typedef struct mender_platform_work_t {
    mender_os_scheduler_work_params_t params;       /**< Work parameters */
    SemaphoreHandle_t                 sem_handle;   /**< Semaphore used to indicate work is pending or executing */
    TimerHandle_t                     timer_handle; /**< Timer used to periodically execute work */
    bool                              activated;    /**< Flag indicating the work is activated */
} mender_platform_work_t;

/**
 * @brief Timer callback used to enqueue work when the timer expires
 * @param timer_handle Timer handle
 */
static void mender_os_scheduler_timer_callback(TimerHandle_t timer_handle);

/**
 * @brief Task used to handle work queue
 * @param arg Not used
 */
static void mender_os_scheduler_work_queue_task(void *arg);

/**
 * @brief Work queue handle
 */
static QueueHandle_t mender_os_scheduler_work_queue_handle;

/**
 * @brief Work queue task handle
 */
static TaskHandle_t mender_os_scheduler_work_queue_task_handle;

/**
 * @brief Initial backoff interval, stored so it can be reset
 */
static uint16_t backoff_interval;

mender_err_t
mender_os_scheduler_init(void) {

    /* Create work queue */
    mender_os_scheduler_work_queue_handle = xQueueCreate(CONFIG_MENDER_SCHEDULER_WORK_QUEUE_LENGTH, sizeof(mender_platform_work_t *));
    if (NULL == mender_os_scheduler_work_queue_handle) {
        mender_log_error("Unable to create work queue");
        return MENDER_FAIL;
    }

    /* Create work queue task */
    if (pdPASS
        != xTaskCreate(mender_os_scheduler_work_queue_task,
                       "mender_work_queue",
                       (CONFIG_MENDER_SCHEDULER_WORK_QUEUE_STACK_SIZE * 1024) / sizeof(StackType_t),
                       NULL,
                       CONFIG_MENDER_SCHEDULER_WORK_QUEUE_PRIORITY,
                       &mender_os_scheduler_work_queue_task_handle)) {
        mender_log_error("Unable to create work queue task");
        vQueueDelete(mender_os_scheduler_work_queue_handle);
        mender_os_scheduler_work_queue_handle = NULL;
        return MENDER_FAIL;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_work_create(mender_os_scheduler_work_params_t *work_params, mender_work_t **work) {
    assert(NULL != work_params);
    assert(NULL != work_params->function);
    assert(NULL != work_params->name);
    assert(NULL != work);

    /* Create work context */
    mender_platform_work_t *work_context = mender_calloc(1, sizeof(mender_platform_work_t));
    if (NULL == work_context) {
        mender_log_error("Unable to allocate memory");
        goto FAIL;
    }

    /* Copy work parameters */
    work_context->params.function             = work_params->function;
    work_context->params.period               = work_params->period;
    work_context->params.backoff.max_interval = work_params->backoff.max_interval;
    work_context->params.backoff.interval     = work_params->backoff.interval;

    /* Store the backoff interval so we can reset it */
    backoff_interval = work_params->backoff.interval;

    if (NULL == (work_context->params.name = mender_utils_strdup(work_params->name))) {
        mender_log_error("Unable to allocate memory");
        goto FAIL;
    }

    /* Create semaphore used to protect work function */
    work_context->sem_handle = xSemaphoreCreateBinary();
    if (NULL == work_context->sem_handle) {
        mender_log_error("Unable to create semaphore");
        goto FAIL;
    }

    /* Create timer to handle the work periodically.
     * Use a period of 1 tick as placeholder; actual period is set on activation.
     * pdFALSE = one-shot; we reschedule manually after each execution to support backoff. */
    work_context->timer_handle = xTimerCreate(work_params->name, 1, pdFALSE, (void *)work_context, mender_os_scheduler_timer_callback);
    if (NULL == work_context->timer_handle) {
        mender_log_error("Unable to create timer");
        goto FAIL;
    }

    /* Return handle to the new work */
    *work = work_context;

    return MENDER_OK;

FAIL:

    /* Release resources */
    if (NULL != work_context) {
        if (NULL != work_context->timer_handle) {
            xTimerDelete(work_context->timer_handle, portMAX_DELAY);
        }
        if (NULL != work_context->sem_handle) {
            vSemaphoreDelete(work_context->sem_handle);
        }
        mender_free(work_context->params.name);
        mender_free(work_context);
    }

    return MENDER_FAIL;
}

mender_err_t
mender_os_scheduler_work_activate(mender_work_t *work) {
    assert(NULL != work);

    /* Give semaphore used to protect the work function */
    xSemaphoreGive(work->sem_handle);

    /* Check the timer period */
    if (work->params.period > 0) {

        mender_log_debug("Activating %s every %" PRIu32 " seconds", work->params.name, work->params.period);

        /* Start the timer to handle the work */
        if (pdPASS != xTimerChangePeriod(work->timer_handle, pdMS_TO_TICKS(work->params.period * 1000), portMAX_DELAY)) {
            mender_log_error("Unable to start timer");
            return MENDER_FAIL;
        }

        /* Execute the work now by enqueuing it */
        mender_os_scheduler_timer_callback(work->timer_handle);
    }

    /* Indicate the work has been activated */
    work->activated = true;

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_work_set_period(mender_work_t *work, uint32_t period) {
    assert(NULL != work);

    /* Set timer period */
    work->params.period = period;
    if (work->params.period > 0) {
        if (pdPASS != xTimerChangePeriod(work->timer_handle, pdMS_TO_TICKS(period * 1000), portMAX_DELAY)) {
            mender_log_error("Unable to set timer period");
            return MENDER_FAIL;
        }
    } else {
        xTimerStop(work->timer_handle, portMAX_DELAY);
        work->activated = false;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_work_execute(mender_work_t *work) {
    assert(NULL != work);

    /* Execute the work now by enqueuing it */
    mender_os_scheduler_timer_callback(work->timer_handle);

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_work_deactivate(mender_work_t *work) {
    assert(NULL != work);

    /* Check if the work was activated */
    if (work->activated) {

        /* Stop the timer used to periodically execute the work */
        xTimerStop(work->timer_handle, portMAX_DELAY);

        /* Wait if the work is pending or executing */
        if (pdTRUE != xSemaphoreTake(work->sem_handle, portMAX_DELAY)) {
            mender_log_error("Work '%s' is pending or executing", work->params.name);
            return MENDER_FAIL;
        }

        /* Indicate the work has been deactivated */
        work->activated = false;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_work_delete(mender_work_t *work) {
    if (NULL == work) {
        return MENDER_OK;
    }

    if (NULL != work->timer_handle) {
        xTimerDelete(work->timer_handle, portMAX_DELAY);
    }
    if (NULL != work->sem_handle) {
        vSemaphoreDelete(work->sem_handle);
    }
    mender_free(work->params.name);
    mender_free(work);

    return MENDER_OK;
}

mender_err_t
mender_os_scheduler_exit(void) {

    /* Submit empty work to the work queue, this asks the work queue task to terminate */
    mender_platform_work_t *work = NULL;
    if (pdPASS != xQueueSend(mender_os_scheduler_work_queue_handle, &work, portMAX_DELAY)) {
        mender_log_error("Unable to submit empty work to the work queue");
        return MENDER_FAIL;
    }

    /* Wait for the task to terminate.
     * The task deletes itself; we wait by polling its state.
     * A brief delay avoids busy-waiting. */
    while (eDeleted != eTaskGetState(mender_os_scheduler_work_queue_task_handle)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Clean up the queue */
    vQueueDelete(mender_os_scheduler_work_queue_handle);
    mender_os_scheduler_work_queue_handle = NULL;

    return MENDER_OK;
}

static void
mender_os_scheduler_timer_callback(TimerHandle_t timer_handle) {
    /* Get work context */
    mender_platform_work_t *work = (mender_platform_work_t *)pvTimerGetTimerID(timer_handle);
    assert(NULL != work);

    /* Exit if the work is already pending or executing */
    if (pdTRUE != xSemaphoreTake(work->sem_handle, 0)) {
        mender_log_debug("Work '%s' is not activated, already pending or executing", work->params.name);
        return;
    }

    /* Submit the work to the work queue */
    if (pdPASS != xQueueSend(mender_os_scheduler_work_queue_handle, &work, 0)) {
        mender_log_warning("Unable to submit work '%s' to the work queue", work->params.name);
        xSemaphoreGive(work->sem_handle);
    }
}

static void
mender_os_scheduler_work_queue_task(MENDER_ARG_UNUSED void *arg) {
    mender_platform_work_t *work = NULL;

    /* Handle work to be executed */
    while (pdTRUE == xQueueReceive(mender_os_scheduler_work_queue_handle, &work, portMAX_DELAY)) {

        /* Check if empty work is received from the work queue, this asks the work queue task to terminate */
        if (NULL == work) {
            goto END;
        }

        /* Call work function */
        mender_log_debug("Executing %s work", work->params.name);
        mender_err_t ret    = work->params.function();
        uint32_t     period = work->params.period;

        if (MENDER_DONE == ret) {
            /* Reset the backoff */
            work->params.backoff.interval = backoff_interval;
            /* Release semaphore and stop — nothing more to do */
            xSemaphoreGive(work->sem_handle);
            continue;
        }
        if (MENDER_OK != ret) {
            if (MENDER_RETRY_ERROR == ret) {
                /* Check if there's a rate-limit interval */
                uint32_t retry_interval = mender_http_get_retry_interval();
                if (retry_interval > 0) {
                    /* Use the rate-limit interval from the server */
                    mender_log_debug("Rate limit detected, retrying");
                    period = retry_interval;
                } else {
                    /* Normal exponential backoff */
                    mender_log_debug("Retry error detected, retrying with backoff");
                    period                        = work->params.backoff.interval;
                    uint16_t next                 = work->params.backoff.interval * 2;
                    work->params.backoff.interval = (next >= work->params.backoff.max_interval) ? work->params.backoff.max_interval : next;
                }
            }
            mender_log_error("Work %s failed, retrying in %" PRIu32 " seconds", work->params.name, period);
        } else {
            /* Reset the backoff */
            work->params.backoff.interval = backoff_interval;
        }

        /* Release semaphore used to protect the work function */
        xSemaphoreGive(work->sem_handle);

        /* Reschedule the timer for the next period if work is still activated */
        if (work->activated && period > 0) {
            xTimerChangePeriod(work->timer_handle, pdMS_TO_TICKS(period * 1000), portMAX_DELAY);
        }
    }

END:
    /* Terminate work queue task */
    vTaskDelete(NULL);
}

mender_err_t
mender_os_mutex_create(void **handle) {
    assert(NULL != handle);

    /* Create mutex */
    *handle = (void *)xSemaphoreCreateMutex();
    if (NULL == *handle) {
        return MENDER_FAIL;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_mutex_take(void *handle, int32_t delay_ms) {
    assert(NULL != handle);

    /* Take mutex */
    TickType_t ticks = (delay_ms >= 0) ? pdMS_TO_TICKS((uint32_t)delay_ms) : portMAX_DELAY;
    if (pdTRUE != xSemaphoreTake((SemaphoreHandle_t)handle, ticks)) {
        return MENDER_FAIL;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_mutex_give(void *handle) {
    assert(NULL != handle);

    /* Give mutex */
    if (pdTRUE != xSemaphoreGive((SemaphoreHandle_t)handle)) {
        return MENDER_FAIL;
    }

    return MENDER_OK;
}

mender_err_t
mender_os_mutex_delete(void *handle) {

    /* Release mutex */
    if (NULL != handle) {
        vSemaphoreDelete((SemaphoreHandle_t)handle);
    }

    return MENDER_OK;
}

void
mender_os_reboot(void) {
    /* FreeRTOS has no standard reboot API.
     * Users should provide a hardware-specific implementation by overriding
     * this weak symbol or using the weak platform implementation pattern. */
    return;
}

void
mender_os_sleep(uint32_t period_ms) {
    vTaskDelay(pdMS_TO_TICKS(period_ms));
}

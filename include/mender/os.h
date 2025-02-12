/**
 * @file      os.h
 * @brief     Mender OS interface
 *
 * Copyright joelguittet and mender-mcu-client contributors
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

#ifndef __MENDER_OS_H__
#define __MENDER_OS_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <mender/utils.h>

/**
 * @brief Work parameters
 */
typedef struct {
    mender_err_t (*function)(void); /**< Work function */
    uint32_t period;                /**< Work period (seconds), 0 to disable periodic execution */
    char    *name;                  /**< Work name */
} mender_os_scheduler_work_params_t;

/**
 * @brief Work item
 * @note  This is an opaque type the implementation of which is platform-dependent.
 */
typedef struct mender_platform_work_t mender_work_t;

/**
 * @brief Initialization of the scheduler
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_init(void);

/**
 * @brief Function used to register a new work
 * @param work_params Work parameters
 * @param handle Work handle if the function succeeds, NULL otherwise
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_create(mender_os_scheduler_work_params_t *work_params, mender_work_t **work);

/**
 * @brief Function used to activate a work
 * @param handle Work handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_activate(mender_work_t *work);

/**
 * @brief Function used to set work period
 * @param handle Work handle
 * @param period Work period (seconds)
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_set_period(mender_work_t *work, uint32_t period);

/**
 * @brief Function used to trigger execution of the work
 * @param handle Work handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_execute(mender_work_t *work);

/**
 * @brief Function used to deactivate a work
 * @param handle Work handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_deactivate(mender_work_t *work);

/**
 * @brief Function used to delete a work
 * @param handle Work handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_work_delete(mender_work_t *work);

/**
 * @brief Release mender scheduler
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_scheduler_exit(void);

/**
 * @brief Function used to create a mutex
 * @param handle Mutex handle if the function succeeds, NULL otherwise
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_mutex_create(void **handle);

/**
 * @brief Function used to take a mutex
 * @param handle Mutex handle
 * @param delay_ms Delay to obtain the mutex, -1 to block indefinitely (without a timeout)
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_mutex_take(void *handle, int32_t delay_ms);

/**
 * @brief Function used to give a mutex
 * @param handle Mutex handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_mutex_give(void *handle);

/**
 * @brief Function used to delete a mutex
 * @param handle Mutex handle
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
mender_err_t mender_os_mutex_delete(void *handle);

/**
 * @brief Unconditionally reboot the system (e.g. if the reboot callback fails to do so)
 */
void mender_os_reboot(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __MENDER_OS_H__ */

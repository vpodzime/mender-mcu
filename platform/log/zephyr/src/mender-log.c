/**
 * @file      mender-log.c
 * @brief     Mender logging interface for Zephyr platform
 *
 * Copyright joelguittet and mender-mcu-client contributors
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

#include "mender-log.h"

#define LOG_MESSAGE_MAX_SIZE_BYTES 256

mender_err_t
mender_log_init(void) {

    /* Nothing to do */
    return MENDER_OK;
}

mender_err_t
mender_log_print(uint8_t level, const char *filename, const char *function, int line, char *format, ...) {

    (void)filename;

    va_list args;
    va_start(args, format);

    char log_buff[LOG_MESSAGE_MAX_SIZE_BYTES];
    int  ret = vsnprintf(log_buff, LOG_MESSAGE_MAX_SIZE_BYTES, format, args);

    va_end(args);

    if (ret < 0) {
        printf("error: logger error: log message formatting failed: %d", ret);
        return MENDER_FAIL;
    } else if (ret >= LOG_MESSAGE_MAX_SIZE_BYTES) {
        /* Log message is too long; add ... at the end */
        memset(&log_buff[LOG_MESSAGE_MAX_SIZE_BYTES - 4], '.', 3);
    }

    /* Switch depending log level */
    switch (level) {
        case MENDER_LOG_LEVEL_ERR:
            printf("error: %s\n", log_buff);
            break;
        case MENDER_LOG_LEVEL_WRN:
            printf("warning: %s\n", log_buff);
            break;
        case MENDER_LOG_LEVEL_INF:
            printf("info: %s\n", log_buff);
            break;
        case MENDER_LOG_LEVEL_DBG:
            printf("debug [%s (%d)]: %s\n", function, line, log_buff);
            break;
        default:
            break;
    }

    return MENDER_OK;
}

mender_err_t
mender_log_exit(void) {

    /* Nothing to do */
    return MENDER_OK;
}

/**
 * @file      alloc.c
 * @brief     FreeRTOS-specific implementation of the Mender memory management functions
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

#include <string.h>

#include <FreeRTOS.h>

#include "alloc.h"

/**
 * @brief Size header prepended to each allocation to support realloc.
 *
 * FreeRTOS does not provide a standard realloc. We store the allocation
 * size in a header before the returned pointer so that realloc can copy
 * the correct number of bytes.
 */
typedef struct {
    size_t size;
} mender_freertos_alloc_header_t;

static void *
mender_freertos_malloc(size_t size) {
    mender_freertos_alloc_header_t *header = pvPortMalloc(sizeof(mender_freertos_alloc_header_t) + size);
    if (NULL == header) {
        return NULL;
    }
    header->size = size;
    return (void *)(header + 1);
}

static void *
mender_freertos_realloc(void *ptr, size_t size) {
    if (NULL == ptr) {
        return mender_freertos_malloc(size);
    }
    if (0 == size) {
        mender_freertos_alloc_header_t *header = ((mender_freertos_alloc_header_t *)ptr) - 1;
        vPortFree(header);
        return NULL;
    }

    mender_freertos_alloc_header_t *old_header = ((mender_freertos_alloc_header_t *)ptr) - 1;
    size_t                          old_size   = old_header->size;

    void *new_ptr = mender_freertos_malloc(size);
    if (NULL == new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, (old_size < size) ? old_size : size);
    vPortFree(old_header);

    return new_ptr;
}

static void
mender_freertos_free(void *ptr) {
    if (NULL != ptr) {
        mender_freertos_alloc_header_t *header = ((mender_freertos_alloc_header_t *)ptr) - 1;
        vPortFree(header);
    }
}

void
mender_set_platform_allocation_funcs(void) {
    mender_set_allocation_funcs(mender_freertos_malloc, mender_freertos_realloc, mender_freertos_free);
}

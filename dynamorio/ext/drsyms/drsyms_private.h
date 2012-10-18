/* **********************************************************
 * Copyright (c) 2011-2012 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* drsyms private header.
 */

#ifndef DRSYMS_PRIVATE_H
#define DRSYMS_PRIVATE_H

#include "drsyms.h"

#define BUFFER_SIZE_BYTES(buf)      sizeof(buf)
#define BUFFER_SIZE_ELEMENTS(buf)   (BUFFER_SIZE_BYTES(buf) / sizeof(buf[0]))
#define BUFFER_LAST_ELEMENT(buf)    buf[BUFFER_SIZE_ELEMENTS(buf) - 1]
#define NULL_TERMINATE_BUFFER(buf)  BUFFER_LAST_ELEMENT(buf) = 0

#ifndef MIN
# define MIN(x, y) ((x) <= (y) ? (x) : (y))
#endif

#define IS_SIDELINE (shmid != 0)

#define NOTIFY(...) do { \
    if (verbose) { \
        dr_fprintf(STDERR, __VA_ARGS__); \
    } \
} while (0)

#define ALIGN_FORWARD(x, alignment) \
    ((((ptr_uint_t)x) + ((alignment)-1)) & (~((alignment)-1)))

/* check if all bits in mask are set in var */
#define TESTALL(mask, var) (((mask) & (var)) == (mask))
/* check if any bit in mask is set in var */
#define TESTANY(mask, var) (((mask) & (var)) != 0)
/* check if a single bit is set in var */
#define TEST TESTANY

/* Memory pool that uses externally allocated memory.
 */
typedef struct _mempool_t {
    char *base;
    size_t size;
    char *cur;
} mempool_t;

/* Initialize the memory pool to point at an external sized buffer.  This pool
 * does not perform heap allocations to initialize or grow the pool, and hence
 * does not require any finalization.
 */
void pool_init(mempool_t *pool, char *buf, size_t sz);

/* Returned memory is 8-byte aligned on all platforms.
 * Good for everything except floats or SSE.
 */
void *pool_alloc(mempool_t *pool, size_t sz);

#define POOL_ALLOC(pool, type) \
    ((type*)pool_alloc(pool, sizeof(type)))
#define POOL_ALLOC_SIZE(pool, type, size) \
    ((type*)pool_alloc(pool, (size)))

/***************************************************************************
 * Cygwin interface from Unix to Windows
 * For all of these, the caller is responsible for synchronization
 */

void
drsym_unix_init(void);

void
drsym_unix_exit(void);

void *
drsym_unix_load(const char *modpath);

void
drsym_unix_unload(void *p);

drsym_error_t
drsym_unix_lookup_address(void *moddata, size_t modoffs,
                          drsym_info_t *out INOUT, uint flags);

drsym_error_t
drsym_unix_lookup_symbol(void *moddata, const char *symbol, size_t *modoffs OUT,
                         uint flags);

drsym_error_t
drsym_unix_enumerate_symbols(void *moddata, drsym_enumerate_cb callback, void *data,
                             uint flags);

size_t
drsym_unix_demangle_symbol(char *dst OUT, size_t dst_sz, const char *mangled,
                           uint flags);

drsym_error_t
drsym_unix_get_type(void *mod_in, size_t modoffs, uint levels_to_expand,
                    char *buf, size_t buf_sz, drsym_type_t **type OUT);

drsym_error_t
drsym_unix_get_func_type(void *moddata, size_t modoffs, char *buf,
                         size_t buf_sz, drsym_func_type_t **func_type OUT);

drsym_error_t
drsym_unix_expand_type(const char *modpath, uint type_id, uint levels_to_expand,
                       char *buf, size_t buf_sz,
                       drsym_type_t **expanded_type OUT);

drsym_error_t
drsym_unix_get_module_debug_kind(void *moddata, drsym_debug_kind_t *kind OUT);

#endif /* DRSYMS_PRIVATE_H */

/* -*- C -*-
 *
 * Copyright (c) 2017 Intel Corporation. All rights reserved.
 * This software is available to you under the BSD license.
 *
 * This file is part of the Sandia OpenSHMEM software package. For license
 * information, see the LICENSE file in the top level directory of the
 * distribution.
 *
 */

#include "config.h"

#ifdef HAVE_SYS_GETTID
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/syscall.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/param.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <netdb.h>

#if HAVE_FNMATCH_H
#include <fnmatch.h>
#else
#define fnmatch(P, S, F) strcmp(P, S)
#endif

#define SHMEM_INTERNAL_INCLUDE
#include "shmem.h"
#include "shmem_internal.h"
#include "shmem_comm.h"
#include "transport_ofi.h"
#include <unistd.h>
#include "runtime.h"
#include "uthash.h"

struct fabric_info {
    struct fi_info *fabrics;
    struct fi_info *p_info;
    char *prov_name;
    char *fabric_name;
    char *domain_name;
    int npes;
};

struct fid_fabric*              shmem_transport_ofi_fabfd;
struct fid_domain*              shmem_transport_ofi_domainfd;
struct fid_av*                  shmem_transport_ofi_avfd;
struct fid_ep*                  shmem_transport_ofi_target_ep;
#if ENABLE_TARGET_CNTR
struct fid_cntr*                shmem_transport_ofi_target_cntrfd;
#endif
#ifdef ENABLE_MR_SCALABLE
#ifdef ENABLE_REMOTE_VIRTUAL_ADDRESSING
struct fid_mr*                  shmem_transport_ofi_target_mrfd;
#else  /* !ENABLE_REMOTE_VIRTUAL_ADDRESSING */
struct fid_mr*                  shmem_transport_ofi_target_heap_mrfd;
struct fid_mr*                  shmem_transport_ofi_target_data_mrfd;
#endif
#else  /* !ENABLE_MR_SCALABLE */
struct fid_mr*                  shmem_transport_ofi_target_heap_mrfd;
struct fid_mr*                  shmem_transport_ofi_target_data_mrfd;
uint64_t*                       shmem_transport_ofi_target_heap_keys;
uint64_t*                       shmem_transport_ofi_target_data_keys;
#ifndef ENABLE_REMOTE_VIRTUAL_ADDRESSING
uint8_t**                       shmem_transport_ofi_target_heap_addrs;
uint8_t**                       shmem_transport_ofi_target_data_addrs;
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* ENABLE_MR_SCALABLE */
uint64_t                        shmem_transport_ofi_max_poll;
long                            shmem_transport_ofi_put_poll_limit;
long                            shmem_transport_ofi_get_poll_limit;
size_t                          shmem_transport_ofi_max_buffered_send;
size_t                          shmem_transport_ofi_max_msg_size;
size_t                          shmem_transport_ofi_bounce_buffer_size;
long                            shmem_transport_ofi_max_bounce_buffers;
size_t                          shmem_transport_ofi_addrlen;
#ifdef ENABLE_MR_RMA_EVENT
int                             shmem_transport_ofi_mr_rma_event;
#endif
fi_addr_t                       *addr_table;
#ifdef ENABLE_THREADS
shmem_internal_mutex_t          shmem_transport_ofi_lock;
pthread_mutex_t                 shmem_transport_ofi_progress_lock = PTHREAD_MUTEX_INITIALIZER;
#endif /* ENABLE_THREADS */

/* Need a syscall to gettid() because glibc doesn't provide a wrapper
 * (see gettid manpage in the NOTES section): */
static inline
struct shmem_internal_tid shmem_transport_ofi_gettid(void)
{
    struct shmem_internal_tid tid;
    memset(&tid, 0, sizeof(struct shmem_internal_tid));

    if (shmem_internal_gettid_fn) {
        tid.tid_t = tid_is_uint64_t;
        tid.val.uint64_val = (*shmem_internal_gettid_fn)();
    } else {
#ifndef __APPLE__
#ifdef HAVE_SYS_GETTID
        tid.tid_t = tid_is_pid_t;
        tid.val.pid_val = syscall(SYS_gettid);
#else
        /* Cannot query the tid with a syscall, so instead assume each tid
         * query corresponds to a unique thread. */
        tid.tid_t = tid_is_uint64_t;
        static uint64_t tid_val = 0;
        static int tid_cnt_start = 0;
        if (!tid_cnt_start)
            tid_cnt_start = 1;
        else
            tid_val++;
        tid.val.uint64_val = tid_val;
#endif /* HAVE_SYS_GETTID */
#else
        tid.tid_t = tid_is_uint64_t;
        int ret;
        ret = pthread_threadid_np(NULL, &tid.val.uint64_val);
        if (ret != 0)
            RAISE_ERROR_MSG("Error getting thread ID: %s\n", strerror(ret));
#endif /* APPLE */
    }
    return tid;
}

static struct fabric_info shmem_transport_ofi_info = {0};

static shmem_transport_ctx_t** shmem_transport_ofi_contexts = NULL;
static size_t shmem_transport_ofi_contexts_len = 0;
static size_t shmem_transport_ofi_grow_size = 128;

#define SHMEM_TRANSPORT_CTX_DEFAULT_ID -1
shmem_transport_ctx_t shmem_transport_ctx_default;
shmem_ctx_t SHMEM_CTX_DEFAULT = (shmem_ctx_t) &shmem_transport_ctx_default;

size_t SHMEM_Dtsize[FI_DATATYPE_LAST];

static char * SHMEM_DtName[FI_DATATYPE_LAST];
static char * SHMEM_OpName[FI_ATOMIC_OP_LAST];

static inline void init_ofi_tables(void)
{
    SHMEM_Dtsize[FI_INT8]                = sizeof(int8_t);
    SHMEM_Dtsize[FI_UINT8]               = sizeof(uint8_t);
    SHMEM_Dtsize[FI_INT16]               = sizeof(int16_t);
    SHMEM_Dtsize[FI_UINT16]              = sizeof(uint16_t);
    SHMEM_Dtsize[FI_INT32]               = sizeof(int32_t);
    SHMEM_Dtsize[FI_UINT32]              = sizeof(uint32_t);
    SHMEM_Dtsize[FI_INT64]               = sizeof(int64_t);
    SHMEM_Dtsize[FI_UINT64]              = sizeof(uint64_t);
    SHMEM_Dtsize[FI_FLOAT]               = sizeof(float);
    SHMEM_Dtsize[FI_DOUBLE]              = sizeof(double);
    SHMEM_Dtsize[FI_FLOAT_COMPLEX]       = sizeof(float _Complex);
    SHMEM_Dtsize[FI_DOUBLE_COMPLEX]      = sizeof(double _Complex);
    SHMEM_Dtsize[FI_LONG_DOUBLE]         = sizeof(long double);
    SHMEM_Dtsize[FI_LONG_DOUBLE_COMPLEX] = sizeof(long double _Complex);

    SHMEM_DtName[FI_INT8]                = "int8";
    SHMEM_DtName[FI_UINT8]               = "uint8";
    SHMEM_DtName[FI_INT16]               = "int16";
    SHMEM_DtName[FI_UINT16]              = "uint16";
    SHMEM_DtName[FI_INT32]               = "int32";
    SHMEM_DtName[FI_UINT32]              = "uint32";
    SHMEM_DtName[FI_INT64]               = "int64";
    SHMEM_DtName[FI_UINT64]              = "uint64";
    SHMEM_DtName[FI_FLOAT]               = "float";
    SHMEM_DtName[FI_DOUBLE]              = "double";
    SHMEM_DtName[FI_FLOAT_COMPLEX]       = "float _Complex";
    SHMEM_DtName[FI_DOUBLE_COMPLEX]      = "double _Complex";
    SHMEM_DtName[FI_LONG_DOUBLE]         = "long double";
    SHMEM_DtName[FI_LONG_DOUBLE_COMPLEX] = "long double _Complex";

    SHMEM_OpName[FI_MIN]                 = "MIN";
    SHMEM_OpName[FI_MAX]                 = "MAX";
    SHMEM_OpName[FI_SUM]                 = "SUM";
    SHMEM_OpName[FI_PROD]                = "PROD";
    SHMEM_OpName[FI_LOR]                 = "LOR";
    SHMEM_OpName[FI_LAND]                = "LAND";
    SHMEM_OpName[FI_BOR]                 = "BOR";
    SHMEM_OpName[FI_BAND]                = "BAND";
    SHMEM_OpName[FI_LXOR]                = "LXOR";
    SHMEM_OpName[FI_BXOR]                = "BXOR";
    SHMEM_OpName[FI_ATOMIC_READ]         = "ATOMIC_WRITE";
    SHMEM_OpName[FI_ATOMIC_WRITE]        = "ATOMIC_READ";
    SHMEM_OpName[FI_CSWAP]               = "CSWAP";
    SHMEM_OpName[FI_CSWAP_NE]            = "CSWAP_NE";
    SHMEM_OpName[FI_CSWAP_LE]            = "CSWAP_LE";
    SHMEM_OpName[FI_CSWAP_LT]            = "CSWAP_LT";
    SHMEM_OpName[FI_CSWAP_GE]            = "CSWAP_GE";
    SHMEM_OpName[FI_CSWAP_GT]            = "CSWAP_GT";
    SHMEM_OpName[FI_MSWAP]               = "MSWAP";
}

/* Cover OpenSHMEM atomics API */

#define SIZEOF_AMO_DT 5
static int DT_AMO_STANDARD[]=
{
    SHM_INTERNAL_INT, SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG,
    SHM_INTERNAL_INT32, SHM_INTERNAL_INT64
};
#define SIZEOF_AMO_OPS 1
static int AMO_STANDARD_OPS[]=
{
    SHM_INTERNAL_SUM
};
#define SIZEOF_AMO_FOPS 1
static int FETCH_AMO_STANDARD_OPS[]=
{
    SHM_INTERNAL_SUM
};
#define SIZEOF_AMO_COPS 1
static int COMPARE_AMO_STANDARD_OPS[]=
{
    FI_CSWAP
};

/* Note: Fortran-specific types should be last so they can be disabled here */
#ifdef ENABLE_FORTRAN
#define SIZEOF_AMO_EX_DT 8
#else
#define SIZEOF_AMO_EX_DT 7
#endif
static int DT_AMO_EXTENDED[]=
{
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_INT, SHM_INTERNAL_LONG,
    SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32, SHM_INTERNAL_INT64,
    SHM_INTERNAL_FORTRAN_INTEGER
};
#define SIZEOF_AMO_EX_OPS 1
static int AMO_EXTENDED_OPS[]=
{
    FI_ATOMIC_WRITE
};
#define SIZEOF_AMO_EX_FOPS 2
static int FETCH_AMO_EXTENDED_OPS[]=
{
    FI_ATOMIC_WRITE, FI_ATOMIC_READ
};


/* Cover one-sided implementation of reduction */

#define SIZEOF_RED_DT 6
static int DT_REDUCE_BITWISE[]=
{
    SHM_INTERNAL_SHORT, SHM_INTERNAL_INT, SHM_INTERNAL_LONG,
    SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32, SHM_INTERNAL_INT64
};
#define SIZEOF_RED_OPS 3
static int REDUCE_BITWISE_OPS[]=
{
    SHM_INTERNAL_BAND, SHM_INTERNAL_BOR, SHM_INTERNAL_BXOR
};


#define SIZEOF_REDC_DT 9
static int DT_REDUCE_COMPARE[]=
{
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_SHORT,
    SHM_INTERNAL_INT, SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG,
    SHM_INTERNAL_INT32, SHM_INTERNAL_INT64, SHM_INTERNAL_LONG_DOUBLE
};
#define SIZEOF_REDC_OPS 2
static int REDUCE_COMPARE_OPS[]=
{
    SHM_INTERNAL_MAX, SHM_INTERNAL_MIN
};


#define SIZEOF_REDA_DT 11
static int DT_REDUCE_ARITH[]=
{
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_FLOAT_COMPLEX,
    SHM_INTERNAL_DOUBLE_COMPLEX, SHM_INTERNAL_SHORT, SHM_INTERNAL_INT,
    SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32,
    SHM_INTERNAL_INT64, SHM_INTERNAL_LONG_DOUBLE
};
#define SIZEOF_REDA_OPS 2
static int REDUCE_ARITH_OPS[]=
{
    SHM_INTERNAL_SUM, SHM_INTERNAL_PROD
};

/* Internal to SHMEM implementation atomic requirement */
/* Locking implementation requirement */
#define SIZEOF_INTERNAL_REQ_DT 1
static int DT_INTERNAL_REQ[]=
{
    SHM_INTERNAL_INT
};
#define SIZEOF_INTERNAL_REQ_OPS 1
static int INTERNAL_REQ_OPS[]=
{
    FI_MSWAP
};

typedef enum{
    ATOMIC_NO_SUPPORT,
    ATOMIC_WARNINGS,
    ATOMIC_SOFT_SUPPORT,
}atomic_support_lv;


/* default CQ depth */
uint64_t shmem_transport_ofi_max_poll = (1ULL<<30);


enum stx_allocator_t {
    ROUNDROBIN = 0,
    RANDOM
};
typedef enum stx_allocator_t stx_allocator_t;
static stx_allocator_t shmem_transport_ofi_stx_allocator;

static long shmem_transport_ofi_stx_max;
static long shmem_transport_ofi_stx_threshold;

struct shmem_transport_ofi_stx_t {
    struct fid_stx*   stx;
    long              ref_cnt;
    int               is_private;
};
typedef struct shmem_transport_ofi_stx_t shmem_transport_ofi_stx_t;
static shmem_transport_ofi_stx_t* shmem_transport_ofi_stx_pool;

struct shmem_transport_ofi_stx_kvs_t {
    int                         stx_idx;
    struct shmem_internal_tid   tid;
    UT_hash_handle              hh;
};
typedef struct shmem_transport_ofi_stx_kvs_t shmem_transport_ofi_stx_kvs_t;
static shmem_transport_ofi_stx_kvs_t* shmem_transport_ofi_stx_kvs = NULL;

#define SHMEM_TRANSPORT_OFI_DUMP_STX()                                                          \
    do {                                                                                        \
        char stx_str[256];                                                                      \
        int i, offset;                                                                          \
                                                                                                \
        for (i = offset = 0; i < shmem_transport_ofi_stx_max; i++)                              \
            offset += snprintf(stx_str+offset, 256-offset,                                      \
                               (i == shmem_transport_ofi_stx_max-1) ? "%ld%s" : "%ld%s ",       \
                               shmem_transport_ofi_stx_pool[i].ref_cnt,                         \
                               shmem_transport_ofi_stx_pool[i].is_private ? "P" : "S");         \
                                                                                                \
        DEBUG_MSG("STX[%ld] = [ %s ]\n", shmem_transport_ofi_stx_max, stx_str);                 \
    } while (0)

static inline
int shmem_transport_ofi_is_private(long options) {
    if (!shmem_internal_params.OFI_STX_DISABLE_PRIVATE &&
        (options & SHMEM_CTX_PRIVATE)) {
        return 1;
    } else {
        return 0;
    }
}

static unsigned int rand_pool_seed;

static inline
void shmem_transport_ofi_stx_rand_init(void) {
    rand_pool_seed = shmem_internal_my_pe;
    return;
}

static inline
int shmem_transport_ofi_stx_search_unused(void)
{
    int stx_idx = -1, i;

    for (i = 0; i < shmem_transport_ofi_stx_max; i++) {
        if (shmem_transport_ofi_stx_pool[i].ref_cnt == 0) {
            shmem_internal_assert(!shmem_transport_ofi_stx_pool[i].is_private);
            stx_idx = i;
            break;
        }
    }

    return stx_idx;
}


static inline
int shmem_transport_ofi_stx_search_shared(long threshold)
{
    static int rr_start_idx = 0;
    int stx_idx = -1, i, count;

    switch (shmem_transport_ofi_stx_allocator) {
        case ROUNDROBIN:
            i = rr_start_idx;
            for (count = 0; count < shmem_transport_ofi_stx_max; count++) {
                if (shmem_transport_ofi_stx_pool[i].ref_cnt > 0 &&
                    (shmem_transport_ofi_stx_pool[i].ref_cnt <= threshold || threshold == -1) &&
                    !shmem_transport_ofi_stx_pool[i].is_private) {
                    stx_idx = i;
                    rr_start_idx = (i + 1) % shmem_transport_ofi_stx_max;
                    break;
                }

                i = (i + 1) % shmem_transport_ofi_stx_max;
            }

            break;

        case RANDOM:
            for (i = count = 0; i < shmem_transport_ofi_stx_max; i++) {
                if (shmem_transport_ofi_stx_pool[i].ref_cnt > 0 &&
                    (shmem_transport_ofi_stx_pool[i].ref_cnt <= threshold || threshold == -1) &&
                    !shmem_transport_ofi_stx_pool[i].is_private)
                {
                    ++count;
                    break;
                }
            }

            if (count == 0)
                break;

            /* Probe at random until we select an available STX */
            else {
                do {
                    stx_idx = (int) (rand_r(&rand_pool_seed) / (RAND_MAX + 1.0) * shmem_transport_ofi_stx_max);
                } while (!(shmem_transport_ofi_stx_pool[stx_idx].ref_cnt > 0 &&
                           (shmem_transport_ofi_stx_pool[stx_idx].ref_cnt <= threshold || threshold == -1) &&
                           !shmem_transport_ofi_stx_pool[stx_idx].is_private));
            }

            break;
        default:
            RAISE_ERROR_MSG("Invalid STX allocator (%d)\n",
                            shmem_transport_ofi_stx_allocator);
    }

    return stx_idx;
}



static inline
void shmem_transport_ofi_stx_allocate(shmem_transport_ctx_t *ctx)
{
    /* SHMEM contexts that are private to the same thread (i.e. have
     * SHMEM_CTX_PRIVATE option set) share the same STX.  */
    if (shmem_transport_ofi_is_private(ctx->options)) {

        shmem_transport_ofi_stx_kvs_t *f;
        HASH_FIND(hh, shmem_transport_ofi_stx_kvs,
                  &ctx->tid, sizeof(struct shmem_internal_tid), f);

        if (f) {
            shmem_transport_ofi_stx_pool[f->stx_idx].ref_cnt++;
            ctx->stx_idx = f->stx_idx;

        } else {
            /* No STX allocated to the given TID, attempt to allocate one */
            int is_unused = 1;
            int stx_idx;
            shmem_transport_ofi_stx_t *stx = NULL;

            stx_idx = shmem_transport_ofi_stx_search_unused();

            /* Couldn't get new STX, assign a shared one */
            /* Note: shared STX allocation is always successful */
            if (stx_idx < 0) {
                DEBUG_STR("private STX unavailable, falling back to STX sharing");
                is_unused = 0;
                stx_idx = shmem_transport_ofi_stx_search_shared(shmem_transport_ofi_stx_threshold);
                if (stx_idx < 0)
                    stx_idx = shmem_transport_ofi_stx_search_shared(-1);
            }

            shmem_internal_assert(stx_idx >= 0);
            stx = &shmem_transport_ofi_stx_pool[stx_idx];
            ctx->stx_idx = stx_idx;
            stx->ref_cnt++;

            if (is_unused) {
                stx->is_private = 1;
                shmem_transport_ofi_stx_kvs_t *e = malloc(sizeof(shmem_transport_ofi_stx_kvs_t));
                if (e == NULL) {
                    RAISE_ERROR_STR("out of memory when allocating STX KVS entry");
                }
                e->tid     = ctx->tid;
                e->stx_idx = ctx->stx_idx;
                HASH_ADD(hh, shmem_transport_ofi_stx_kvs, tid,
                         sizeof(struct shmem_internal_tid), e);
            } else {
                ctx->options &= ~SHMEM_CTX_PRIVATE;
            }
        }
    /* TODO: Optimize this case? else if (ctx->options & SHMEM_CTX_SERIALIZED) */
    } else {
        int stx_idx = shmem_transport_ofi_stx_search_shared(shmem_transport_ofi_stx_threshold);

        if (stx_idx < 0)
            stx_idx = shmem_transport_ofi_stx_search_unused();

        if (stx_idx < 0)
            stx_idx = shmem_transport_ofi_stx_search_shared(-1);

        shmem_internal_assert(stx_idx >= 0);
        ctx->stx_idx = stx_idx;
        shmem_transport_ofi_stx_pool[ctx->stx_idx].ref_cnt++;
    }

    SHMEM_TRANSPORT_OFI_DUMP_STX();

    return;
}

#define OFI_MAJOR_VERSION 1
#ifdef ENABLE_MR_RMA_EVENT
#define OFI_MINOR_VERSION 5
#else
#define OFI_MINOR_VERSION 0
#endif

static
void init_bounce_buffer(shmem_free_list_item_t *item)
{
    shmem_transport_ofi_frag_t *frag =
        (shmem_transport_ofi_frag_t*) item;
    frag->mytype = SHMEM_TRANSPORT_OFI_TYPE_BOUNCE;
}


static inline
int bind_enable_ep_resources(shmem_transport_ctx_t *ctx)
{
    int ret = 0;

    ret = fi_ep_bind(ctx->ep, &shmem_transport_ofi_stx_pool[ctx->stx_idx].stx->fid, 0);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind STX to endpoint failed");

    /* Put counter captures completions for non-fetching operations (put,
     * atomic, etc.) */
    ret = fi_ep_bind(ctx->ep, &ctx->put_cntr->fid, FI_WRITE);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind put CNTR to endpoint failed");

    /* Get counter captures completions for fetching operations (get,
     * fetch-atomic, etc.) */
    ret = fi_ep_bind(ctx->ep, &ctx->get_cntr->fid, FI_READ);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind get CNTR to endpoint failed");

    /* In addition to incrementing the put counter, bounce buffered puts and
     * non-fetching AMOs generate a CQ event that is used to reclaim the buffer
     * (pointer is returned in event context) after the operation completes. */
    ret = fi_ep_bind(ctx->ep, &ctx->cq->fid,
                     FI_SELECTIVE_COMPLETION | FI_TRANSMIT);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind CQ to endpoint failed");

    ret = fi_ep_bind(ctx->ep, &shmem_transport_ofi_avfd->fid, 0);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind AV to endpoint failed");

    ret = fi_enable(ctx->ep);
    OFI_CHECK_RETURN_STR(ret, "fi_enable on endpoint failed");

    return ret;
}


static inline
int allocate_recv_cntr_mr(void)
{
    int ret = 0;
    uint64_t flags = 0;

    /* ------------------------------------ */
    /* POST enable resources for to EP      */
    /* ------------------------------------ */

    /* since this is AFTER enable and RMA you must create memory regions for
     * incoming reads/writes and outgoing non-blocking Puts, specifying entire
     * VA range */

#if ENABLE_TARGET_CNTR
    {
        struct fi_cntr_attr cntr_attr = {0};

        /* Create counter for incoming writes */
        cntr_attr.events   = FI_CNTR_EVENTS_COMP;
        cntr_attr.wait_obj = FI_WAIT_UNSPEC;

        ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_attr,
                           &shmem_transport_ofi_target_cntrfd, NULL);
        OFI_CHECK_RETURN_STR(ret, "target CNTR open failed");

#ifdef ENABLE_MR_RMA_EVENT
        if (shmem_transport_ofi_mr_rma_event)
            flags |= FI_RMA_EVENT;
#endif /* ENABLE_MR_RMA_EVENT */
    }
#endif

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    ret = fi_mr_reg(shmem_transport_ofi_domainfd, 0, UINT64_MAX,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0ULL, flags,
                    &shmem_transport_ofi_target_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (all) registration failed");

    /* Bind counter with target memory region for incoming messages */
#if ENABLE_TARGET_CNTR
    ret = fi_mr_bind(shmem_transport_ofi_target_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to MR failed");

#ifdef ENABLE_MR_RMA_EVENT
    if (shmem_transport_ofi_mr_rma_event) {
        ret = fi_mr_enable(shmem_transport_ofi_target_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target MR enable failed");
    }
#endif /* ENABLE_MR_RMA_EVENT */
#endif /* ENABLE_TARGET_CNTR */

#else
    /* Register separate data and heap segments using keys 0 and 1,
     * respectively.  In MR_BASIC_MODE, the keys are ignored and selected by
     * the provider. */
    ret = fi_mr_reg(shmem_transport_ofi_domainfd, shmem_internal_heap_base,
                    shmem_internal_heap_length,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 1ULL, flags,
                    &shmem_transport_ofi_target_heap_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (heap) registration failed");

    ret = fi_mr_reg(shmem_transport_ofi_domainfd, shmem_internal_data_base,
                    shmem_internal_data_length,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0ULL, flags,
                    &shmem_transport_ofi_target_data_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (data) registration failed");

    /* Bind counter with target memory region for incoming messages */
#if ENABLE_TARGET_CNTR
    ret = fi_mr_bind(shmem_transport_ofi_target_heap_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to heap MR failed");

    ret = fi_mr_bind(shmem_transport_ofi_target_data_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to data MR failed");

#ifdef ENABLE_MR_RMA_EVENT
    if (shmem_transport_ofi_mr_rma_event) {
        ret = fi_mr_enable(shmem_transport_ofi_target_data_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target data MR enable failed");

        ret = fi_mr_enable(shmem_transport_ofi_target_heap_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target heap MR enable failed");
    }
#endif /* ENABLE_MR_RMA_EVENT */
#endif /* ENABLE_TARGET_CNTR */
#endif

    return ret;
}

static
int publish_mr_info(void)
{
#ifndef ENABLE_MR_SCALABLE
    {
        int err;
        uint64_t heap_key, data_key;

        heap_key = fi_mr_key(shmem_transport_ofi_target_heap_mrfd);
        data_key = fi_mr_key(shmem_transport_ofi_target_data_mrfd);

        err = shmem_runtime_put("fi_heap_key", &heap_key, sizeof(uint64_t));
        if (err) {
            RAISE_WARN_STR("Put of heap key to runtime KVS failed");
            return 1;
        }

        err = shmem_runtime_put("fi_data_key", &data_key, sizeof(uint64_t));
        if (err) {
            RAISE_WARN_STR("Put of data segment key to runtime KVS failed");
            return 1;
        }
    }

#ifndef ENABLE_REMOTE_VIRTUAL_ADDRESSING
    {
        int err;
        err = shmem_runtime_put("fi_heap_addr", &shmem_internal_heap_base, sizeof(uint8_t*));
        if (err) {
            RAISE_WARN_STR("Put of heap address to runtime KVS failed");
            return 1;
        }

        err = shmem_runtime_put("fi_data_addr", &shmem_internal_data_base, sizeof(uint8_t*));
        if (err) {
            RAISE_WARN_STR("Put of data segment address to runtime KVS failed");
            return 1;
        }
    }
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* ENABLE_MR_SCALABLE */

    return 0;
}

static
int populate_mr_tables(void)
{
#ifndef ENABLE_MR_SCALABLE
    {
        int i, err;

        shmem_transport_ofi_target_heap_keys = malloc(sizeof(uint64_t) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_heap_keys) {
            RAISE_WARN_STR("Out of memory allocating heap keytable");
            return 1;
        }

        shmem_transport_ofi_target_data_keys = malloc(sizeof(uint64_t) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_data_keys) {
            RAISE_WARN_STR("Out of memory allocating heap keytable");
            return 1;
        }

        /* Called after the upper layer performs the runtime exchange */
        for (i = 0; i < shmem_internal_num_pes; i++) {
            err = shmem_runtime_get(i, "fi_heap_key",
                                    &shmem_transport_ofi_target_heap_keys[i],
                                    sizeof(uint64_t));
            if (err) {
                RAISE_WARN_STR("Get of heap key from runtime KVS failed");
                return 1;
            }
            err = shmem_runtime_get(i, "fi_data_key",
                                    &shmem_transport_ofi_target_data_keys[i],
                                    sizeof(uint64_t));
            if (err) {
                RAISE_WARN_STR("Get of data segment key from runtime KVS failed");
                return 1;
            }
        }
    }

#ifndef ENABLE_REMOTE_VIRTUAL_ADDRESSING
    {
        int i, err;

        shmem_transport_ofi_target_heap_addrs = malloc(sizeof(uint8_t*) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_heap_addrs) {
            RAISE_WARN_STR("Out of memory allocating heap addrtable");
            return 1;
        }

        shmem_transport_ofi_target_data_addrs = malloc(sizeof(uint8_t*) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_data_addrs) {
            RAISE_WARN_STR("Out of memory allocating data addrtable");
            return 1;
        }

        /* Called after the upper layer performs the runtime exchange */
        for (i = 0; i < shmem_internal_num_pes; i++) {
            err = shmem_runtime_get(i, "fi_heap_addr",
                                    &shmem_transport_ofi_target_heap_addrs[i],
                                    sizeof(uint8_t*));
            if (err) {
                RAISE_WARN_STR("Get of heap address from runtime KVS failed");
                return 1;
            }
            err = shmem_runtime_get(i, "fi_data_addr",
                                    &shmem_transport_ofi_target_data_addrs[i],
                                    sizeof(uint8_t*));
            if (err) {
                RAISE_WARN_STR("Get of data segment address from runtime KVS failed");
                return 1;
            }
        }
    }
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* ENABLE_MR_SCALABLE */

    return 0;
}

/* SOFT_SUPPORT will not produce warning or error */
static inline
int atomicvalid_rtncheck(int ret, int atomic_size,
                         atomic_support_lv atomic_sup,
                         char strOP[], char strDT[])
{
    if ((ret != 0 || atomic_size == 0) && atomic_sup != ATOMIC_SOFT_SUPPORT) {
        RAISE_WARN_MSG("Provider does not support atomic '%s' "
                       "on type '%s' (%d, %d)\n", strOP, strDT, ret, atomic_size);

        if (atomic_sup != ATOMIC_WARNINGS) {
            return ret ? ret : -1;
        }
    }

    return 0;
}

static inline
int atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int DT[], int OPS[],
                      atomic_support_lv atomic_sup)
{
    int i, j, ret = 0;
    size_t atomic_size;

    for(i=0; i<DT_MAX; i++) {
        for(j=0; j<OPS_MAX; j++) {
            ret = fi_atomicvalid(shmem_transport_ctx_default.ep, DT[i],
                                 OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[DT[i]]))
                return ret;
        }
    }

    return 0;
}

static inline
int compare_atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int DT[],
                              int OPS[], atomic_support_lv atomic_sup)
{
    int i, j, ret = 0;
    size_t atomic_size;

    for(i=0; i<DT_MAX; i++) {
        for(j=0; j<OPS_MAX; j++) {
            ret = fi_compare_atomicvalid(shmem_transport_ctx_default.ep, DT[i],
                                         OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[DT[i]]))
                return ret;
        }
    }

    return 0;
}

static inline
int fetch_atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int DT[], int OPS[],
                            atomic_support_lv atomic_sup)
{
    int i, j, ret = 0;
    size_t atomic_size;

    for(i=0; i<DT_MAX; i++) {
        for(j=0; j<OPS_MAX; j++) {
            ret = fi_fetch_atomicvalid(shmem_transport_ctx_default.ep, DT[i],
                                       OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[DT[i]]))
                return ret;
        }
    }

    return 0;
}

static inline
int atomic_limitations_check(void)
{
    /* Retrieve messaging limitations from OFI
     *
     * NOTE: Currently only have reduction software atomic support. User can
     * optionally request for warnings if other atomic limitations are detected
     */

    int ret = 0;
    atomic_support_lv general_atomic_sup = ATOMIC_NO_SUPPORT;
    atomic_support_lv reduction_sup = ATOMIC_SOFT_SUPPORT;

    if (shmem_internal_params.OFI_ATOMIC_CHECKS_WARN)
        general_atomic_sup = ATOMIC_WARNINGS;

    init_ofi_tables();

    /* Standard OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_OPS, DT_AMO_STANDARD,
                            AMO_STANDARD_OPS, general_atomic_sup);
    if (ret)
        return ret;

    ret = fetch_atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_FOPS,
                                  DT_AMO_STANDARD, FETCH_AMO_STANDARD_OPS,
                                  general_atomic_sup);
    if (ret)
        return ret;

    ret = compare_atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_COPS,
                                    DT_AMO_STANDARD, COMPARE_AMO_STANDARD_OPS,
                                    general_atomic_sup);
    if (ret)
        return ret;

    /* Extended OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_AMO_EX_DT, SIZEOF_AMO_EX_OPS, DT_AMO_EXTENDED,
                            AMO_EXTENDED_OPS, general_atomic_sup);
    if (ret)
        return ret;

    ret = fetch_atomicvalid_DTxOP(SIZEOF_AMO_EX_DT, SIZEOF_AMO_EX_FOPS,
                                  DT_AMO_EXTENDED, FETCH_AMO_EXTENDED_OPS,
                                  general_atomic_sup);
    if (ret)
        return ret;

    /* Reduction OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_RED_DT, SIZEOF_RED_OPS, DT_REDUCE_BITWISE,
                            REDUCE_BITWISE_OPS, reduction_sup);
    if (ret)
        return ret;

    ret = atomicvalid_DTxOP(SIZEOF_REDC_DT, SIZEOF_REDC_OPS, DT_REDUCE_COMPARE,
                            REDUCE_COMPARE_OPS, reduction_sup);
    if (ret)
        return ret;

    ret = atomicvalid_DTxOP(SIZEOF_REDA_DT, SIZEOF_REDA_OPS, DT_REDUCE_ARITH,
                            REDUCE_ARITH_OPS, reduction_sup);
    if (ret)
        return ret;

    /* Internal atomic requirement */
    ret = compare_atomicvalid_DTxOP(SIZEOF_INTERNAL_REQ_DT, SIZEOF_INTERNAL_REQ_OPS,
                                    DT_INTERNAL_REQ, INTERNAL_REQ_OPS,
                                    general_atomic_sup);
    if (ret)
        return ret;

    return 0;
}

static inline
int publish_av_info(struct fabric_info *info)
{
    int    ret = 0;
    char   epname[128];
    size_t epnamelen = sizeof(epname);

    ret = fi_getname((fid_t)shmem_transport_ofi_target_ep, epname, &epnamelen);
    if (ret != 0 || (epnamelen > sizeof(epname))) {
        RAISE_WARN_STR("fi_getname failed");
        return ret;
    }

    ret = shmem_runtime_put("fi_epname", epname, epnamelen);
    OFI_CHECK_RETURN_STR(ret, "shmem_runtime_put fi_epname failed");

    /* Note: we assume that the length of an address is the same for all
     * endpoints.  This is safe for most HPC systems, but could be incorrect in
     * a heterogeneous context. */
    shmem_transport_ofi_addrlen = epnamelen;

    return ret;
}

static inline
int populate_av(void)
{
    int    i, ret, err = 0;
    char   *alladdrs = NULL;

    alladdrs = malloc(shmem_internal_num_pes * shmem_transport_ofi_addrlen);
    if (alladdrs == NULL) {
        RAISE_WARN_STR("Out of memory allocating 'alladdrs'");
        return 1;
    }

    for (i = 0; i < shmem_internal_num_pes; i++) {
        char *addr_ptr = alladdrs + i * shmem_transport_ofi_addrlen;
        err = shmem_runtime_get(i, "fi_epname", addr_ptr, shmem_transport_ofi_addrlen);
        if (err != 0) {
            RAISE_ERROR_STR("Runtime get of 'fi_epname' failed");
        }
    }

    ret = fi_av_insert(shmem_transport_ofi_avfd,
                       alladdrs,
                       shmem_internal_num_pes,
                       addr_table,
                       0,
                       NULL);
    if (ret != shmem_internal_num_pes) {
        RAISE_WARN_STR("av insert failed");
        return ret;
    }

    free(alladdrs);

    return 0;
}

static inline
int allocate_fabric_resources(struct fabric_info *info)
{
    int ret = 0;
    struct fi_av_attr   av_attr = {0};


    /* fabric domain: define domain of resources physical and logical */
    ret = fi_fabric(info->p_info->fabric_attr, &shmem_transport_ofi_fabfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "fabric initialization failed");

    DEBUG_MSG("OFI version: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"; "
              "provider version: %"PRIu32".%"PRIu32"\n",
              FI_MAJOR_VERSION, FI_MINOR_VERSION,
              FI_MAJOR(fi_version()), FI_MINOR(fi_version()),
              FI_MAJOR(info->p_info->fabric_attr->prov_version),
              FI_MINOR(info->p_info->fabric_attr->prov_version));

    if (FI_MAJOR_VERSION != FI_MAJOR(fi_version()) ||
        FI_MINOR_VERSION != FI_MINOR(fi_version())) {
        RAISE_WARN_MSG("OFI version mismatch: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"\n",
                       FI_MAJOR_VERSION, FI_MINOR_VERSION,
                       FI_MAJOR(fi_version()), FI_MINOR(fi_version()));
    }

    /* access domain: define communication resource limits/boundary within
     * fabric domain */
    ret = fi_domain(shmem_transport_ofi_fabfd, info->p_info,
                    &shmem_transport_ofi_domainfd,NULL);
    OFI_CHECK_RETURN_STR(ret, "domain initialization failed");

    /* AV table set-up for PE mapping */

#ifdef USE_AV_MAP
    av_attr.type = FI_AV_MAP;
    addr_table   = (fi_addr_t*) malloc(info->npes * sizeof(fi_addr_t));
#else
    /* open Address Vector and bind the AV to the domain */
    av_attr.type = FI_AV_TABLE;
    addr_table   = NULL;
#endif

    ret = fi_av_open(shmem_transport_ofi_domainfd,
                     &av_attr,
                     &shmem_transport_ofi_avfd,
                     NULL);
    OFI_CHECK_RETURN_STR(ret, "AV creation failed");

    return ret;
}

static inline
int query_for_fabric(struct fabric_info *info)
{
    int                 ret = 0;
    struct fi_info      hints = {0};
    struct fi_tx_attr   tx_attr = {0};
    struct fi_domain_attr domain_attr = {0};
    struct fi_fabric_attr fabric_attr = {0};
    struct fi_ep_attr   ep_attr = {0};

    shmem_transport_ofi_max_buffered_send = sizeof(long double);

    fabric_attr.prov_name = info->prov_name;

    hints.caps   = FI_RMA |     /* request rma capability
                                   implies FI_READ/WRITE FI_REMOTE_READ/WRITE */
        FI_ATOMICS;  /* request atomics capability */
#if ENABLE_TARGET_CNTR
    hints.caps |= FI_RMA_EVENT; /* want to use remote counters */
#endif /* ENABLE_TARGET_CNTR */
    hints.addr_format         = FI_FORMAT_UNSPEC;
    domain_attr.data_progress = FI_PROGRESS_AUTO;
    domain_attr.resource_mgmt = FI_RM_ENABLED;
#ifdef ENABLE_MR_SCALABLE
    domain_attr.mr_mode       = FI_MR_SCALABLE; /* VA space-doesn't have to be pre-allocated */
#  if !defined(ENABLE_HARD_POLLING) && defined(ENABLE_MR_RMA_EVENT)
    domain_attr.mr_mode       = FI_MR_RMA_EVENT; /* can support RMA_EVENT on MR */
#  endif
#else
    domain_attr.mr_mode       = FI_MR_BASIC; /* VA space is pre-allocated */
#endif
#if !defined(ENABLE_MR_SCALABLE) || !defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    domain_attr.mr_key_size   = 1; /* Heap and data use different MR keys, need
                                      at least 1 byte */
#endif
#ifdef ENABLE_THREADS
    if (shmem_internal_thread_level == SHMEM_THREAD_MULTIPLE) {
#ifdef USE_THREAD_COMPLETION
        domain_attr.threading = FI_THREAD_COMPLETION;
#else
        domain_attr.threading = FI_THREAD_SAFE;
#endif /* USE_THREAD_COMPLETION */
    } else
        domain_attr.threading = FI_THREAD_DOMAIN;
#else
    domain_attr.threading     = FI_THREAD_DOMAIN;
#endif

    hints.domain_attr         = &domain_attr;
    ep_attr.type              = FI_EP_RDM; /* reliable connectionless */
    hints.fabric_attr         = &fabric_attr;
    tx_attr.op_flags          = FI_DELIVERY_COMPLETE;
    tx_attr.inject_size       = shmem_transport_ofi_max_buffered_send; /* require provider to support this as a min */
    hints.tx_attr             = &tx_attr; /* TODO: fill tx_attr */
    hints.rx_attr             = NULL;
    hints.ep_attr             = &ep_attr;

    /* find fabric provider to use that is able to support RMA and ATOMICS */
    ret = fi_getinfo( FI_VERSION(OFI_MAJOR_VERSION, OFI_MINOR_VERSION),
                      NULL, NULL, 0, &hints, &(info->fabrics));

    OFI_CHECK_RETURN_MSG(ret, "OFI transport did not find any valid fabric services "
                              "(provider=%s)\n",
                              info->prov_name != NULL ? info->prov_name : "<auto>");

    /* If the user supplied a fabric or domain name, use it to select the
     * fabric.  Otherwise, select the first fabric in the list. */
    if (info->fabric_name != NULL || info->domain_name != NULL) {
        struct fi_info *cur_fabric;

        info->p_info = NULL;

        for (cur_fabric = info->fabrics; cur_fabric; cur_fabric = cur_fabric->next) {
            if (info->fabric_name == NULL ||
                fnmatch(info->fabric_name, cur_fabric->fabric_attr->name, 0) == 0) {
                if (info->domain_name == NULL ||
                    fnmatch(info->domain_name, cur_fabric->domain_attr->name, 0) == 0) {
                    info->p_info = cur_fabric;
                    break;
                }
            }
        }
    }
    else {
        info->p_info = info->fabrics;
    }

    if (NULL == info->p_info) {
        RAISE_WARN_MSG("OFI transport, no valid fabric (prov=%s, fabric=%s, domain=%s)\n",
                       info->prov_name != NULL ? info->prov_name : "<auto>",
                       info->fabric_name != NULL ? info->fabric_name : "<auto>",
                       info->domain_name != NULL ? info->domain_name : "<auto>");
        return ret;
    }

    if (info->p_info->ep_attr->max_msg_size > 0) {
        shmem_transport_ofi_max_msg_size = info->p_info->ep_attr->max_msg_size;
    } else {
        RAISE_WARN_STR("OFI provider did not set max_msg_size");
        return 1;
    }

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    /* Only use a single MR, no keys required */
    info->p_info->domain_attr->mr_key_size = 0;
#else
    /* Heap and data use different MR keys, need at least 1 byte */
    info->p_info->domain_attr->mr_key_size = 1;
#endif

    shmem_internal_assertp(info->p_info->tx_attr->inject_size >= shmem_transport_ofi_max_buffered_send);
    shmem_transport_ofi_max_buffered_send = info->p_info->tx_attr->inject_size;
#ifdef ENABLE_MR_RMA_EVENT
    shmem_transport_ofi_mr_rma_event = (info->p_info->domain_attr->mr_mode & FI_MR_RMA_EVENT) != 0;
#endif

    DEBUG_MSG("OFI provider: %s, fabric: %s, domain: %s\n"
              RAISE_PE_PREFIX "max_inject: %zd, max_msg: %zd\n",
              info->p_info->fabric_attr->prov_name,
              info->p_info->fabric_attr->name, info->p_info->domain_attr->name,
              shmem_internal_my_pe,
              shmem_transport_ofi_max_buffered_send,
              shmem_transport_ofi_max_msg_size);

    return ret;
}

static int shmem_transport_ofi_target_ep_init(void)
{
    int ret = 0;

    struct fabric_info* info = &shmem_transport_ofi_info;
    info->p_info->ep_attr->tx_ctx_cnt = 0;
    info->p_info->caps = FI_RMA | FI_ATOMICS | FI_REMOTE_READ | FI_REMOTE_WRITE;
#if ENABLE_TARGET_CNTR
    info->p_info->caps |= FI_RMA_EVENT;
#endif
    info->p_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    info->p_info->mode = 0;
    info->p_info->tx_attr->mode = 0;
    info->p_info->rx_attr->mode = 0;

    ret = fi_endpoint(shmem_transport_ofi_domainfd,
                      info->p_info, &shmem_transport_ofi_target_ep, NULL);
    OFI_CHECK_RETURN_MSG(ret, "target endpoint creation failed (%s)\n", fi_strerror(errno));

    /* Attach the address vector */
    ret = fi_ep_bind(shmem_transport_ofi_target_ep, &shmem_transport_ofi_avfd->fid, 0);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind AV to target endpoint failed");

    ret = allocate_recv_cntr_mr();
    if (ret != 0) return ret;

    ret = fi_enable(shmem_transport_ofi_target_ep);
    OFI_CHECK_RETURN_STR(ret, "fi_enable on target endpoint failed");

    return 0;
}

static int shmem_transport_ofi_ctx_init(shmem_transport_ctx_t *ctx, int id)
{
    int ret = 0;

    struct fi_cntr_attr cntr_put_attr = {0};
    struct fi_cntr_attr cntr_get_attr = {0};
    cntr_put_attr.events   = FI_CNTR_EVENTS_COMP;
    cntr_get_attr.events   = FI_CNTR_EVENTS_COMP;

    /* Set FI_WAIT based on the put and get polling limits defined above */
    if (shmem_transport_ofi_put_poll_limit < 0) {
        cntr_put_attr.wait_obj = FI_WAIT_NONE;
    } else {
        cntr_put_attr.wait_obj = FI_WAIT_UNSPEC;
    }
    if (shmem_transport_ofi_get_poll_limit < 0) {
        cntr_get_attr.wait_obj = FI_WAIT_NONE;
    } else {
        cntr_get_attr.wait_obj = FI_WAIT_UNSPEC;
    }

    /* Allow provider to choose CQ size, since we are using FI_RM_ENABLED.
     * Context format is used to return bounce buffer pointers in the event
     * so that they can be inserted back into to the free list. */
    struct fi_cq_attr cq_attr = {0};
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;

    struct fabric_info* info = &shmem_transport_ofi_info;
    info->p_info->ep_attr->tx_ctx_cnt = FI_SHARED_CONTEXT;
    info->p_info->caps = FI_RMA | FI_WRITE | FI_READ | FI_ATOMICS;
    info->p_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    info->p_info->mode = 0;
    info->p_info->tx_attr->mode = 0;
    info->p_info->rx_attr->mode = 0;

    ctx->id = id;
#ifdef USE_CTX_LOCK
    SHMEM_MUTEX_INIT(ctx->lock);
#endif

    ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_put_attr,
                       &ctx->put_cntr, NULL);
    OFI_CHECK_RETURN_MSG(ret, "put_cntr creation failed (%s)\n", fi_strerror(errno));

    ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_get_attr,
                       &ctx->get_cntr, NULL);
    OFI_CHECK_RETURN_MSG(ret, "get_cntr creation failed (%s)\n", fi_strerror(errno));

    ret = fi_cq_open(shmem_transport_ofi_domainfd, &cq_attr, &ctx->cq, NULL);
    if (ret && errno == FI_EMFILE) {
        DEBUG_STR("Context creation failed because of open files limit, consider increasing with 'ulimit' command");
    }
    OFI_CHECK_RETURN_MSG(ret, "cq_open failed (%s)\n", fi_strerror(errno));

    ret = fi_endpoint(shmem_transport_ofi_domainfd,
                      info->p_info, &ctx->ep, NULL);
    OFI_CHECK_RETURN_MSG(ret, "ep creation failed (%s)\n", fi_strerror(errno));

    /* TODO: Fill in TX attr */

    /* Allocate STX from the pool */
    if (shmem_internal_thread_level > SHMEM_THREAD_FUNNELED &&
        shmem_transport_ofi_is_private(ctx->options)) {
            ctx->tid = shmem_transport_ofi_gettid();
    }
    shmem_transport_ofi_stx_allocate(ctx);

    ret = bind_enable_ep_resources(ctx);
    OFI_CHECK_RETURN_MSG(ret, "context bind/enable endpoint failed (%s)\n", fi_strerror(errno));

    if (ctx->options & SHMEMX_CTX_BOUNCE_BUFFER &&
        shmem_transport_ofi_bounce_buffer_size > 0 &&
        shmem_transport_ofi_max_bounce_buffers > 0)
    {
        ctx->bounce_buffers =
            shmem_free_list_init(sizeof(shmem_transport_ofi_bounce_buffer_t) +
                                 shmem_transport_ofi_bounce_buffer_size,
                                 init_bounce_buffer);
    }
    else {
        ctx->options &= ~SHMEMX_CTX_BOUNCE_BUFFER;
        ctx->bounce_buffers = NULL;
    }

    return 0;
}


int shmem_transport_init(void)
{
    int ret = 0;

    SHMEM_MUTEX_INIT(shmem_transport_ofi_lock);

    shmem_transport_ofi_info.npes = shmem_runtime_get_size();

    if (shmem_internal_params.OFI_PROVIDER_provided)
        shmem_transport_ofi_info.prov_name = shmem_internal_params.OFI_PROVIDER;
    else if (shmem_internal_params.OFI_USE_PROVIDER_provided)
        shmem_transport_ofi_info.prov_name = shmem_internal_params.OFI_USE_PROVIDER;
    else
        shmem_transport_ofi_info.prov_name = NULL;

    if (shmem_internal_params.OFI_FABRIC_provided)
        shmem_transport_ofi_info.fabric_name = shmem_internal_params.OFI_FABRIC;
    else
        shmem_transport_ofi_info.fabric_name = NULL;

    if (shmem_internal_params.OFI_DOMAIN_provided)
        shmem_transport_ofi_info.domain_name = shmem_internal_params.OFI_DOMAIN;
    else
        shmem_transport_ofi_info.domain_name = NULL;


    ret = query_for_fabric(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    ret = allocate_fabric_resources(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    /* STX max settings */
    if ((shmem_internal_thread_level == SHMEM_THREAD_SINGLE ||
         shmem_internal_thread_level == SHMEM_THREAD_FUNNELED ) &&
         shmem_internal_params.OFI_STX_MAX > 1) {
        if (shmem_internal_params.OFI_STX_MAX_provided) {
            /* We need only 1 STX per PE with SHMEM_THREAD_SINGLE or SHMEM_THREAD_FUNNELED */
            RAISE_WARN_MSG("Ignoring invalid STX max setting '%ld'; using 1 STX in single-threaded mode\n",
                           shmem_internal_params.OFI_STX_MAX);
        }
        shmem_transport_ofi_stx_max = 1;
    } else {
        shmem_transport_ofi_stx_max = shmem_internal_params.OFI_STX_MAX;
    }
    shmem_transport_ofi_stx_threshold = shmem_internal_params.OFI_STX_THRESHOLD;

    /* STX sharing settings */
    char *type = shmem_internal_params.OFI_STX_ALLOCATOR;
    if (0 == strcmp(type, "round-robin")) {
        shmem_transport_ofi_stx_allocator = ROUNDROBIN;
    } else if (0 == strcmp(type, "random")) {
        shmem_transport_ofi_stx_allocator = RANDOM;
        shmem_transport_ofi_stx_rand_init();
    } else {
        RAISE_WARN_MSG("Ignoring bad STX share algorithm '%s', using 'round-robin'\n", type);
        shmem_transport_ofi_stx_allocator = ROUNDROBIN;
    }


    /* The current bounce buffering implementation is only compatible with
     * providers that don't require FI_CONTEXT */
    if (shmem_transport_ofi_info.p_info->mode & FI_CONTEXT) {
        if (shmem_internal_my_pe == 0 && shmem_internal_params.BOUNCE_SIZE > 0) {
            DEBUG_STR("OFI provider requires FI_CONTEXT; disabling bounce buffering");
        }
        shmem_transport_ofi_bounce_buffer_size = 0;
        shmem_transport_ofi_max_bounce_buffers = 0;
    } else {
        shmem_transport_ofi_bounce_buffer_size = shmem_internal_params.BOUNCE_SIZE;
        shmem_transport_ofi_max_bounce_buffers = shmem_internal_params.MAX_BOUNCE_BUFFERS;
    }

    shmem_transport_ofi_put_poll_limit = shmem_internal_params.OFI_TX_POLL_LIMIT;
    shmem_transport_ofi_get_poll_limit = shmem_internal_params.OFI_RX_POLL_LIMIT;

#ifdef USE_CTX_LOCK
    /* In multithreaded mode, force completion polling so that threads yield
     * the lock during put/get completion operations.  User can still override
     * (get blocking behavior) by setting the env vars. */
    if (shmem_internal_thread_level == SHMEM_THREAD_MULTIPLE) {
        if (!shmem_internal_params.OFI_TX_POLL_LIMIT_provided)
            shmem_transport_ofi_put_poll_limit = -1;
        if (!shmem_internal_params.OFI_RX_POLL_LIMIT_provided)
            shmem_transport_ofi_get_poll_limit = -1;
    }
#endif

    shmem_transport_ctx_default.options = SHMEMX_CTX_BOUNCE_BUFFER;

    ret = shmem_transport_ofi_target_ep_init();
    if (ret != 0) return ret;

    ret = publish_mr_info();
    if (ret != 0) return ret;

    ret = publish_av_info(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    return 0;
}

int shmem_transport_startup(void)
{
    int ret;
    int i;
    int num_on_node;
    long ofi_tx_ctx_cnt;

    if (shmem_internal_params.OFI_STX_AUTO) {

        if (shmem_internal_params.OFI_STX_NODE_MAX_provided) {
            if (shmem_internal_params.OFI_STX_NODE_MAX_provided > 0) {
                ofi_tx_ctx_cnt = shmem_internal_params.OFI_STX_NODE_MAX;
            } else {
                RAISE_ERROR_STR("OFI_STX_NODE_MAX must be greater than zero");
            }
        } else {
            ofi_tx_ctx_cnt = shmem_transport_ofi_info.fabrics->domain_attr->tx_ctx_cnt;
        }

        num_on_node = shmem_runtime_get_local_size();

        /* Paritition TX resources evenly across node-local PEs */
        shmem_transport_ofi_stx_max = ofi_tx_ctx_cnt / num_on_node;
        int remainder = ofi_tx_ctx_cnt % num_on_node;
        int node_pe = shmem_internal_my_pe % shmem_internal_num_pes;
        if (remainder > 0 && ((node_pe % num_on_node) < remainder)) {
            shmem_transport_ofi_stx_max++;
        }

        /* When running more PEs than available STXs, must assign each PE at least 1 */
        if (shmem_transport_ofi_stx_max <= 0) {
            shmem_transport_ofi_stx_max = 1;
            RAISE_WARN_MSG("Need at least 1 STX per PE, but detected %ld available STXs for %d PEs\n",
                           ofi_tx_ctx_cnt, num_on_node);
        }

        DEBUG_MSG("PE %d auto-set STX max to %ld\n", shmem_internal_my_pe, shmem_transport_ofi_stx_max);
    }

    /* Allocate STX array with max length */
    shmem_transport_ofi_stx_pool = malloc(shmem_transport_ofi_stx_max *
                                          sizeof(shmem_transport_ofi_stx_t));
    if (shmem_transport_ofi_stx_pool == NULL) {
        RAISE_ERROR_STR("Out of memory when allocating OFI STX pool");
    }

    for (i = 0; i < shmem_transport_ofi_stx_max; i++) {
        ret = fi_stx_context(shmem_transport_ofi_domainfd, NULL,
                             &shmem_transport_ofi_stx_pool[i].stx, NULL);
        OFI_CHECK_RETURN_MSG(ret, "STX context creation failed (%s)\n", fi_strerror(ret));
        shmem_transport_ofi_stx_pool[i].ref_cnt = 0;
        shmem_transport_ofi_stx_pool[i].is_private = 0;
    }

    ret = shmem_transport_ofi_ctx_init(&shmem_transport_ctx_default, SHMEM_TRANSPORT_CTX_DEFAULT_ID);
    if (ret != 0) return ret;

    ret = atomic_limitations_check();
    if (ret != 0) return ret;

    ret = populate_mr_tables();
    if (ret != 0) return ret;

    ret = populate_av();
    if (ret != 0) return ret;

    return 0;
}

int shmem_transport_ctx_create(long options, shmem_transport_ctx_t **ctx)
{
    SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);

    int ret;
    size_t id;

    /* Look for an open slot in the contexts array */
    for (id = 0; id < shmem_transport_ofi_contexts_len; id++)
        if (shmem_transport_ofi_contexts[id] == NULL) break;

    /* If none found, grow the array */
    if (id >= shmem_transport_ofi_contexts_len) {
        id = shmem_transport_ofi_contexts_len;

        size_t i = shmem_transport_ofi_contexts_len;
        shmem_transport_ofi_contexts_len += shmem_transport_ofi_grow_size;
        shmem_transport_ofi_contexts = realloc(shmem_transport_ofi_contexts,
               shmem_transport_ofi_contexts_len * sizeof(shmem_transport_ctx_t*));

        for ( ; i < shmem_transport_ofi_contexts_len; i++)
            shmem_transport_ofi_contexts[i] = NULL;

        if (shmem_transport_ofi_contexts == NULL) {
            RAISE_ERROR_STR("Out of memory when allocating OFI ctx array");
        }
    }

    shmem_transport_ctx_t *ctxp = malloc(sizeof(shmem_transport_ctx_t));

    if (ctxp == NULL) {
        RAISE_ERROR_STR("Out of memory when allocating OFI ctx object");
    }

    memset(ctxp, 0, sizeof(shmem_transport_ctx_t));

#ifndef USE_CTX_LOCK
    shmem_internal_atomic_write(&ctxp->pending_put_cntr, 0);
    shmem_internal_atomic_write(&ctxp->pending_get_cntr, 0);
#endif

    ctxp->stx_idx = -1;
    ctxp->options = options;

    ret = shmem_transport_ofi_ctx_init(ctxp, id);

    if (ret) {
        shmem_transport_ctx_destroy(ctxp);
    } else {
        shmem_transport_ofi_contexts[id] = ctxp;
        *ctx = ctxp;
    }

    SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
    return ret;

}

void shmem_transport_ctx_destroy(shmem_transport_ctx_t *ctx)
{
    int ret;

    if(shmem_internal_params.DEBUG) {
        SHMEM_TRANSPORT_OFI_CTX_LOCK(ctx);
        if (ctx->bounce_buffers) SHMEM_TRANSPORT_OFI_CTX_BB_LOCK(ctx);
        DEBUG_MSG("id = %d, options = %#0lx, stx_idx = %d\n"
                  RAISE_PE_PREFIX "pending_put_cntr = %9"PRIu64", completed_put_cntr = %9"PRIu64"\n"
                  RAISE_PE_PREFIX "pending_get_cntr = %9"PRIu64", completed_get_cntr = %9"PRIu64"\n"
                  RAISE_PE_PREFIX "pending_bb_cntr  = %9"PRIu64", completed_bb_cntr  = %9"PRIu64"\n",
                  ctx->id, (unsigned long) ctx->options, ctx->stx_idx,
                  shmem_internal_my_pe,
                  SHMEM_TRANSPORT_OFI_CNTR_READ(&ctx->pending_put_cntr), fi_cntr_read(ctx->put_cntr),
                  shmem_internal_my_pe,
                  SHMEM_TRANSPORT_OFI_CNTR_READ(&ctx->pending_get_cntr), fi_cntr_read(ctx->get_cntr),
                  shmem_internal_my_pe,
                  ctx->pending_bb_cntr, ctx->completed_bb_cntr
                 );
        if (ctx->bounce_buffers) SHMEM_TRANSPORT_OFI_CTX_BB_UNLOCK(ctx);
        SHMEM_TRANSPORT_OFI_CTX_UNLOCK(ctx);
    }

    if (ctx->ep) {
        ret = fi_close(&ctx->ep->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context endpoint close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->bounce_buffers) {
        shmem_free_list_destroy(ctx->bounce_buffers);
    }

    if (ctx->stx_idx >= 0) {
        SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);
        if (shmem_transport_ofi_is_private(ctx->options)) {
            shmem_transport_ofi_stx_kvs_t *e;
            HASH_FIND(hh, shmem_transport_ofi_stx_kvs, &ctx->tid,
                      sizeof(struct shmem_internal_tid), e);
            if (e) {
                shmem_transport_ofi_stx_t *stx = &shmem_transport_ofi_stx_pool[ctx->stx_idx];
                stx->ref_cnt--;
                if (stx->ref_cnt == 0) {
                    HASH_DEL(shmem_transport_ofi_stx_kvs, e);
                    free(e);
                    shmem_transport_ofi_stx_pool[ctx->stx_idx].is_private = 0;
                }
            }
            else {
                RAISE_WARN_STR("Unable to locate private STX");
            }
        } else {
            shmem_transport_ofi_stx_pool[ctx->stx_idx].ref_cnt--;
            if (shmem_transport_ofi_stx_pool[ctx->stx_idx].is_private) {
                SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
                RAISE_ERROR_STR("Destroyed a ctx with an inconsistent is_private field");
            }
        }
        SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
    }

    if (ctx->put_cntr) {
        ret = fi_close(&ctx->put_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context put CNTR close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->get_cntr) {
        ret = fi_close(&ctx->get_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context get CNTR close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->cq) {
        ret = fi_close(&ctx->cq->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context CQ close failed (%s)\n", fi_strerror(errno));
    }

#ifdef USE_CTX_LOCK
    SHMEM_MUTEX_DESTROY(ctx->lock);
#endif

    if (ctx->id >= 0) {
        SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);
        shmem_transport_ofi_contexts[ctx->id] = NULL;
        SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
        free(ctx);
    }
    else if (ctx->id != SHMEM_TRANSPORT_CTX_DEFAULT_ID) {
        RAISE_ERROR_MSG("Attempted to destroy an invalid context (%d)\n", ctx->id);
    }
}

int shmem_transport_fini(void)
{
    int ret;
    shmem_transport_ofi_stx_kvs_t* e;
    int stx_len = 0;

    /* Free all shareable contexts.  This performs a quiet on each context,
     * ensuring all operations have completed before proceeding with shutdown. */

    for (size_t i = 0; i < shmem_transport_ofi_contexts_len; ++i) {
        if (shmem_transport_ofi_contexts[i]) {
            if (shmem_transport_ofi_is_private(shmem_transport_ofi_contexts[i]->options))
                RAISE_WARN_MSG("Shutting down with unfreed private context (%zd)\n", i);
            shmem_transport_quiet(shmem_transport_ofi_contexts[i]);
            shmem_transport_ctx_destroy(shmem_transport_ofi_contexts[i]);
        }
    }

    if (shmem_transport_ofi_contexts) free(shmem_transport_ofi_contexts);

    shmem_transport_quiet(&shmem_transport_ctx_default);
    shmem_transport_ctx_destroy(&shmem_transport_ctx_default);

    for (e = shmem_transport_ofi_stx_kvs; e != NULL; ) {
        shmem_transport_ofi_stx_kvs_t *last = e;
        stx_len++;
        e = e->hh.next;
        free(last);
    }

    if (stx_len > 0) {
        RAISE_WARN_MSG("Key/value store contained %d unfreed private contexts\n", stx_len);
    }

    for (long i = 0; i < shmem_transport_ofi_stx_max; ++i) {
        if (shmem_transport_ofi_stx_pool[i].ref_cnt != 0)
            RAISE_WARN_MSG("Closing a %s STX (%zu) with nonzero ref. count (%ld)\n",
                           shmem_transport_ofi_stx_pool[i].is_private ? "private" : "shared",
                           i, shmem_transport_ofi_stx_pool[i].ref_cnt);
        ret = fi_close(&shmem_transport_ofi_stx_pool[i].stx->fid);
        OFI_CHECK_ERROR_MSG(ret, "STX context close failed (%s)\n", fi_strerror(errno));
    }
    free(shmem_transport_ofi_stx_pool);

    ret = fi_close(&shmem_transport_ofi_target_ep->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target endpoint close failed (%s)\n", fi_strerror(errno));

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    ret = fi_close(&shmem_transport_ofi_target_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target MR close failed (%s)\n", fi_strerror(errno));
#else
    ret = fi_close(&shmem_transport_ofi_target_heap_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target heap MR close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_target_data_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target data MR close failed (%s)\n", fi_strerror(errno));
#endif

#if ENABLE_TARGET_CNTR
    ret = fi_close(&shmem_transport_ofi_target_cntrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target CT close failed (%s)\n", fi_strerror(errno));
#endif

    ret = fi_close(&shmem_transport_ofi_avfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "AV close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_domainfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Domain close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_fabfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Fabric close failed (%s)\n", fi_strerror(errno));

#ifdef USE_AV_MAP
    free(addr_table);
#endif

    fi_freeinfo(shmem_transport_ofi_info.fabrics);

    SHMEM_MUTEX_DESTROY(shmem_transport_ofi_lock);

    return 0;
}

/* -*- C -*-
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S.  Government
 * retains certain rights in this software.
 * 
 * This file is part of the Portals SHMEM software package. For license
 * information, see the LICENSE file in the top level directory of the
 * distribution.
 *
 */

#include "config.h"

#include <portals4.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/param.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "mpp/shmem.h"
#include "shmem_internal.h"
#include "runtime.h"

ptl_handle_ni_t shmem_internal_ni_h = PTL_INVALID_HANDLE;
ptl_pt_index_t shmem_internal_data_pt = PTL_PT_ANY;
ptl_pt_index_t shmem_internal_heap_pt = PTL_PT_ANY;
ptl_handle_md_t shmem_internal_put_md_h = PTL_INVALID_HANDLE;
ptl_handle_md_t shmem_internal_get_md_h = PTL_INVALID_HANDLE;
ptl_handle_le_t shmem_internal_data_le_h = PTL_INVALID_HANDLE;
ptl_handle_le_t shmem_internal_heap_le_h = PTL_INVALID_HANDLE;
ptl_handle_ct_t shmem_internal_target_ct_h = PTL_INVALID_HANDLE;
ptl_handle_ct_t shmem_internal_put_ct_h = PTL_INVALID_HANDLE;
ptl_handle_ct_t shmem_internal_get_ct_h = PTL_INVALID_HANDLE;
#ifdef ENABLE_EVENT_COMPLETION
ptl_handle_eq_t shmem_internal_put_eq_h = PTL_INVALID_HANDLE;
#endif
ptl_handle_eq_t shmem_internal_err_eq_h = PTL_INVALID_HANDLE;
ptl_size_t shmem_internal_max_put_size = 0;
ptl_size_t shmem_internal_max_atomic_size = 0;
ptl_size_t shmem_internal_max_fetch_atomic_size = 0;
ptl_size_t shmem_internal_pending_put_counter = 0;
ptl_size_t shmem_internal_pending_get_counter = 0;
ptl_ni_limits_t ni_limits;

static void
cleanup_handles(void)
{
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_get_md_h, PTL_INVALID_HANDLE)) {
        PtlMDRelease(shmem_internal_get_md_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_put_md_h, PTL_INVALID_HANDLE)) {
        PtlMDRelease(shmem_internal_put_md_h);
    }
#ifdef ENABLE_EVENT_COMPLETION
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_put_eq_h, PTL_INVALID_HANDLE)) {
        PtlEQFree(shmem_internal_put_eq_h);
    }
#endif
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_get_ct_h, PTL_INVALID_HANDLE)) {
        PtlCTFree(shmem_internal_get_ct_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_put_ct_h, PTL_INVALID_HANDLE)) {
        PtlCTFree(shmem_internal_put_ct_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_heap_le_h, PTL_INVALID_HANDLE)) {
        PtlLEUnlink(shmem_internal_heap_le_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_data_le_h, PTL_INVALID_HANDLE)) {
        PtlLEUnlink(shmem_internal_data_le_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_target_ct_h, PTL_INVALID_HANDLE)) {
        PtlCTFree(shmem_internal_target_ct_h);
    }
    if (PTL_PT_ANY != shmem_internal_heap_pt) {
        PtlPTFree(shmem_internal_ni_h, shmem_internal_heap_pt);
    }
    if (PTL_PT_ANY != shmem_internal_data_pt) {
        PtlPTFree(shmem_internal_ni_h, shmem_internal_data_pt);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_err_eq_h, PTL_INVALID_HANDLE)) {
        PtlEQFree(shmem_internal_err_eq_h);
    }
    if (PTL_OK != PtlHandleIsEqual(shmem_internal_ni_h, PTL_INVALID_HANDLE)) {
        PtlNIFini(shmem_internal_ni_h);
    }
}


int
shmem_transport_portals4_init(void)
{
    ptl_process_t my_id;
    int ret;
    ptl_ni_limits_t ni_req_limits;

    /* Initialize Portals */
    ret = PtlInit();
    if (PTL_OK != ret) {
        fprintf(stderr, "ERROR: PtlInit failed: %d\n", ret);
        abort();
    }

    /* Initialize network */
    ni_req_limits.max_entries = 1024;
    ni_req_limits.max_unexpected_headers = 1024;
    ni_req_limits.max_mds = 1024;
    ni_req_limits.max_eqs = 1024;
    ni_req_limits.max_cts = 1024;
    ni_req_limits.max_pt_index = 64;
    ni_req_limits.max_iovecs = 1024;
    ni_req_limits.max_list_size = 1024;
    ni_req_limits.max_triggered_ops = 1024;
    ni_req_limits.max_msg_size = LONG_MAX;
    ni_req_limits.max_atomic_size = LONG_MAX;
    ni_req_limits.max_fetch_atomic_size = LONG_MAX;
    ni_req_limits.max_waw_ordered_size = LONG_MAX;
    ni_req_limits.max_war_ordered_size = LONG_MAX;
    ni_req_limits.max_volatile_size = 512; /* BWB: FIX ME, SEE PORTALS ISSUE 2 */
    ni_req_limits.features = PTL_TOTAL_DATA_ORDERING;

    ret = PtlNIInit(PTL_IFACE_DEFAULT,
                    PTL_NI_NO_MATCHING | PTL_NI_LOGICAL,
                    PTL_PID_ANY,
                    &ni_req_limits,
                    &ni_limits,
                    &shmem_internal_ni_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlNIInit failed: %d\n",
                shmem_internal_my_pe, ret);
        return ret;
    }

    ret = PtlGetPhysId(shmem_internal_ni_h, &my_id);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlGetPhysId failed: %d\n",
                shmem_internal_my_pe, ret);
        return ret;
    }

    /* Share information */
    ret = shmem_runtime_put("portals4-procid", &my_id, sizeof(my_id));
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: runtime_put failed: %d\n",
                shmem_internal_my_pe, ret);
        return ret;
    }

    return 0;
}


int
shmem_transport_portals4_startup(void *data_start, size_t data_len,
                                 void *heap_start, size_t heap_len)
{
    int ret, i;
    ptl_process_t *desired = NULL;
    ptl_md_t md;
    ptl_le_t le;
    ptl_uid_t uid = PTL_UID_ANY;
    long waw_size;

    desired = malloc(sizeof(ptl_process_t) * shmem_internal_num_pes);
    if (NULL == desired) goto cleanup;

    for (i = 0 ; i < shmem_internal_num_pes; ++i) {
        ret = shmem_runtime_get(i, "portals4-procid", &desired[i], sizeof(ptl_process_t));
        if (0 != ret) {
            fprintf(stderr, "[%03d] ERROR: runtime_get failed: %d\n",
                    shmem_internal_my_pe, ret);
            goto cleanup;
        }
    }

    ret = PtlSetMap(shmem_internal_ni_h,
                    shmem_internal_num_pes,                    
                    desired);
    if (PTL_OK != ret && PTL_IGNORED != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlSetMap failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    ret = PtlGetUid(shmem_internal_ni_h, &uid);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlGetUid failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* Check message size limits to make sure rational */
    if ((PTL_TOTAL_DATA_ORDERING & ni_limits.features) != 0) {
        shmem_internal_total_data_ordering = 1;        
        waw_size = ni_limits.max_waw_ordered_size;
    } else {
        /* waw ordering doesn't matter for message size if no total
           ordering.  Therefore, make it big, so it's not the limiter
           in the following tests. */
        waw_size = LONG_MAX;
    }
    shmem_internal_max_put_size = MIN(waw_size, ni_limits.max_volatile_size);
    shmem_internal_max_atomic_size = MIN(MIN(waw_size, ni_limits.max_volatile_size),
                                         ni_limits.max_atomic_size);
    shmem_internal_max_fetch_atomic_size = MIN(waw_size, ni_limits.max_atomic_size);
    if (shmem_internal_max_put_size < sizeof(long double complex)) {
        fprintf(stderr, "[%03d] ERROR: Max put size found to be %lu, too small to continue\n",
                shmem_internal_my_pe, (unsigned long) shmem_internal_max_put_size);
        goto cleanup;
    }
    if (shmem_internal_max_atomic_size < sizeof(long double complex)) {
        fprintf(stderr, "[%03d] ERROR: Max atomic size found to be %lu, too small to continue\n",
                shmem_internal_my_pe, (unsigned long) shmem_internal_max_put_size);
        goto cleanup;
    }
    if (shmem_internal_max_fetch_atomic_size < sizeof(long double complex)) {
        fprintf(stderr, "[%03d] ERROR: Max fetch atomic size found to be %lu, too small to continue\n",
                shmem_internal_my_pe, (unsigned long) shmem_internal_max_put_size);
        goto cleanup;
    }

    /* create portal table entry */
    ret = PtlEQAlloc(shmem_internal_ni_h, 64, &shmem_internal_err_eq_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlEQAlloc failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }
    ret = PtlPTAlloc(shmem_internal_ni_h,
                     0,
                     shmem_internal_err_eq_h,
                     DATA_IDX,
                     &shmem_internal_data_pt);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlPTAlloc of data table failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }
    ret = PtlPTAlloc(shmem_internal_ni_h,
                     0,
                     shmem_internal_err_eq_h,
                     HEAP_IDX,
                     &shmem_internal_heap_pt);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlPTAlloc of heap table failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* target ct */
    ret = PtlCTAlloc(shmem_internal_ni_h, &shmem_internal_target_ct_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlCTAlloc of target ct failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* Open LE to heap section */
    le.start = heap_start;
    le.length = heap_len;
    le.ct_handle = shmem_internal_target_ct_h;
    le.uid = uid;
    le.options = PTL_LE_OP_PUT | PTL_LE_OP_GET | 
        PTL_LE_EVENT_LINK_DISABLE |
        PTL_LE_EVENT_SUCCESS_DISABLE | 
        PTL_LE_EVENT_CT_COMM;
    ret = PtlLEAppend(shmem_internal_ni_h,
                      shmem_internal_heap_pt,
                      &le,
                      PTL_PRIORITY_LIST,
                      NULL,
                      &shmem_internal_heap_le_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlLEAppend of heap section failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* Open LE to data section */
    le.start = data_start;
    le.length = data_len;
    le.ct_handle = shmem_internal_target_ct_h;
    le.uid = uid;
    le.options = PTL_LE_OP_PUT | PTL_LE_OP_GET | 
        PTL_LE_EVENT_LINK_DISABLE |
        PTL_LE_EVENT_SUCCESS_DISABLE | 
        PTL_LE_EVENT_CT_COMM;
    ret = PtlLEAppend(shmem_internal_ni_h,
                      shmem_internal_data_pt,
                      &le,
                      PTL_PRIORITY_LIST,
                      NULL,
                      &shmem_internal_data_le_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlLEAppend of data section failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* Open MD to all memory */
    ret = PtlCTAlloc(shmem_internal_ni_h, &shmem_internal_put_ct_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlCTAlloc of put ct failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }
    ret = PtlCTAlloc(shmem_internal_ni_h, &shmem_internal_get_ct_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlCTAlloc of get ct failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }
#ifdef ENABLE_EVENT_COMPLETION
    ret = PtlEQAlloc(shmem_internal_ni_h, 64, &shmem_internal_put_eq_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlEQAlloc of put eq failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }
#endif

    md.start = 0;
    md.length = SIZE_MAX;
    md.options = PTL_MD_EVENT_CT_ACK;
#if ! defined(ENABLE_EVENT_COMPLETION)
    md.options |= PTL_MD_EVENT_SUCCESS_DISABLE;
    if ((PTL_TOTAL_DATA_ORDERING & ni_limits.features) != 0) {
        md.options |= PTL_MD_VOLATILE;
    }
#endif

#ifdef ENABLE_EVENT_COMPLETION
    md.eq_handle = shmem_internal_put_eq_h;
#else
    md.eq_handle = shmem_internal_err_eq_h;
#endif
    md.ct_handle = shmem_internal_put_ct_h;
    ret = PtlMDBind(shmem_internal_ni_h,
                    &md,
                    &shmem_internal_put_md_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlMDBind of put MD failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    md.start = 0;
    md.length = SIZE_MAX;
    md.options = PTL_MD_EVENT_CT_REPLY | 
        PTL_MD_EVENT_SUCCESS_DISABLE;
    md.eq_handle = shmem_internal_err_eq_h;
    md.ct_handle = shmem_internal_get_ct_h;
    ret = PtlMDBind(shmem_internal_ni_h,
                    &md,
                    &shmem_internal_get_md_h);
    if (PTL_OK != ret) {
        fprintf(stderr, "[%03d] ERROR: PtlMDBind of get MD failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    return 0;

 cleanup:
    if (desired)
        free(desired);
    cleanup_handles();
    if (NULL != shmem_internal_data_base) {
        shmem_internal_symmetric_fini();
    }
    shmem_internal_runtime_fini();
    PtlFini();
    abort();

}


int
shmem_transport_portals4_fini(void)
{
    ptl_ct_event_t ct;

    /* wait for remote completion (acks) of all pending events */
    PtlCTWait(shmem_internal_put_ct_h, 
              shmem_internal_pending_put_counter, &ct);

    cleanup_handles();
    PtlFini();

    return 0;
}


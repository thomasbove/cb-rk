/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sel4bench/sel4bench.h>
#include <utils/util.h>

#include "../../event_counters.h"

const char*
sel4bench_arch_get_counter_description(counter_t counter)
{
    ZF_LOGE("Not implemented");
    return NULL;
}

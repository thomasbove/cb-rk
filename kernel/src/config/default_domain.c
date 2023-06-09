/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <util.h>
#include <object/structures.h>
#include <model/statedata.h>

/* Default schedule. */
const dschedule_t ksDomSchedule[] = {
    { .domain = 0, .length = 1,
#ifdef CONFIG_DOMAIN_IRQ_PARTITIONING
      .irqs = {irqInvalid}
#endif
    },
};

const word_t ksDomScheduleLength = ARRAY_SIZE(ksDomSchedule);


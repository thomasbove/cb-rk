/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/ioctl.h>
#include <linux/fs.h> /* everything... */
#include <linux/errno.h> /* error codes */
#include <linux/proc_fs.h>
#include <linux/fcntl.h> /* O_ACCMODE */
#include <asm/uaccess.h> /* copy_from/to_user */
#include <asm-generic/ioctl.h>
#include <linux/wait.h>
#include <linux/sched.h> /*Helps fix TASK_UNINTERRUPTIBLE */
#include <linux/pid.h>
#include <linux/fdtable.h>
#include <linux/rcupdate.h>
#include <linux/eventfd.h>
#include <linux/kthread.h>  // for threads
#include <linux/time.h>   // for using jiffies
#include <linux/timer.h>

#include "vmm_manager.h"
#include "vchan_copy.h"

int reg_event_irq_handler(void);
void free_event_irq_handler(void);

int call_into_hypervisor(int cmd, void *data, size_t sz, vmcall_args_t *vmcall);

int init_event_thread(void);
int event_thread_info(int domain, int port, int type, int *closed);

int new_event_instance(int domain, int port, int eventfd, vchan_alert_t *event_mon, int self);
void rem_event_instance(int domain, int port);

int wait_for_event(int domain, int port, int type, size_t request_size);

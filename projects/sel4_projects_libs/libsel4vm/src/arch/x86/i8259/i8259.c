/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Intel 8259 Programmable Interrupt Controller (PIC) emulator on x86.
 *
 * The functions related to machine state manipulations were taken
 * from Linux kernel 3.8.8 arch/x86/kvm/i8259.c
 *
 */

#include <autoconf.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <sel4/sel4.h>
#include <stdio.h>
#include <utils/util.h>

#include <sel4vm/guest_vm.h>
#include <sel4vm/boot.h>
#include <sel4vm/guest_irq_controller.h>
#include <sel4vm/arch/ioports.h>
#include "i8259.h"

#define I8259_MASTER   0
#define I8259_SLAVE    1

#define PIC_NUM_PINS 16

/*first programmable interrupt controller, master*/
#define X86_IO_PIC_1_START   0x20
#define X86_IO_PIC_1_END     0x21

/*second programmable interrupt controller, slave*/
#define X86_IO_PIC_2_START    0xa0
#define X86_IO_PIC_2_END      0xa1

/*ELCR (edge/level control register) for IRQ line*/
#define X86_IO_ELCR_START      0x4d0
#define X86_IO_ELCR_END        0x4d1

typedef struct i8259_irq_ack {
    irq_ack_fn_t callback;
    void *cookie;
} i8259_irq_ack_t;

static i8259_irq_ack_t irq_ack_fns[PIC_NUM_PINS];

/* PIC Machine state. */
struct i8259_state {
    unsigned char last_irr;        /* Edge detection */
    unsigned char irr;             /* Interrupt request register */
    unsigned char imr;             /* Interrupt mask register */
    unsigned char isr;             /* Interrupt service register */
    unsigned char priority_add;    /* Highest irq priority */
    unsigned char irq_base;
    unsigned char read_reg_select;
    unsigned char poll;
    unsigned char special_mask;
    unsigned char init_state;
    unsigned char auto_eoi;
    unsigned char rotate_on_auto_eoi;
    unsigned char special_fully_nested_mode;
    unsigned char init4;           /* True if 4 byte init */
    unsigned char elcr;            /* PIIX edge/trigger selection */
    unsigned char elcr_mask;
    unsigned char isr_ack;         /* Interrupt ack detection */
    struct i8259 *pics_state;
};

/* Struct containig PIC state for a Guest OS instance. */
struct i8259 {
    unsigned int wakeup_needed;
    unsigned int pending_acks;
    struct i8259_state pics[2];  /* 0 is master pic, 1 is slave pic */
    int output;                    /* Intr from master PIC */
    int emitagain;
};

static inline int select_pic(unsigned int irq)
{
    assert(irq < 16);
    if (irq < 8) {
        return I8259_MASTER;
    } else {
        return I8259_SLAVE;
    }
}

static inline int __vm_irq_line_state(unsigned long *irq_state,
                                      int irq_source_id, int level)
{
    /* Logical OR for level trig interrupt. */
    if (level) {
        (*irq_state) |= BIT(irq_source_id);
    } else {
        (*irq_state) &= ~BIT(irq_source_id);
    }

    return !!(*irq_state);
}

/* Return the highest priority found in mask (highest = smallest number). Return 8 if no irq */
static inline int get_priority(struct i8259_state *s, int mask)
{
    int priority = 0;

    if (!mask) {
        return 8;
    }

    while (!(mask & (1 << ((priority + s->priority_add) & 7)))) {
        priority++;
    }
    return priority;
}

/* Check if given IO address is valid. */
static int i8259_in_range(unsigned int addr)
{
    switch (addr) {
    case 0x20:
    case 0x21:
    case 0xa0:
    case 0xa1:
    case 0x4d0:
    case 0x4d1:
        return 1;
    default:
        return 0;
    }
}


/* Compare ISR with the highest priority IRQ in IRR.
 *    Returns -1 if no interrupts,
 *    Otherwise returns the PIC interrupt generated.
 */
static int pic_get_irq(struct i8259_state *s)
{
    int mask, cur_priority, priority;

    mask = s->irr & ~s->imr;
    priority = get_priority(s, mask);
    if (priority == 8) {
        return -1;
    }
    /* Compute current priority. If special fully nested mode on the
     * master, the IRQ coming from the slave is not taken into account
     * for the priority computation.
     */
    mask = s->isr;
    if (s->special_fully_nested_mode && s == &s->pics_state->pics[0]) {
        mask &= ~(1 << 2);
    }
    cur_priority = get_priority(s, mask);
    if (priority < cur_priority) {
        /* Higher priority found: an irq should be generated. */
        return (priority + s->priority_add) & 7;
    } else {
        return -1;
    }
}

/* Clear the IRQ from ISR, the IRQ has been served. */
static void pic_clear_isr(vm_t *vm, struct i8259_state *s, int irq)
{
    /* Clear the ISR, notify the ack handler. */
    s->isr &= ~(1 << irq);
    if (s != &s->pics_state->pics[0]) {
        irq += 8;
    }

    if (irq != 2) {
        if (irq_ack_fns[irq].callback) {
            irq_ack_fns[irq].callback(vm->vcpus[BOOT_VCPU], irq, irq_ack_fns[irq].cookie);
        }
    }
}

/* Set irq level. If an edge is detected, then the IRR is set to 1. */
static inline int pic_set_irq1(struct i8259_state *s, int irq, int level)
{
    int mask, ret = 1;
    mask = 1 << irq;

    if (s->elcr & mask) {
        /* Level triggered. */
        if (level) {
            ret = !(s->irr & mask);
            s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->irr &= ~mask;
            s->last_irr &= ~mask;
        }
    } else {
        /* Edge triggered. */
        if (level) {
            if ((s->last_irr & mask) == 0) {
                ret = !(s->irr & mask);
                s->irr |= mask;
            }
            s->last_irr |= mask;
        } else {
            s->last_irr &= ~mask;
        }
    }

    return (s->imr & mask) ? -1 : ret;
}


/* Raise IRQ on CPU if necessary. Must be called every time the active IRQ may change.
   Update the master pic and trigger interrupt injection according to the IRR and ISR. */
static void pic_update_irq(struct i8259 *s)
{
    int irq2, irq;

    irq2 = pic_get_irq(&s->pics[1]);
    if (irq2 >= 0) {
        /* If IRQ request by slave PIC, signal master PIC and set the IRR in master PIC. */
        pic_set_irq1(&s->pics[0], 2, 1);
        pic_set_irq1(&s->pics[0], 2, 0);
    }
    irq = pic_get_irq(&s->pics[0]);

    /* PIC status changed injection flag. */
    if (!s->output) {
        s->wakeup_needed = true;
    }

    if (irq >= 0) {
        s->output = 1;
    } else {
        s->output = 0;
    }

    if (s->emitagain && s->output) {
//        haveint_emit();
        s->emitagain = 0;
    }
}

/* Reset the PIC state for a guest OS. */
static void pic_reset(vm_t *vm, struct i8259_state *s)
{
    int irq;
    unsigned char edge_irr = s->irr & ~s->elcr;

    s->last_irr = 0;
    s->irr &= s->elcr;
    s->imr = 0;
    s->priority_add = 0;
    s->special_mask = 0;
    s->read_reg_select = 0;
    if (!s->init4) {
        s->special_fully_nested_mode = 0;
        s->auto_eoi = 0;
    }
    s->init_state = 1;

#if 0
    /* FIXME: CONNECT pic with APIC */
    kvm_for_each_vcpu(i, vcpu, s->piics_state->kvm)
    if (kvm_apic_accept_pic_intr(vcpu)) {
        found = true;
        break;
    }


    if (!found) {
        return;
    }
#endif

    for (irq = 0; irq < PIC_NUM_PINS / 2; irq++) {
        if (edge_irr & (1 << irq)) {
            pic_clear_isr(vm, s, irq);
        }
    }
}

/* Write into the state owned by the guest OS. */
static void pic_ioport_write(vm_vcpu_t *vcpu, struct i8259_state *s, unsigned int addr, unsigned int val)
{
    int priority, cmd, irq;

    addr &= 1;

    if (addr == 0) {
        if (val & 0x10) {
            /* ICW1 */
            s->init4 = val & 1;
            if (val & 0x02) {
                printf("PIC: single mode not supported\n");
            }
            if (val & 0x08) {
                printf("PIC: level sensitive irq not supported\n");
            }
            /* Reset the machine state and pending IRQS. */
            pic_reset(vcpu->vm, s);

        } else if (val & 0x08) {
            /* OCW 3 */
            if (val & 0x04) {
                s->poll = 1;
            }
            if (val & 0x02) {
                s->read_reg_select = val & 1;
            }
            if (val & 0x40) {
                s->special_mask = (val >> 5) & 1;
            }
        } else {
            /* OCW 2 */
            cmd = val >> 5;
            switch (cmd) {
            case 0:
            case 4:
                s->rotate_on_auto_eoi = cmd >> 2;
                break;
            case 1:
            /* End of interrupt. */
            case 5:
                /* Clear ISR and update IRQ*/
                priority = get_priority(s, s->isr);
                if (priority != 8) {
                    irq = (priority + s->priority_add) & 7;
                    if (cmd == 5) {
                        s->priority_add = (irq + 1) & 7;
                    }
                    pic_clear_isr(vcpu->vm, s, irq);
                    pic_update_irq(s->pics_state);
                }
                break;
            case 3:
                /* Specific EOI command. */
                irq = val & 7;
                pic_clear_isr(vcpu->vm, s, irq);
                pic_update_irq(s->pics_state);
                break;
            case 6:
                /* Set priority command. */
                s->priority_add = (val + 1) & 7;
                pic_update_irq(s->pics_state);
                break;
            case 7:
                /* Rotate on specific eoi command. */
                irq = val & 7;
                s->priority_add = (irq + 1) & 7;
                pic_clear_isr(vcpu->vm, s, irq);
                pic_update_irq(s->pics_state);
                break;
            default:
                /* No operation. */
                break;
            }
        }
    } else
        switch (s->init_state) {
        case 0: { /* Normal mode OCW 1. */
            unsigned char imr_diff = s->imr ^ val;
            (void) imr_diff;
            //off = (s == &s->pics_state->pics[0]) ? 0 : 8;
            s->imr = val;
#if 0
            for (irq = 0; irq < PIC_NUM_PINS / 2; irq++)
                if (imr_diff & (1 << irq))
                    /*FIXME: notify the status changes for IMR*/
                    kvm_fire_mask_notifiers(
                        s->pics_state->kvm,
                        select_pic(irq + off),
                        irq + off,
                        !!(s->imr & (1 << irq)));
#endif
            pic_update_irq(s->pics_state);
            break;
        }
        case 1:
            /* ICW 2 */
            s->irq_base = val & 0xf8;
            s->init_state = 2;
            break;
        case 2:
            if (s->init4) {
                s->init_state = 3;
            } else {
                s->init_state = 0;
            }
            break;
        case 3:
            /* ICW 4 */
            s->special_fully_nested_mode = (val >> 4) & 1;
            s->auto_eoi = (val >> 1) & 1;
            s->init_state = 0;
            break;
        }
}

/* Poll the pending IRQS for the highest priority IRQ, ack the IRQ: clear the ISR and IRR, and
 * update PIC state. Returns -1 if no pending IRQ. */
static unsigned int pic_poll_read(vm_t *vm, struct i8259_state *s, unsigned int addr1)
{
    unsigned int ret;

    ret = pic_get_irq(s);

    if (ret >= 0) {
        if (addr1 >> 7) {
            s->pics_state->pics[0].isr &= ~(1 << 2);
            s->pics_state->pics[0].irr &= ~(1 << 2);
        }
        s->irr &= ~(1 << ret);
        pic_clear_isr(vm, s, ret);
        if (addr1 >> 7 || ret != 2) {
            pic_update_irq(s->pics_state);
        }
    } else {
        ret = 0x07;
        pic_update_irq(s->pics_state);
    }

    return ret;
}


/* Read and write functions for PIC (master and slave). */
static unsigned int pic_ioport_read(vm_vcpu_t *vcpu, struct i8259_state *s, unsigned int addr)
{
    unsigned int ret;

    /* Poll for the highest priority IRQ. */
    if (s->poll) {
        ret = pic_poll_read(vcpu->vm, s, addr);
        s->poll = 0;

    } else {
        if (!(addr & 1)) {
            if (s->read_reg_select) {
                ret = s->isr;
            } else {
                ret = s->irr;
            }
        } else {
            ret = s->imr;
        }

    }
    return ret;
}

/*read and write functions for ELCR  (edge/level control registers)
IO: 0x4d0 0x4d1 each bit corresponsing to an IRQ from 8259
bit set: level triggered mode
bit clear: edge triggered mode*/
static void elcr_ioport_write(struct i8259_state *s, unsigned int addr, unsigned int val)
{
    s->elcr = val & s->elcr_mask;
}

static unsigned int elcr_ioport_read(struct i8259_state *s, unsigned int addr)
{
    return s->elcr;
}


ioport_fault_result_t i8259_port_out(vm_vcpu_t *vcpu, void *cookie, unsigned int port_no, unsigned int size,
                                     unsigned int value)
{
    /* Sender thread is the VMM main thread, calculate guest ID according to the badge. */
    struct i8259 *s = vcpu->vm->arch.i8259_gs;

    if (!i8259_in_range(port_no)) {
        return IO_FAULT_ERROR;
    }
    if (size != 1) {
        return IO_FAULT_ERROR;
    }

    /* 0x20, 0x21, master pic, 0xa0, 0xa1 slave PIC. */
    switch (port_no) {
    case 0x20:
    case 0x21:
    case 0xa0:
    case 0xa1:
        pic_ioport_write(vcpu, &s->pics[port_no >> 7], port_no, value);
        break;
    case 0x4d0:
    case 0x4d1:
        elcr_ioport_write(&s->pics[port_no & 1], port_no, value);
        break;
    }

    return IO_FAULT_HANDLED;

}

ioport_fault_result_t i8259_port_in(vm_vcpu_t *vcpu, void *cookie, unsigned int port_no, unsigned int size,
                                    unsigned int *result)
{
    /* Sender thread is the VMM main thread, calculate guest ID according to the badge. */
    struct i8259 *s = vcpu->vm->arch.i8259_gs;

    if (!i8259_in_range(port_no)) {
        return IO_FAULT_ERROR;
    }
    if (size != 1) {
        return IO_FAULT_ERROR;
    }

    /* 0x20, 0x21, master pic, 0xa0, 0xa1 slave PIC. */
    switch (port_no) {
    case 0x20:
    case 0x21:
    case 0xa0:
    case 0xa1:
        *result = pic_ioport_read(vcpu, &s->pics[port_no >> 7], port_no);
        break;
    case 0x4d0:
    case 0x4d1:
        *result = elcr_ioport_read(&s->pics[port_no & 1], port_no);
        break;
    }
    return IO_FAULT_HANDLED;
}

/* Init internal status for PIC driver. */
static void i8259_init_state(struct i8259 *s)
{
    /* Init pic machine state for guest OS. */
//        s->pics[0].elcr = seL4_IA32_IOPort_In8(LIB_VMM_IO_PCI_CAP, 0x4d0).result;
//        s->pics[1].elcr = seL4_IA32_IOPort_In8(LIB_VMM_IO_PCI_CAP, 0x4d1).result;
    s->pics[0].elcr = 0;
    s->pics[1].elcr = 0;
    s->pics[0].elcr_mask = 0xf8;
    s->pics[1].elcr_mask = 0xde;
    s->pics[0].pics_state = s;
    s->pics[1].pics_state = s;
}


/* Acknowledge interrupt IRQ. */
static inline void pic_intack(vm_t *vm, struct i8259_state *s, int irq)
{
    /* Ack the IRQ, set the ISR. */
    s->isr |= 1 << irq;

    /* We don't clear a level sensitive interrupt here. */
    if (!(s->elcr & (1 << irq))) {
        s->irr &= ~(1 << irq);
    }

    /* Clear the ISR for auto EOI mode. */
    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi) {
            s->priority_add = (irq + 1) & 7;
        }
        pic_clear_isr(vm, s, irq);
    }
}

/* Use output as a flag for pending IRQ. */
static int i8259_has_irq(vm_t *vm)
{
    struct i8259 *s = vm->arch.i8259_gs;
    return s->output;
}

#if 0
static int i8259_poll_irq()
{
    struct i8259 *s = &i8259_gs;

    int irq, irq2, intno;

    /* Search for the highest priority IRQ. */
    irq = pic_get_irq(&s->pics[0]);

    if (irq >= 0) {
        if (irq == 2) {
            irq2 = pic_get_irq(&s->pics[1]);
            if (irq2 >= 0) {
            } else {
                /* Spurious IRQ on slave controller. */
                irq2 = 7;
            }
            intno = s->pics[1].irq_base + irq2;
            irq = irq2 + 8;
        } else {
            intno = s->pics[0].irq_base + irq;
        }
    } else {
        /* Spurious IRQ on host 8259 controller. */
        irq = 7;
        intno = s->pics[0].irq_base + irq;
    }

    return intno;
}
#endif

/* Return the highest pending IRQ. Ack the IRQ by updating the ISR before entering guest, using this
 * function to get the pending IRQ. */
static int i8259_read_irq(vm_t *vm)
{
    struct i8259 *s = vm->arch.i8259_gs;

    int irq, irq2, intno;

    /* Search for the highest priority IRQ. */
    irq = pic_get_irq(&s->pics[0]);

    if (irq >= 0) {
        /* Ack the IRQ. */
        pic_intack(vm, &s->pics[0], irq);

        /* Ack the slave 8259 controller. */
        if (irq == 2) {
            irq2 = pic_get_irq(&s->pics[1]);
            if (irq2 >= 0) {
                pic_intack(vm, &s->pics[1], irq2);
            } else
                /* Spurious IRQ on slave controller. */
            {
                irq2 = 7;
            }
            intno = s->pics[1].irq_base + irq2;
            irq = irq2 + 8;
        } else {
            intno = s->pics[0].irq_base + irq;
        }
    } else {
        /* Spurious IRQ on host 8259 controller. */
        irq = 7;
        intno = s->pics[0].irq_base + irq;
    }
    pic_update_irq(s);

    return intno;
}

int i8259_get_interrupt(vm_t *vm)
{
    int ret;
    if (i8259_has_irq(vm)) {
        ret = i8259_read_irq(vm);
    } else {
        ret = -1;
    }
    if (!i8259_has_irq(vm)) {
        vm->arch.i8259_gs->emitagain = 1;
    }
    return ret;
}

int i8259_has_interrupt(vm_t *vm)
{
    int ret = i8259_has_irq(vm);
    return ret;
}

vm_ioport_entry_t pic_ioports[] = {
    {{X86_IO_PIC_1_START, X86_IO_PIC_1_END}, {NULL, i8259_port_in, i8259_port_out, "8259 Programmable Interrupt Controller (1st, Master)"}},
    {{X86_IO_PIC_2_START, X86_IO_PIC_2_END}, {NULL, i8259_port_in, i8259_port_out, "8259 Programmable Interrupt Controller (2nd, Slave)"}},
    {{X86_IO_ELCR_START, X86_IO_ELCR_END}, {NULL, i8259_port_in, i8259_port_out, "ELCR (edge/level control register) for IRQ line"}}
};

int i8259_pre_init(vm_t *vm)
{
    int err;
    /* First initialize the emulated pic state */
    vm->arch.i8259_gs = calloc(1, sizeof(struct i8259));
    if (!vm->arch.i8259_gs) {
        return -1;
    }
    i8259_init_state(vm->arch.i8259_gs);
    vm->arch.i8259_gs->emitagain = 1;
    for (int i = 0; i < ARRAY_SIZE(pic_ioports); i++) {
        vm_ioport_range_t pic_range = pic_ioports[i].range;
        vm_ioport_interface_t pic_interface = pic_ioports[i].interface;
        err = vm_io_port_add_handler(vm, pic_range, pic_interface);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* This is the actual function that will get called for all interrupt events
 * Furthermore this implements the guest irq controller interface */

/* To inject an IRQ: First set the level as 1, then set the level as 0, toggling the level for
 * triggering the IRQ.
 * IRQ source ID is used for mapping multiple IRQ source into a IRQ pin.
 * Sets irq request into the state machine for PIC.
 */
int vm_set_irq_level(vm_vcpu_t *vcpu, int irq, int irq_level)
{
    int ret;

    struct i8259 *s = vcpu->vm->arch.i8259_gs;

    /* Set IRR. */
    ret = pic_set_irq1(&s->pics[irq >> 3], irq & 7, irq_level);
    pic_update_irq(s);

    if (ret == -1) {
        return -1;
    }
    return 0;
}

int vm_inject_irq(vm_vcpu_t *vcpu, int irq)
{
    vm_set_irq_level(vcpu, irq, 1);
    vm_set_irq_level(vcpu, irq, 0);
    return 0;
}

int vm_register_irq(vm_vcpu_t *vcpu, int irq, irq_ack_fn_t fn, void *cookie)
{
    if (irq < 0 || irq >= PIC_NUM_PINS) {
        ZF_LOGE("irq %d is invalid", irq);
        return -1;
    }
    i8259_irq_ack_t *ack = &irq_ack_fns[irq];
    ack->callback = fn;
    ack->cookie = cookie;
    return 0;
}

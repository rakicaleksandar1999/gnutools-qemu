/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2012  MIPS Technologies, Inc.  All rights reserved.
 * Authors: Sanjay Lal <sanjayl@kymasys.com>
 *
 * Copyright (C) 2015 Imagination Technologies
 */

#include <string.h>
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/bitmap.h"
#include "exec/memory.h"
#include "sysemu/sysemu.h"
#include "qom/cpu.h"
#include "exec/address-spaces.h"

#ifdef CONFIG_KVM
#include "sysemu/kvm.h"
#include "kvm_mips.h"
#endif

#include "hw/mips/mips_gic.h"
#include "hw/mips/mips_gcmpregs.h"

/* #define DEBUG */

#ifdef DEBUG
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...)
#endif

#define TIMER_PERIOD 10 /* 10 ns period for 100 Mhz frequency */

static inline int gic_get_current_cpu(MIPSGICState *g)
{
    if (g->num_cpu > 1) {
        return current_cpu->cpu_index;
    }
    return 0;
}

/* GIC VPE Local Timer */
static uint32_t gic_vpe_timer_update(MIPSGICState *gic, uint32_t vp_index)
{
    uint64_t now, next;
    uint32_t wait;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    wait = gic->gic_vpe_comparelo[vp_index] - gic->gic_sh_counterlo -
            (uint32_t)(now / TIMER_PERIOD);
    next = now + (uint64_t)wait * TIMER_PERIOD;

    qemu_log("GIC timer scheduled, now = %llx, next = %llx (wait = %u)\n", now, next, wait);

    timer_mod(gic->gic_timer[vp_index].timer, next);
    return wait;
}

static void gic_vpe_timer_expire(MIPSGICState *gic, uint32_t vp_index)
{
    uint32_t pin;
    pin = (gic->gic_vpe_compare_map[vp_index] & 0x3F) + 2;
    qemu_log("GIC timer expire => VPE[%d] irq %d\n", vp_index, pin);
    gic_vpe_timer_update(gic, vp_index);
    gic->gic_vpe_pend[vp_index] |= (1 << 1);

    if (gic->gic_vpe_pend[vp_index] &
            (gic->gic_vpe_mask[vp_index] & GIC_VPE_SMASK_CMP_MSK)) {
        if (gic->gic_vpe_compare_map[vp_index] & 0x80000000) {
            gic->timer_irq[vp_index] = 1;
            qemu_irq_raise(gic->env[vp_index]->irq[pin]);
        } else {
            qemu_log("    disabled!\n");
        }
    } else {
        qemu_log("    masked off!\n");
    }
}

static uint32_t gic_get_sh_count(MIPSGICState *gic)
{
    int i;
    if (gic->gic_gl_config & (1 << 28)) {
        return gic->gic_sh_counterlo;
    } else {
        uint64_t now;
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        for (i = 0; i < gic->num_cpu; i++) {
            if (timer_pending(gic->gic_timer[i].timer)
                && timer_expired(gic->gic_timer[i].timer, now)) {
                /* The timer has already expired.  */
                gic_vpe_timer_expire(gic, i);
            }
        }
        return gic->gic_sh_counterlo + (uint32_t)(now / TIMER_PERIOD);
    }
}

static void gic_store_sh_count(MIPSGICState *gic, uint64_t count)
{
    int i;
    DPRINTF("QEMU: gic_store_count %lx\n", count);

    if ((gic->gic_gl_config & 0x10000000) || !gic->gic_timer) {
        gic->gic_sh_counterlo = count;
    } else {
        /* Store new count register */
        gic->gic_sh_counterlo = count -
            (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / TIMER_PERIOD);
        /* Update timer timer */
        for (i = 0; i < gic->num_cpu; i++) {
            gic_vpe_timer_update(gic, i);
        }
    }
}

static void gic_store_vpe_compare(MIPSGICState *gic, uint32_t vp_index,
                                  uint64_t compare)
{
    uint32_t wait;
    gic->gic_vpe_comparelo[vp_index] = (uint32_t) compare;
    wait = gic_vpe_timer_update(gic, vp_index);

    DPRINTF("GIC Compare modified (GIC_VPE%d_Compare=0x%x GIC_Counter=0x%x) "
            "- schedule CMP timer interrupt after 0x%x\n",
            vp_index,
            gic->gic_vpe_comparelo[vp_index], gic->gic_sh_counterlo,
            wait);

    gic->gic_vpe_pend[vp_index] &= ~(1 << 1);
    if (gic->gic_vpe_compare_map[vp_index] & 0x80000000) {
        uint32_t irq_num = (gic->gic_vpe_compare_map[vp_index] & 0x3F) + 2;
        gic->timer_irq[vp_index] = 0;
        if (!gic->ic_irq[vp_index]) {
            qemu_set_irq(gic->env[vp_index]->irq[irq_num], 0);
        }
    }
}

static void gic_vpe_timer_cb(void *opaque)
{
    MIPSGICTimerState *gic_timer = opaque;
    gic_timer->gic->gic_sh_counterlo++;
    gic_vpe_timer_expire(gic_timer->gic, gic_timer->vp_index);
    gic_timer->gic->gic_sh_counterlo--;
}

static void gic_timer_start_count(MIPSGICState *gic)
{
    DPRINTF("QEMU: GIC timer starts count\n");
    gic_store_sh_count(gic, gic->gic_sh_counterlo);
}

static void gic_timer_stop_count(MIPSGICState *gic)
{
    int i;

    DPRINTF("QEMU: GIC timer stops count\n");
    /* Store the current value */
    gic->gic_sh_counterlo +=
        (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / TIMER_PERIOD);
    for (i = 0; i < gic->num_cpu; i++) {
        timer_del(gic->gic_timer[i].timer);
    }
}

static void gic_timer_init(MIPSGICState *gic, uint32_t ncpus)
{
    int i;
    gic->gic_timer = (void *) g_malloc0(sizeof(MIPSGICTimerState) * ncpus);
    for (i = 0; i < ncpus; i++) {
        gic->gic_timer[i].gic = gic;
        gic->gic_timer[i].vp_index = i;
        gic->gic_timer[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               &gic_vpe_timer_cb,
                                               &gic->gic_timer[i]);
    }
    gic_store_sh_count(gic, gic->gic_sh_counterlo);
}

/* GIC Read VPE Local/Other Registers */
static uint64_t gic_read_vpe(MIPSGICState *gic, uint32_t vp_index, hwaddr addr,
                             unsigned size)
{
    switch (addr) {
    case GIC_VPE_CTL_OFS:
        DPRINTF("(GIC_VPE_CTL) -> 0x%016x\n", gic->gic_vpe_ctl[vp_index]);
        return gic->gic_vpe_ctl[vp_index];
    case GIC_VPE_PEND_OFS:
        gic_get_sh_count(gic);
        DPRINTF("(GIC_VPE_PEND) -> 0x%016x\n", gic->gic_vpe_pend[vp_index]);
        return gic->gic_vpe_pend[vp_index];
    case GIC_VPE_MASK_OFS:
        DPRINTF("(GIC_VPE_MASK) -> 0x%016x\n", gic->gic_vpe_mask[vp_index]);
        return gic->gic_vpe_mask[vp_index];
    case GIC_VPE_WD_MAP_OFS:
        return gic->gic_vpe_wd_map[vp_index];
    case GIC_VPE_COMPARE_MAP_OFS:
        return gic->gic_vpe_compare_map[vp_index];
    case GIC_VPE_TIMER_MAP_OFS:
        return gic->gic_vpe_timer_map[vp_index];
    case GIC_VPE_OTHER_ADDR_OFS:
        DPRINTF("(GIC_VPE_OTHER_ADDR) -> 0x%016x\n",
                gic->gic_vpe_other_addr[vp_index]);
        return gic->gic_vpe_other_addr[vp_index];
    case GIC_VPE_IDENT_OFS:
        return vp_index;
    case GIC_VPE_COMPARE_LO_OFS:
        DPRINTF("(GIC_VPE_COMPARELO) -> 0x%016x\n",
                gic->gic_vpe_comparelo[vp_index]);
        return gic->gic_vpe_comparelo[vp_index];
    case GIC_VPE_COMPARE_HI_OFS:
        DPRINTF("(GIC_VPE_COMPAREhi) -> 0x%016x\n",
                gic->gic_vpe_comparehi[vp_index]);
        return gic->gic_vpe_comparehi[vp_index];
    default:
        DPRINTF("Warning *** read %d bytes at GIC offset LOCAL/OTHER 0x%"
                PRIx64 "\n",
                size, addr);
        break;
    }
    return 0;
}

static uint64_t gic_read(void *opaque, hwaddr addr, unsigned size)
{
    int reg;
    MIPSGICState *gic = (MIPSGICState *) opaque;
    uint32_t vp_index = gic_get_current_cpu(gic);
    uint64_t ret = 0;
    int i, base;

    DPRINTF("Info read %d bytes at GIC offset 0x%" PRIx64,
            size, addr);

    switch (addr) {
    case GIC_SH_CONFIG_OFS:
        DPRINTF("(GIC_SH_CONFIG) -> 0x%016x\n", gic->gic_gl_config);
        return gic->gic_gl_config;
    case GIC_SH_CONFIG_OFS + 4:
        /* do nothing */
        return 0;
    case GIC_SH_COUNTERLO_OFS:
    {
        ret = gic_get_sh_count(gic);
        qemu_log("(GIC_SH_COUNTERLO) -> 0x%016x\n", ret);
        return ret;
    }
    case GIC_SH_COUNTERHI_OFS:
        DPRINTF("(Not supported GIC_SH_COUNTERHI) -> 0x%016x\n", 0);
        return 0;
    case GIC_SH_POL_31_0_OFS:
    case GIC_SH_POL_63_32_OFS:
    case GIC_SH_POL_95_64_OFS:
    case GIC_SH_POL_127_96_OFS:
    case GIC_SH_POL_159_128_OFS:
    case GIC_SH_POL_191_160_OFS:
    case GIC_SH_POL_223_192_OFS:
    case GIC_SH_POL_255_224_OFS:
        base = (addr - GIC_SH_POL_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->gic_irqs[i].polarity & 1) << i;
        }
        DPRINTF("(GIC_SH_POL) -> 0x%016x\n", ret);
        return ret;
    case GIC_SH_TRIG_31_0_OFS:
    case GIC_SH_TRIG_63_32_OFS:
    case GIC_SH_TRIG_95_64_OFS:
    case GIC_SH_TRIG_127_96_OFS:
    case GIC_SH_TRIG_159_128_OFS:
    case GIC_SH_TRIG_191_160_OFS:
    case GIC_SH_TRIG_223_192_OFS:
    case GIC_SH_TRIG_255_224_OFS:
        base = (addr - GIC_SH_TRIG_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->gic_irqs[i].trigger_type & 1) << i;
        }
        DPRINTF("(GIC_SH_TRIG) -> 0x%016x\n", ret);
        return ret;
    case GIC_SH_PEND_31_0_OFS:
    case GIC_SH_PEND_63_32_OFS:
    case GIC_SH_PEND_95_64_OFS:
    case GIC_SH_PEND_127_96_OFS:
    case GIC_SH_PEND_159_128_OFS:
    case GIC_SH_PEND_191_160_OFS:
    case GIC_SH_PEND_223_192_OFS:
    case GIC_SH_PEND_255_224_OFS:
        base = (addr - GIC_SH_PEND_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->gic_irqs[i].pending & 1) << i;
        }
        DPRINTF("(GIC_SH_PEND) -> 0x%016x\n", ret);
        return ret;
    case GIC_SH_MASK_31_0_OFS:
    case GIC_SH_MASK_63_32_OFS:
    case GIC_SH_MASK_95_64_OFS:
    case GIC_SH_MASK_127_96_OFS:
    case GIC_SH_MASK_159_128_OFS:
    case GIC_SH_MASK_191_160_OFS:
    case GIC_SH_MASK_223_192_OFS:
    case GIC_SH_MASK_255_224_OFS:
        base = (addr - GIC_SH_MASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->gic_irqs[i].enabled & 1) << i;
        }
        DPRINTF("(GIC_SH_MASK) -> 0x%016x\n", ret);
        return ret;
    default:
        if (addr < GIC_SH_INTR_MAP_TO_PIN_BASE_OFS) {
            DPRINTF("Warning *** read %d bytes at GIC offset 0x%" PRIx64 "\n",
                    size, addr);
        }
        break;
    }

    /* Global Interrupt Map SrcX to Pin register */
    if (addr >= GIC_SH_INTR_MAP_TO_PIN_BASE_OFS
        && addr <= GIC_SH_MAP_TO_PIN(255)) {
        reg = (addr - GIC_SH_INTR_MAP_TO_PIN_BASE_OFS) / 4;
        ret = gic->gic_irqs[reg].map_pin;
        DPRINTF("(GIC) -> 0x%016x\n", ret);
        return ret;
    }

    /* Global Interrupt Map SrcX to VPE register */
    if (addr >= GIC_SH_INTR_MAP_TO_VPE_BASE_OFS
        && addr <= GIC_SH_MAP_TO_VPE_REG_OFF(255, 63)) {
        reg = (addr - GIC_SH_INTR_MAP_TO_VPE_BASE_OFS) / 32;
        ret = 1 << (gic->gic_irqs[reg].map_vpe);
        DPRINTF("(GIC) -> 0x%016x\n", ret);
        return ret;
    }

    /* VPE-Local Register */
    if (addr >= GIC_VPELOCAL_BASE_ADDR && addr < GIC_VPEOTHER_BASE_ADDR) {
        return gic_read_vpe(gic, vp_index, addr - GIC_VPELOCAL_BASE_ADDR, size);
    }

    /* VPE-Other Register */
    if (addr >= GIC_VPEOTHER_BASE_ADDR && addr < GIC_USERMODE_BASE_ADDR) {
        uint32_t other_index = gic->gic_vpe_other_addr[vp_index];
        return gic_read_vpe(gic, other_index, addr - GIC_VPEOTHER_BASE_ADDR,
                            size);
    }

    DPRINTF("GIC unimplemented register %" PRIx64 "\n", addr);
    return 0ULL;
}

/* GIC Write VPE Local/Other Registers */
static void gic_write_vpe(MIPSGICState *gic, uint32_t vp_index, hwaddr addr,
                              uint64_t data, unsigned size)
{
    switch (addr) {
    case GIC_VPE_CTL_OFS:
        gic->gic_vpe_ctl[vp_index] &= ~1;
        gic->gic_vpe_ctl[vp_index] |= data & 1;

        DPRINTF("QEMU: GIC_VPE%d_CTL Write %lx\n", vp_index, data);
        break;
    case GIC_VPE_RMASK_OFS:
        gic->gic_vpe_mask[vp_index] &= ~(data & 0x3f) & 0x3f;

        DPRINTF("QEMU: GIC_VPE%d_RMASK Write data %lx, mask %x\n", vp_index,
                data, gic->gic_vpe_mask[vp_index]);
        break;
    case GIC_VPE_SMASK_OFS:
        gic->gic_vpe_mask[vp_index] |= (data & 0x3f);

        DPRINTF("QEMU: GIC_VPE%d_SMASK Write data %lx, mask %x\n", vp_index,
                data, gic->gic_vpe_mask[vp_index]);
        break;
    case GIC_VPE_WD_MAP_OFS:
        gic->gic_vpe_wd_map[vp_index] = data & 0xE000003F;
        break;
    case GIC_VPE_COMPARE_MAP_OFS:
        gic->gic_vpe_compare_map[vp_index] = data & 0xE000003F;

        DPRINTF("QEMU: GIC_VPE%d_COMPARE_MAP %lx %x\n", vp_index,
                data, gic->gic_vpe_compare_map[vp_index]);
        break;
    case GIC_VPE_TIMER_MAP_OFS:
        gic->gic_vpe_timer_map[vp_index] = data & 0xE000003F;

        DPRINTF("QEMU: GIC Timer MAP %lx %x\n", data,
                gic->gic_vpe_timer_map[vp_index]);
        break;
    case GIC_VPE_OTHER_ADDR_OFS:
        if (data < gic->num_cpu) {
            gic->gic_vpe_other_addr[vp_index] = data;
        }

        DPRINTF("QEMU: GIC other addressing reg WRITE %lx\n", data);
        break;
    case GIC_VPE_OTHER_ADDR_OFS + 4:
        /* do nothing */
        break;
    case GIC_VPE_COMPARE_LO_OFS:
        gic_store_vpe_compare(gic, vp_index, data);
        break;
    case GIC_VPE_COMPARE_HI_OFS:
        /* do nothing */
        break;
    default:
        DPRINTF("Warning *** write %d bytes at GIC offset LOCAL/OTHER "
                "0x%" PRIx64" 0x%08lx\n", size, addr, data);
        break;
    }
}

static void gic_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    int reg, intr;
    MIPSGICState *gic = (MIPSGICState *) opaque;
    uint32_t vp_index = gic_get_current_cpu(gic);
    int i, base;

    switch (addr) {
    case GIC_SH_CONFIG_OFS:
    {
        uint32_t pre = gic->gic_gl_config;
        gic->gic_gl_config = (gic->gic_gl_config & 0xEFFFFFFF) |
                             (data & 0x10000000);
        if (pre != gic->gic_gl_config) {
            if ((gic->gic_gl_config & 0x10000000)) {
                DPRINTF("Info GIC_SH_CONFIG.COUNTSTOP modified STOPPING\n");
                gic_timer_stop_count(gic);
            }
            if (!(gic->gic_gl_config & 0x10000000)) {
                DPRINTF("Info GIC_SH_CONFIG.COUNTSTOP modified STARTING\n");
                gic_timer_start_count(gic);
            }
        }
    }
        break;
    case GIC_SH_CONFIG_OFS + 4:
        /* do nothing */
        break;
    case GIC_SH_COUNTERLO_OFS:
        if (gic->gic_gl_config & 0x10000000) {
            gic_store_sh_count(gic, data);
        }
        break;
    case GIC_SH_COUNTERHI_OFS:
        /* do nothing */
        break;
    case GIC_SH_POL_31_0_OFS:
    case GIC_SH_POL_63_32_OFS:
    case GIC_SH_POL_95_64_OFS:
    case GIC_SH_POL_127_96_OFS:
    case GIC_SH_POL_159_128_OFS:
    case GIC_SH_POL_191_160_OFS:
    case GIC_SH_POL_223_192_OFS:
    case GIC_SH_POL_255_224_OFS:
        base = (addr - GIC_SH_POL_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->gic_irqs[base + i].polarity = (data >> i) & 1;
        }
        break;
    case GIC_SH_TRIG_31_0_OFS:
    case GIC_SH_TRIG_63_32_OFS:
    case GIC_SH_TRIG_95_64_OFS:
    case GIC_SH_TRIG_127_96_OFS:
    case GIC_SH_TRIG_159_128_OFS:
    case GIC_SH_TRIG_191_160_OFS:
    case GIC_SH_TRIG_223_192_OFS:
    case GIC_SH_TRIG_255_224_OFS:
        base = (addr - GIC_SH_TRIG_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->gic_irqs[base + i].trigger_type = (data >> i) & 1;
        }
        break;
    case GIC_SH_RMASK_31_0_OFS:
    case GIC_SH_RMASK_63_32_OFS:
    case GIC_SH_RMASK_95_64_OFS:
    case GIC_SH_RMASK_127_96_OFS:
    case GIC_SH_RMASK_159_128_OFS:
    case GIC_SH_RMASK_191_160_OFS:
    case GIC_SH_RMASK_223_192_OFS:
    case GIC_SH_RMASK_255_224_OFS:
        base = (addr - GIC_SH_RMASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->gic_irqs[base + i].enabled &= !((data >> i) & 1);
        }
        break;
    case GIC_SH_WEDGE_OFS:
        DPRINTF("addr: %#" PRIx64 ", data: %#" PRIx64 ", size: %#x\n", addr,
               data, size);
        /* Figure out which VPE/HW Interrupt this maps to */
        intr = data & 0x7FFFFFFF;
        /* Mask/Enabled Checks */
        if (data & 0x80000000) {
            qemu_set_irq(gic->irqs[intr], 1);
        } else {
            qemu_set_irq(gic->irqs[intr], 0);
        }
        break;
    case GIC_SH_SMASK_31_0_OFS:
    case GIC_SH_SMASK_63_32_OFS:
    case GIC_SH_SMASK_95_64_OFS:
    case GIC_SH_SMASK_127_96_OFS:
    case GIC_SH_SMASK_159_128_OFS:
    case GIC_SH_SMASK_191_160_OFS:
    case GIC_SH_SMASK_223_192_OFS:
    case GIC_SH_SMASK_255_224_OFS:
        base = (addr - GIC_SH_SMASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->gic_irqs[base + i].enabled |= (data >> i) & 1;
        }
        break;

    default:
        if (addr < GIC_SH_INTR_MAP_TO_PIN_BASE_OFS) {
            DPRINTF("Warning *** write %d bytes at GIC offset 0x%" PRIx64
                    " 0x%08lx\n",
                    size, addr, data);
        }
        break;
    }

    /* Other cases */
    if (addr >= GIC_SH_INTR_MAP_TO_PIN_BASE_OFS
        && addr <= GIC_SH_MAP_TO_PIN(255)) {
        reg = (addr - GIC_SH_INTR_MAP_TO_PIN_BASE_OFS) / 4;
        gic->gic_irqs[reg].map_pin = data;
    }
    if (addr >= GIC_SH_INTR_MAP_TO_VPE_BASE_OFS
        && addr <= GIC_SH_MAP_TO_VPE_REG_OFF(255, 63)) {
        reg = (addr - GIC_SH_INTR_MAP_TO_VPE_BASE_OFS) / 32;
        gic->gic_irqs[reg].map_vpe = ffsll(data) - 1;
    }

    /* VPE-Local Register */
    if (addr >= GIC_VPELOCAL_BASE_ADDR && addr < GIC_VPEOTHER_BASE_ADDR) {
        gic_write_vpe(gic, vp_index, addr - GIC_VPELOCAL_BASE_ADDR,
                      data, size);
    }

    /* VPE-Other Register */
    if (addr >= GIC_VPEOTHER_BASE_ADDR && addr < GIC_USERMODE_BASE_ADDR) {
        uint32_t other_index = gic->gic_vpe_other_addr[vp_index];
        gic_write_vpe(gic, other_index, addr - GIC_VPEOTHER_BASE_ADDR,
                      data, size);
    }
}

static void gic_reset(void *opaque)
{
    int i;
    MIPSGICState *gic = (MIPSGICState *) opaque;

    /* Rest value is map to pin */
    for (i = 0; i < gic->num_irq; i++) {
        gic->gic_irqs[i].map_pin = GIC_MAP_TO_PIN_MSK;
    }

    gic->gic_sh_counterlo = 0;
    gic->gic_gl_config = 0x100f0000 | gic->num_cpu;
}


static void gic_set_irq(void *opaque, int n_IRQ, int level)
{
    int vpe = -1, pin = -1, i;
    MIPSGICState *gic = (MIPSGICState *) opaque;
    int ored_level = level;

    gic->gic_irqs[n_IRQ].pending = (bool) level;

    if (!gic->gic_irqs[n_IRQ].enabled) {
        /* GIC interrupt source disabled */
        return;
    }

    /* Mapping: assume MAP_TO_PIN */
    pin = gic->gic_irqs[n_IRQ].map_pin & 0x3f;
    vpe = gic->gic_irqs[n_IRQ].map_vpe;

    if (vpe < 0 || vpe >= gic->num_cpu) {
        return;
    }

    /* ORing pending regs sharing same pin */
    if (!ored_level) {
        for (i = 0; i < gic->num_irq; i++) {
            if ((gic->gic_irqs[i].map_pin & 0x3f) == pin &&
                    gic->gic_irqs[i].map_vpe == vpe &&
                    gic->gic_irqs[i].enabled) {
                ored_level |= gic->gic_irqs[i].pending;
            }
            if (ored_level) {
                /* no need to iterate all interrupts */
                break;
            }
        }
        if ((gic->gic_vpe_compare_map[vpe] & 0x3f) == pin  &&
                ((gic->gic_vpe_mask[vpe] & GIC_VPE_SMASK_CMP_MSK))) {
            /* ORing with local pending register (count/compare) */
            ored_level |= ((gic->gic_vpe_pend[vpe] >> 1) & 1);
        }
    }


#ifdef CONFIG_KVM
    if (kvm_enabled())  {
        kvm_mips_set_ipi_interrupt(gic->env[vpe], pin + 2, ored_level);
    }
#endif
    qemu_set_irq(gic->env[vpe]->irq[pin+2], ored_level);
}

/* Read GCR registers */
static uint64_t gcr_read(void *opaque, hwaddr addr, unsigned size)
{
    MIPSGICState *gic = (MIPSGICState *) opaque;

    DPRINTF("Info read %d bytes at GCR offset 0x%" PRIx64 " (GCR) -> ",
            size, addr);

    switch (addr) {
    case GCMP_GCB_GC_OFS:
        /* Set PCORES to 0 */
        DPRINTF("0x%016x\n", 0);
        return 0;
    case GCMP_GCB_GCMPB_OFS:
        DPRINTF("GCMP_BASE_ADDR: %016llx\n", GCMP_BASE_ADDR);
        return GCMP_BASE_ADDR;
    case GCMP_GCB_GCMPREV_OFS:
        DPRINTF("0x%016x\n", 0x800);
        return 0x800;
    case GCMP_GCB_GICBA_OFS:
        DPRINTF("0x" TARGET_FMT_lx "\n", gic->gcr_gic_base);
        return gic->gcr_gic_base;
    case GCMP_GCB_GICST_OFS:
        /* FIXME indicates a connection between GIC and CM */
        DPRINTF("0x%016x\n", GCMP_GCB_GICST_EX_MSK);
        return GCMP_GCB_GICST_EX_MSK;
    case GCMP_GCB_CPCST_OFS:
        DPRINTF("0x%016x\n", 0);
        return 0;
    case GCMP_GCB_GC_OFS + GCMP_GCB_L2_CONFIG_OFS:
        /* L2 BYPASS */
        DPRINTF("0x%016x\n", GCMP_GCB_L2_CONFIG_BYPASS_MSK);
        return GCMP_GCB_L2_CONFIG_BYPASS_MSK;
    case GCMP_CLCB_OFS + GCMP_CCB_CFG_OFS:
        /* Set PVP to # cores - 1 */
        DPRINTF("0x%016x\n", smp_cpus - 1);
        return smp_cpus - 1;
    case GCMP_COCB_OFS + GCMP_CCB_CFG_OFS:
        /* Set PVP to # cores - 1 */
        DPRINTF("0x%016x\n", smp_cpus - 1);
        return smp_cpus - 1;
    case GCMP_CLCB_OFS + GCMP_CCB_OTHER_OFS:
        DPRINTF("0x%016x\n", 0);
        return 0;
    default:
        DPRINTF("Warning *** unimplemented GCR read at offset 0x%" PRIx64 "\n",
                addr);
        return 0;
    }
    return 0ULL;
}

/* Write GCR registers */
static void gcr_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    MIPSGCRState *gcr = (MIPSGCRState *) opaque;

    switch (addr) {
    case GCMP_GCB_GICBA_OFS:
        DPRINTF("Info write %d bytes at GCR offset %" PRIx64 " <- 0x%016lx\n",
                size, addr, data);
        break;
    default:
        DPRINTF("Warning *** unimplemented GCR write at offset 0x%" PRIx64 "\n",
                addr);
        break;
    }
}

static const MemoryRegionOps gic_ops = {
    .read = gic_read,
    .write = gic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 8,
    },
};

static const MemoryRegionOps gcr_ops = {
    .read = gcr_read,
    .write = gcr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 8,
    },
};

/* QOM */
static void mips_gic_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MIPSGICState *s = MIPS_GIC(obj);
    int i;

    memory_region_init_io(&s->gic_mem, OBJECT(s), &gic_ops, s,
                          "mips-gic", GIC_ADDRSPACE_SZ);
    sysbus_init_mmio(sbd, &s->gic_mem);
    qemu_register_reset(gic_reset, s);
}

static void mips_gcr_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MIPSGCRState *s = MIPS_GCR(obj);

    memory_region_init_io(&s->gcr_mem, OBJECT(s), &gcr_ops, s,
                          "mips-gcr", GCMP_ADDRSPACE_SZ);
    sysbus_init_mmio(sbd, &s->gcr_mem);
}

static void mips_gic_realize(DeviceState *dev, Error **errp)
{
    MIPSGICState *s = MIPS_GIC(dev);
    qemu_irq *irqs = g_new(qemu_irq, s->num_irq);
    CPUState *cs = first_cpu;
    int i;

    /* Register the CPU env for all cpus with the GIC */
    for (i = 0; i < s->num_cpu; i++) {
        if (cs != NULL) {
            s->env[i] = cs->env_ptr;
            cs = CPU_NEXT(cs);
        } else {
            fprintf(stderr, "Unable to initialize GIC - CPUState for "
                    "CPU #%d not valid!", i);
            return;
        }
    }

    s->gic_irqs = g_new(MIPSGICIRQState, s->num_irq);

    gic_timer_init(s, s->num_cpu);

    qdev_init_gpio_in(dev, gic_set_irq, s->num_irq);
    for (i = 0; i < s->num_irq; i++) {
        irqs[i] = qdev_get_gpio_in(dev, i);

        s->gic_irqs[i].irq = irqs[i];

        s->gic_irqs[i].enabled = false;
        s->gic_irqs[i].pending = false;
        s->gic_irqs[i].polarity = false;
        s->gic_irqs[i].trigger_type = false;
        s->gic_irqs[i].dual_edge = false;
        s->gic_irqs[i].map_pin = GIC_MAP_TO_PIN_MSK;
        s->gic_irqs[i].map_vpe = 0;
    }
    s->irqs = irqs;
}

static Property mips_gic_properties[] = {
    DEFINE_PROP_INT32("num-cpu", MIPSGICState, num_cpu, 1),
    DEFINE_PROP_INT32("num-irq", MIPSGICState, num_irq, 256),
    DEFINE_PROP_END_OF_LIST(),
};

static Property mips_gcr_properties[] = {
    DEFINE_PROP_INT32("num-cpu", MIPSGCRState, num_cpu, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void mips_gic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = mips_gic_properties;
    dc->realize = mips_gic_realize;
}

static void mips_gcr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = mips_gcr_properties;
}

static const TypeInfo mips_gic_info = {
    .name          = TYPE_MIPS_GIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MIPSGICState),
    .instance_init = mips_gic_init,
    .class_init    = mips_gic_class_init,
};

static const TypeInfo mips_gcr_info = {
    .name          = TYPE_MIPS_GCR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MIPSGCRState),
    .instance_init = mips_gcr_init,
    .class_init    = mips_gcr_class_init,
};

static void mips_gic_register_types(void)
{
    type_register_static(&mips_gic_info);
}

static void mips_gcr_register_types(void)
{
    type_register_static(&mips_gcr_info);
}

type_init(mips_gic_register_types)
type_init(mips_gcr_register_types)


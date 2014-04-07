/*
 * QEMU MIPS timer support
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw.h"
#include "mips_cpudevs.h"
#include "qemu-timer.h"
#include "qemu-log.h"

#if defined(SV_SUPPORT)
/* In SV - C0COUNT has to be incremented by a number of executed instructions
   as this is how IASim works.
*/
#define TIMER_FREQ get_ticks_per_sec()
#else
#define TIMER_FREQ	100 * 1000 * 1000
#endif

/* XXX: do not use a global */
uint32_t cpu_mips_get_random (CPUState *env)
{
    static uint32_t lfsr = 1;
    static uint32_t prev_idx = 0;
    uint32_t idx;
    /* Don't return same value twice, so get another value */
    do {
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xd0000001u);
        idx = lfsr % (env->tlb->nb_tlb - env->CP0_Wired) + env->CP0_Wired;
    } while (idx == prev_idx);
    prev_idx = idx;
    return idx;
}

/* MIPS R4K timer */
static void cpu_mips_timer_update(CPUState *env)
{
    uint64_t now, next;
    uint32_t wait;

    now = qemu_get_clock_ns(vm_clock);
    wait = env->CP0_Compare - env->CP0_Count -
	    (uint32_t)muldiv64(now, TIMER_FREQ, get_ticks_per_sec());
#if defined(SV_SUPPORT)
    /* FIXME: workaround for SV failures.
       If there is "MTC0 $0, C0COUNT" instruction and C0COMP=0:
       QEMU - schedules timer after 0 cycles and the timer expires immediately
              (during execution of the same instruction)
       IASim - schedules timer after 0xffffffff cycles

       In general it looks like IASim schedules timer for C0COMP - C0COUNT - 1.
       Thus, doing the same here.
       */
    wait--;
    sv_log("Info (MIPS32_EXCEPT) Root - Count (Compare=%u Count=%u CauseDC=%u) schedule timer interrupt after %u (0x%x)\n",
            env->CP0_Compare, env->CP0_Count, (env->CP0_Cause >> CP0Ca_DC) & 1,
            wait, wait);
#endif
    next = now + muldiv64(wait, get_ticks_per_sec(), TIMER_FREQ);
    qemu_mod_timer(env->timer, next);
}

static void cpu_mips_guest_timer_update(CPUState *env)
{
    uint64_t now;
    uint64_t next_guest;
    uint32_t wait_guest;

    now = qemu_get_clock_ns(vm_clock);

    wait_guest = env->Guest.CP0_Compare - env->CP0_Count - env->CP0_GTOffset -
            (uint32_t)muldiv64(now, TIMER_FREQ, get_ticks_per_sec());
#if defined(SV_SUPPORT)
    /* FIXME: workaround for SV failures.
       If there is "MTC0 $0, C0COUNT" instruction and C0COMP=0:
       QEMU - schedules timer after 0 cycles and the timer expires immediately
              (during execution of the same instruction)
       IASim - schedules timer after 0xffffffff cycles

       In general it looks like IASim schedules timer for C0COMP - C0COUNT - 1.
       Thus, doing the same here.
       */
    wait_guest--;
    sv_log("Info (MIPS32_EXCEPT) Guest - Count (Compare=%u Count=%u CauseDC=%u) schedule timer interrupt after %u (0x%x)\n",
                env->Guest.CP0_Compare, env->CP0_Count + env->CP0_GTOffset, (env->Guest.CP0_Cause >> CP0Ca_DC) & 1,
                wait_guest, wait_guest);
#endif
    next_guest = now + muldiv64(wait_guest, get_ticks_per_sec(), TIMER_FREQ);
    // ignore guest timer for now
    qemu_mod_timer(env->guest_timer, next_guest);
}

/* Expire the timer.  */
static void cpu_mips_timer_expire(CPUState *env)
{
    cpu_mips_timer_update(env);
    if (env->insn_flags & ISA_MIPS32R2) {
        env->CP0_Cause |= 1 << CP0Ca_TI;
    }
    qemu_irq_raise(env->irq[(env->CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

static void cpu_mips_guest_timer_expire(CPUState *env)
{
    cpu_mips_guest_timer_update(env);
    if (env->insn_flags & ISA_MIPS32R2) {
        env->Guest.CP0_Cause |= 1 << CP0Ca_TI;
    }
    qemu_irq_raise(env->guest_irq[(env->Guest.CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

void cpu_mips_inject_guest_timer(CPUState *env)
{
    env->Guest.CP0_Cause |= 1 << CP0Ca_TI;
    qemu_irq_raise(env->guest_irq[(env->Guest.CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

void cpu_mips_clear_guest_timer(CPUState *env)
{
    env->CP0_Cause &= ~(1 << CP0Ca_TI);
    qemu_irq_lower(env->irq[(env->CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

uint32_t cpu_mips_get_count (CPUState *env)
{
    if (env->CP0_Cause & (1 << CP0Ca_DC)) {
        return env->CP0_Count;
    } else {
        uint64_t now;

        now = qemu_get_clock_ns(vm_clock);
        if (qemu_timer_pending(env->timer)
            && qemu_timer_expired(env->timer, now)) {
            /* The timer has already expired.  */
            cpu_mips_timer_expire(env);
        }

        return env->CP0_Count +
            (uint32_t)muldiv64(now, TIMER_FREQ, get_ticks_per_sec());
    }
}

void cpu_mips_store_count (CPUState *env, uint32_t count)
{
    if (env->CP0_Cause & (1 << CP0Ca_DC)) {
        env->CP0_Count = count;
    }
    else {
        /* Store new count register */
        env->CP0_Count =
            count - (uint32_t)muldiv64(qemu_get_clock_ns(vm_clock),
                                       TIMER_FREQ, get_ticks_per_sec());
        /* Update timer timer */
        cpu_mips_timer_update(env);
    }
}

void cpu_mips_store_count_guest (CPUState *env, uint32_t count)
{
    if (env->Guest.CP0_Cause & (1 << CP0Ca_DC)) {
        env->Guest.CP0_Count = count;
    }
    else {
        /* Store new count register */
        env->Guest.CP0_Count =
            count - (uint32_t)muldiv64(qemu_get_clock_ns(vm_clock),
                                       TIMER_FREQ, get_ticks_per_sec());
        /* Update timer timer */
        cpu_mips_guest_timer_update(env);
    }
}

void cpu_mips_store_compare (CPUState *env, uint32_t value)
{
    env->CP0_Compare = value;
    if (!(env->CP0_Cause & (1 << CP0Ca_DC)))
        cpu_mips_timer_update(env);
    if (env->insn_flags & ISA_MIPS32R2)
        env->CP0_Cause &= ~(1 << CP0Ca_TI);
    qemu_irq_lower(env->irq[(env->CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

void cpu_mips_store_compare_guest (CPUState *env, uint32_t value)
{
    env->Guest.CP0_Compare = value;
    if (!(env->Guest.CP0_Cause & (1 << CP0Ca_DC)))
        cpu_mips_guest_timer_update(env);
    if (env->insn_flags & ISA_MIPS32R2)
        env->Guest.CP0_Cause &= ~(1 << CP0Ca_TI);
    qemu_irq_lower(env->guest_irq[(env->Guest.CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7]);
}

void cpu_mips_start_count(CPUState *env)
{
    cpu_mips_store_count(env, env->CP0_Count);
    if (env->CP0_Config3 & (1 << CP0C3_VZ)) {
        cpu_mips_store_count_guest(env, env->CP0_Count + env->CP0_GTOffset);
    }
}

void cpu_mips_stop_count(CPUState *env)
{
    /* Store the current value */
    env->CP0_Count += (uint32_t)muldiv64(qemu_get_clock_ns(vm_clock),
                                         TIMER_FREQ, get_ticks_per_sec());
    if (env->CP0_Config3 & (1 << CP0C3_VZ)) {
        env->Guest.CP0_Count = env->CP0_Count + env->CP0_GTOffset;
    }
}

static void mips_timer_cb (void *opaque)
{
    CPUState *env;

    env = opaque;
#if 0 || defined(MIPSSIM_COMPAT)
    qemu_log("%s\n", __func__);
#endif
#if defined(SV_SUPPORT)
    sv_log("Root - Timer interrupt at %ld\n", qemu_get_clock_ns(vm_clock));
    sv_log("timer callback Root.Compare=%u Root.Count=%u\n",
             env->CP0_Compare, env->CP0_Count);
#endif
    if (env->CP0_Cause & (1 << CP0Ca_DC))
        return;

    /* ??? This callback should occur when the counter is exactly equal to
       the comparator value.  Offset the count by one to avoid immediately
       retriggering the callback before any virtual time has passed.  */
    env->CP0_Count++;
    cpu_mips_timer_expire(env);
    env->CP0_Count--;
}

static void mips_guest_timer_cb (void *opaque)
{
    CPUState *env;
    env = opaque;
#if 0 || defined(MIPSSIM_COMPAT)
    qemu_log("%s\n", __func__);
#endif
#if defined(SV_SUPPORT)
    sv_log("Guest - Timer interrupt at %ld\n", qemu_get_clock_ns(vm_clock));
    sv_log("timer callback Guest.Compare=%u Guest.Count=%u\n",
             env->Guest.CP0_Compare, env->Guest.CP0_Count);
#endif
    if (env->Guest.CP0_Cause & (1 << CP0Ca_DC))
        return;
    /* ??? This callback should occur when the counter is exactly equal to
       the comparator value.  Offset the count by one to avoid immediately
       retriggering the callback before any virtual time has passed.  */
    env->CP0_Count++;
    cpu_mips_guest_timer_expire(env);
    env->CP0_Count--;
}

void cpu_mips_clock_init (CPUState *env)
{
    env->timer = qemu_new_timer_ns(vm_clock, &mips_timer_cb, env);
    env->CP0_Compare = 0;
    cpu_mips_store_count(env, 1);

    if (env->CP0_Config3 & (1 << CP0C3_VZ)) {
        env->guest_timer = qemu_new_timer_ns(vm_clock, &mips_guest_timer_cb, env);
        env->Guest.CP0_Compare = 0;
        cpu_mips_store_count_guest(env, 1);
    }
}

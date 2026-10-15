/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Restart diagnostics: say why the keyboard restarted and, after a crash, where.
 *
 * Everything that must outlive a restart sits in __noinit RAM, which survives a soft reset, a
 * watchdog reset or the reset pin, but not a power loss. Each record carries a magic number and a
 * check value, so RAM that was not retained (or was overwritten by a bootloader) reads as empty.
 *
 * A crash and a hang need opposite fixes, and this tells them apart: a crash leaves a fault record
 * and a software reset; a hang leaves no fault record and, if a watchdog is running, a watchdog
 * reset.
 */

#include <string.h>

#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RESET_REASON)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <zmk_studio_log/diag.h>

#include "internal.h"

#define BOOT_MAGIC 0x5A534C42u  /* "ZSLB" */
#define FAULT_MAGIC 0x5A534C46u /* "ZSLF" */

/* Counts boots since the last power loss. */
static __noinit struct {
    uint32_t magic;
    uint32_t count;
    uint32_t check;
} boot_rec;

struct fault_record {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t uptime_ms;
    uint32_t thread;
    char thread_name[16];
    uint32_t check;
};

static __noinit struct fault_record fault_rec;

static uint32_t fault_check(const struct fault_record *r) {
    const uint32_t *w = (const uint32_t *)r;
    uint32_t sum = 0x9E3779B9u;

    for (size_t i = 0; i < offsetof(struct fault_record, check) / sizeof(uint32_t); i++) {
        sum = (sum ^ w[i]) * 16777619u;
    }
    return sum;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_FAULT_CAPTURE)
/* Overrides Zephyr's weak default, which halts (or resets with CONFIG_RESET_ON_FATAL_ERROR)
 * without leaving anything behind. A halted keyboard is dead until it is power cycled, so reboot.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    k_tid_t tid = k_current_get();

    memset(&fault_rec, 0, sizeof(fault_rec));
    fault_rec.reason = reason;
    fault_rec.uptime_ms = k_uptime_get_32();
    fault_rec.thread = (uint32_t)(uintptr_t)tid;

#if IS_ENABLED(CONFIG_THREAD_NAME)
    const char *name = k_thread_name_get(tid);
    if (name) {
        strncpy(fault_rec.thread_name, name, sizeof(fault_rec.thread_name) - 1);
    }
#endif

#if defined(CONFIG_CPU_CORTEX_M)
    /* NULL for faults that are not CPU exceptions, such as k_panic(). */
    if (esf) {
        fault_rec.pc = esf->basic.pc;
        fault_rec.lr = esf->basic.lr;
    }
#else
    ARG_UNUSED(esf);
#endif

    fault_rec.magic = FAULT_MAGIC;
    fault_rec.check = fault_check(&fault_rec);

    printk("zmk_diag crash reason=%u pc=0x%08x lr=0x%08x thread=%s; rebooting\n", reason,
           fault_rec.pc, fault_rec.lr, fault_rec.thread_name);

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}
#endif

static const char *fault_reason_name(uint32_t reason) {
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "cpu_exception";
    case K_ERR_SPURIOUS_IRQ:
        return "spurious_irq";
    case K_ERR_STACK_CHK_FAIL:
        return "stack_overflow";
    case K_ERR_KERNEL_OOPS:
        return "kernel_oops";
    case K_ERR_KERNEL_PANIC:
        return "kernel_panic";
    default:
        return "unknown";
    }
}

static void report_fault(void) {
    if (fault_rec.magic != FAULT_MAGIC || fault_rec.check != fault_check(&fault_rec)) {
        /* Say so explicitly: no line at all would look like the feature is missing. */
        printk("zmk_diag fault none\n");
        return;
    }

    fault_rec.thread_name[sizeof(fault_rec.thread_name) - 1] = '\0';
    printk("zmk_diag fault reason=%u(%s) pc=0x%08x lr=0x%08x thread=%s(0x%08x) at=%ums\n",
           fault_rec.reason, fault_reason_name(fault_rec.reason), fault_rec.pc, fault_rec.lr,
           fault_rec.thread_name, fault_rec.thread, fault_rec.uptime_ms);

    zmk_studio_log_diag_record(ZMK_DIAG_FAULT, (uint8_t)fault_rec.reason,
                               (uint8_t)MIN(fault_rec.uptime_ms / 1000, UINT8_MAX), fault_rec.pc);
    zmk_studio_log_diag_record(ZMK_DIAG_FAULT_LR, 0, 0, fault_rec.lr);

    /* Report once; otherwise every later boot would look like it followed a crash. */
    fault_rec.magic = 0;
}

static uint32_t next_boot_number(void) {
    if (boot_rec.magic == BOOT_MAGIC && boot_rec.check == ~boot_rec.count) {
        boot_rec.count++;
    } else {
        boot_rec.magic = BOOT_MAGIC;
        boot_rec.count = 1;
    }
    boot_rec.check = ~boot_rec.count;
    return boot_rec.count;
}

uint32_t zmk_studio_log_boot_number(void) {
    return boot_rec.magic == BOOT_MAGIC && boot_rec.check == ~boot_rec.count ? boot_rec.count : 0;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RESET_REASON)
static void append_cause(char *buf, size_t size, size_t *len, const char *name) {
    int n = snprintk(buf + *len, size - *len, "%s%s", *len ? "," : "", name);
    if (n > 0) {
        *len = MIN(*len + (size_t)n, size - 1);
    }
}

static void describe_reset_cause(uint32_t cause, char *buf, size_t size) {
    static const struct {
        uint32_t flag;
        const char *name;
    } names[] = {
        {RESET_POR, "power_on"},
        {RESET_PIN, "reset_pin"},
        {RESET_SOFTWARE, "software"},
        {RESET_BROWNOUT, "brownout"},
        {RESET_WATCHDOG, "watchdog"},
        {RESET_DEBUG, "debug"},
        {RESET_LOW_POWER_WAKE, "wake_from_sleep"},
        {RESET_CPU_LOCKUP, "cpu_lockup"},
        {RESET_HARDWARE, "hardware"},
        {RESET_USER, "user"},
        {RESET_TEMPERATURE, "temperature"},
        {RESET_CLOCK, "clock"},
        {RESET_PLL, "pll"},
        {RESET_SECURITY, "security"},
        {RESET_PARITY, "parity"},
    };
    size_t len = 0;

    buf[0] = '\0';
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (cause & names[i].flag) {
            append_cause(buf, size, &len, names[i].name);
        }
    }
    if (len == 0) {
        /* nRF52 reports no flag for a plain power-on. */
        snprintk(buf, size, "%s", cause ? "other" : "power_on");
    }
}
#endif

static int restart_diag_init(void) {
    uint32_t boot = next_boot_number();
    uint32_t cause = 0;

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RESET_REASON)
    char causes[96];
    int err = hwinfo_get_reset_cause(&cause);

    if (err) {
        printk("zmk_diag boot #%u reset=unavailable err=%d\n", boot, err);
    } else {
        describe_reset_cause(cause, causes, sizeof(causes));
        printk("zmk_diag boot #%u reset=%s cause=0x%08x\n", boot, causes, cause);
        /* nRF accumulates causes until cleared; clear so the next boot reports its own. */
        hwinfo_clear_reset_cause();
    }
#else
    printk("zmk_diag boot #%u\n", boot);
#endif

    zmk_studio_log_diag_record(ZMK_DIAG_BOOT, (uint8_t)boot, 0, cause);
    report_fault();
    return 0;
}

/* After the log module installs its printk hook (APPLICATION 0), so these lines are captured. */
SYS_INIT(restart_diag_init, APPLICATION, 1);

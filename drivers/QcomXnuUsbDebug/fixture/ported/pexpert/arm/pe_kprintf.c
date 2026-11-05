#include <kern/boot_progress.h>
/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
 */
/*
 * file: pe_kprintf.c
 *    arm platform expert debugging output initialization.
 */
#include <stdarg.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <os/log_private.h>
#include <libkern/section_keywords.h>
#if XNU_QCOM_USB_DEBUG
#include <QcomXnuUsbDebug.h>
#endif

/* Globals */
typedef void (*PE_kputc_t)(char);
SECURITY_READ_ONLY_LATE(PE_kputc_t) PE_kputc;

#if XNU_QCOM_USB_DEBUG
/* Whether verbose output was requested before a sink existed. */
static bool qcom_usb_debug_kprintf_requested;
#endif

// disable_serial_output disables kprintf() *and* unbuffered panic output.
SECURITY_READ_ONLY_LATE(bool) disable_serial_output = true;
// disable_kprintf_output only disables kprintf().
SECURITY_READ_ONLY_LATE(bool) disable_kprintf_output = true;
// disable_iolog_serial_output only disables IOLog, controlled by
// SERIALMODE_NO_IOLOG.
SECURITY_READ_ONLY_LATE(bool) disable_iolog_serial_output = false;
SECURITY_READ_ONLY_LATE(bool) enable_dklog_serial_output = false;

static SIMPLE_LOCK_DECLARE(kprintf_lock, 0);

static void serial_putc_crlf(char c);

#if XNU_QCOM_USB_DEBUG
/* kprintf reaches the console through _doprnt_log(..., PE_kputc, ...), which
 * bypasses _cnputs, so this hook is kprintf's only USB copy.  There is exactly
 * one case where the byte is already on USB: this patch tees inside _cnputs,
 * and console_write_unbuffered is a thin wrapper over _cnputs.  Every other
 * PE_kputc value -- serial_putc_crlf, boot_progress_putc, or NULL -- owns the
 * byte elsewhere but not in USB, so the byte is teed here.
 */
static bool
qcom_usb_pe_kputc_owns_usb_byte(void)
{
	return PE_kputc != console_write_unbuffered;
}

static void
qcom_usb_tee(char c)
{
	if (PE_kputc && qcom_usb_pe_kputc_owns_usb_byte()) {
		qcom_usb_debug_putc(c);
	}
	if (PE_kputc) {
		PE_kputc(c);
	}
}
#define QCOM_USB_DEBUG_KPRINTF_PUTC qcom_usb_tee
#else
#define QCOM_USB_DEBUG_KPRINTF_PUTC PE_kputc
#endif

__startup_func
static void
PE_init_kprintf_config(void)
{
	if (PE_state.initialized == FALSE) {
		panic("Platform Expert not initialized");
	}

	if (debug_boot_arg & DB_KPRT) {
		disable_serial_output = false;
	}

#if DEBUG
	disable_kprintf_output = false;
#elif DEVELOPMENT
	bool enable_kprintf_spam = false;
	if (PE_parse_boot_argn("-enable_kprintf_spam", &enable_kprintf_spam, sizeof(enable_kprintf_spam))) {
		disable_kprintf_output = false;
	}
#endif
#if XNU_QCOM_USB_DEBUG
	/* This board passes -v, so debug=0x14e sets DB_KPRT and kprintf is on.
	 * Defer the decision until PE_init_kprintf has a sink to write to; the
	 * sink's CPU backlog needs no scheduler and is safe from the first byte.
	 */
	qcom_usb_debug_kprintf_requested = !disable_kprintf_output;
	disable_kprintf_output = true;
#endif
}
// Do this early, so other code can depend on whether kprintf is enabled.
STARTUP(TUNABLES, STARTUP_RANK_LAST, PE_init_kprintf_config);

__startup_func
static void
PE_init_kprintf(void)
{
#if XNU_QCOM_USB_DEBUG
	/* This STARTUP is the first point on this board where the module may
	 * touch its controller: arm_init.c has already called arm_vm_init() and
	 * serial_init(), and console_io_allowed() still panics in
	 * interrupt-disabled context until STARTUP_SUB_EARLY_BOOT.
	 *
	 * Both steps run before the boot-progress branch below, because
	 * zzz-boot-progress.patch makes that branch return early.  That patch's
	 * line is left byte-identical; this port only has to run first.
	 *
	 * A refused initialization costs one banner line: the previous PE_kputc
	 * value stays in place and nothing else changes.
	 */
	const bool qcom_usb_ready = qcom_usb_debug_init();
	disable_kprintf_output = !qcom_usb_debug_kprintf_requested;
#endif
	if (boot_progress_active()) { PE_kputc = boot_progress_putc; return; }
	if (serial_init()) {
		PE_kputc = serial_putc_crlf;
	} else {
		/**
		 * If serial failed to initialize then fall back to using the console,
		 * and assume the console is using the video console (because clearly
		 * serial doesn't work).
		 */
		PE_kputc = console_write_unbuffered;
	}
#if XNU_QCOM_USB_DEBUG
	if (qcom_usb_ready) {
		PE_kputc = qcom_usb_debug_putc;
	}
#endif
}
STARTUP(KPRINTF, STARTUP_RANK_FIRST, PE_init_kprintf);

#ifdef MP_DEBUG
static void
_kprintf(const char *format, ...)
{
	va_list         listp;

	va_start(listp, format);
	_doprnt_log(format, &listp, QCOM_USB_DEBUG_KPRINTF_PUTC, 16);
	va_end(listp);
}
#define MP_DEBUG_KPRINTF(x...)  _kprintf(x)
#else                           /* MP_DEBUG */
#define MP_DEBUG_KPRINTF(x...)
#endif                          /* MP_DEBUG */

#if CONFIG_NO_KPRINTF_STRINGS
/* Prevent CPP from breaking the definition below */
#undef kprintf
#endif

static int      cpu_last_locked = 0;

__attribute__((noinline, not_tail_called))
void
kprintf(const char *fmt, ...)
{
	va_list         listp;
	va_list         listp2;
	boolean_t       state;
	void           *caller = __builtin_return_address(0);

	if (!disable_serial_output && !disable_kprintf_output) {
		va_start(listp, fmt);
		va_copy(listp2, listp);
		/*
		 * Spin to get kprintf lock but re-enable interrupts while failing.
		 * This allows interrupts to be handled while waiting but
		 * interrupts are disabled once we have the lock.
		 */
		state = ml_set_interrupts_enabled(FALSE);
		while (!simple_lock_try(&kprintf_lock, LCK_GRP_NULL)) {
			ml_set_interrupts_enabled(state);
			ml_set_interrupts_enabled(FALSE);
		}

		if (cpu_number() != cpu_last_locked) {
			MP_DEBUG_KPRINTF("[cpu%d...]\n", cpu_number());
			cpu_last_locked = cpu_number();
		}

		_doprnt_log(fmt, &listp, QCOM_USB_DEBUG_KPRINTF_PUTC, 16);

		simple_unlock(&kprintf_lock);

#if SCHED_HYGIENE_DEBUG
		/*
		 * kprintf holds interrupts disabled for far too long
		 * and would trip the spin-debugger.  If we are about to reenable
		 * interrupts then clear the timer and avoid panicking on the delay.
		 * Otherwise, let the code that printed with interrupt disabled
		 * take the panic when it reenables interrupts.
		 * Hopefully one day this is fixed so that this workaround is unnecessary.
		 */
		if (state == TRUE) {
			ml_spin_debug_clear_self();
		}
#endif
		ml_set_interrupts_enabled(state);
		va_end(listp);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
		os_log_with_args(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, fmt, listp2, caller);
		va_end(listp2);
	} else {
		va_start(listp, fmt);
		os_log_with_args(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, fmt, listp, caller);
		va_end(listp);
	}
#pragma clang diagnostic pop
}

static void
serial_putc_crlf(char c)
{
	if (c == '\n') {
		uart_putc('\r');
	}
	uart_putc(c);
}

void
serial_putc_options(char c, bool poll)
{
	uart_putc_options(c, poll);
}

void
serial_putc(char c)
{
	uart_putc(c);
}

int
serial_getc(void)
{
	return uart_getc();
}

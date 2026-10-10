/*
WII_PLATFORM_CHECKS.C

Checks of the Wii platform services through the SDK interfaces the engine
calls (HWI-015): time, threads and their FPSCR, events and mutexes, files on
the SD card through Xbox paths, contiguous memory from the MEM2 arena, and
debug output. Each check logs one "CHECK <name> <pass|fail> ..." line.
*/

#include "platform.h"
#include "wii_platform.h"
#include "port_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *name, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void check(int ok, const char *name, const char *format, ...)
{
	char detail[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(detail, sizeof(detail), format, arguments);
	va_end(arguments);
	wii_log("CHECK %s %s %s\n", name, ok ? "pass" : "fail", detail);
	if (!ok)
		failures++;
}

static unsigned long fpscr_now(void)
{
#if defined(__PPC__)
	double value;
	unsigned long long bits;

	__asm__ volatile("mffs %0" : "=f"(value));
	memcpy(&bits, &value, sizeof(bits));
	return (unsigned long)bits;
#else
	/* (the i686 host reference: no FPSCR; IEEE already) */
	return 0;
#endif
}

struct thread_probe
{
	HANDLE ready;
	unsigned long fpscr;
};

static DWORD WINAPI thread_probe_main(LPVOID parameter)
{
	struct thread_probe *probe = parameter;

	probe->fpscr = fpscr_now();
	SetEvent(probe->ready);
	return 42;
}

/* the data and save roots (port/linux/src/xbox_files.c reads them from
config.toml, which port_config.c keeps in WII_ENGINE_ROOT) */
int wii_platform_configure(void)
{
	int data = config_write("paths.data", WII_ENGINE_DATA_ROOT);
	int saves = config_write("paths.saves", WII_ENGINE_SAVE_ROOT);

	wii_log("CONFIG paths.data=%s (%d) paths.saves=%s (%d)\n", config_string("paths.data"), data,
		config_string("paths.saves"), saves);
	return data && saves;
}

int wii_platform_checks(void)
{
	failures = 0;

	/* ---------- time */
	{
		LARGE_INTEGER frequency, before, after;
		DWORD tick_before = GetTickCount(), tick_after;
		long long elapsed;

		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&before);
		Sleep(50);
		QueryPerformanceCounter(&after);
		tick_after = GetTickCount();
		elapsed = after.QuadPart - before.QuadPart;
		check(frequency.QuadPart == 1000000 && elapsed >= 45000 && elapsed < 1000000 && tick_after >= tick_before,
			"time", "frequency=%lld sleep50_us=%lld tick_ms=%lu", (long long)frequency.QuadPart, elapsed,
			(unsigned long)(tick_after - tick_before));
	}

	/* ---------- threads, events, FPSCR */
	{
		struct thread_probe probe;
		struct wii_thread_report report;
		DWORD id = 0, code = 0, ready_wait, thread_wait;
		HANDLE thread;

		probe.ready = CreateEventA(NULL, FALSE, FALSE, NULL);
		probe.fpscr = 0xffffffffUL;
		thread = CreateThread(NULL, 0, thread_probe_main, &probe, 0, &id);
		ready_wait = WaitForSingleObject(probe.ready, 2000);
		thread_wait = thread ? WaitForSingleObject(thread, 2000) : WAIT_FAILED;
		if (thread)
			GetExitCodeThread(thread, &code);
		wii_thread_report_get(&report);
		check(thread && ready_wait == WAIT_OBJECT_0 && thread_wait == WAIT_OBJECT_0 && code == 42 &&
			!(probe.fpscr & 0x4) && report.created >= 1 && report.fpscr_cleared >= 1,
			"thread", "id=%lu exit=%lu thread_fpscr=%08lx before=%08lx after=%08lx created=%lu cleared=%lu",
			(unsigned long)id, (unsigned long)code, probe.fpscr, (unsigned long)report.last_fpscr_before,
			(unsigned long)report.last_fpscr_after, report.created, report.fpscr_cleared);
		check(WaitForSingleObject(probe.ready, 0) == WAIT_TIMEOUT, "event_auto_reset", "timeout as expected");
		if (thread)
			CloseHandle(thread);
		CloseHandle(probe.ready);
	}
	{
		HANDLE mutex = CreateMutexA(NULL, TRUE, NULL);
		BOOL first = ReleaseMutex(mutex), second = ReleaseMutex(mutex);
		DWORD error = GetLastError();

		check(mutex && first && !second && error == ERROR_NOT_OWNER, "mutex", "release=%d again=%d error=%lu",
			first, second, (unsigned long)error);
		CloseHandle(mutex);
	}

	/* ---------- files on the SD card through Xbox paths */
	{
		static unsigned char written[8192], read_back[8192];
		DWORD count = 0, size = 0, attributes;
		HANDLE file;
		BOOL wrote = FALSE, read = FALSE, seek_read = FALSE;
		unsigned long index;

		for (index = 0; index < sizeof(written); index++)
			written[index] = (unsigned char)(index * 131 + 7);
		CreateDirectoryA("z:\\hwi015", NULL);
		file = CreateFileA("z:\\hwi015\\probe.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			wrote = WriteFile(file, written, sizeof(written), &count, NULL) && count == sizeof(written);
			CloseHandle(file);
		}
		file = CreateFileA("Z:\\HWI015\\PROBE.BIN", GENERIC_READ, 0, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			size = GetFileSize(file, NULL);
			read = ReadFile(file, read_back, sizeof(read_back), &count, NULL) && count == sizeof(read_back) &&
				!memcmp(written, read_back, sizeof(written));
			if (SetFilePointer(file, 4096, NULL, FILE_BEGIN) == 4096)
				seek_read = ReadFile(file, read_back, 16, &count, NULL) && count == 16 &&
					!memcmp(written + 4096, read_back, 16);
			CloseHandle(file);
		}
		DeleteFileA("z:\\hwi015\\probe.bin");
		attributes = GetFileAttributesA("z:\\hwi015\\probe.bin");
		check(wrote && read && seek_read && size == sizeof(written) && attributes == (DWORD)-1, "files",
			"write=%d read=%d seek_read=%d size=%lu deleted=%d (case-insensitive Xbox path z:\\ -> %s)", wrote, read,
			seek_read, (unsigned long)size, attributes == (DWORD)-1, WII_ENGINE_SAVE_ROOT "/z");
	}

	/* ---------- contiguous memory from the arena's general region */
	{
		struct wii_arena_report before, after;
		void *block, *fixed;

		wii_arena_report_get(&before);
		block = XPhysicalAlloc(0x10000, (ULONG_PTR)-1, 0x1000, PAGE_READWRITE);
		fixed = XPhysicalAlloc(0x1000, 0x003A6000, 0, PAGE_READWRITE);
		wii_arena_report_get(&after);
		check(block && !fixed && (unsigned long)block >= before.region_base[_wii_arena_general] &&
			after.general_in_use == 0x10000 && XQueryMemoryProtect(block) == PAGE_READWRITE,
			"contiguous_memory", "block=%p general_in_use=%lu fixed_address_request=%p (rejected: ADR-015)", block,
			(unsigned long)after.general_in_use, fixed);
		XPhysicalFree(block);
		wii_arena_report_get(&after);
		check(after.general_in_use == 0, "contiguous_memory_free", "general_in_use=%lu peak=%lu",
			(unsigned long)after.general_in_use, (unsigned long)after.general_peak);
	}

	/* ---------- debug output */
	OutputDebugStringA("HWI-015 OutputDebugStringA reaches the SD log\n");
	check(wii_log_healthy(), "debug_output", "log writes ok");

	return failures;
}

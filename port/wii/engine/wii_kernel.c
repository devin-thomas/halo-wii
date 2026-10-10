/*
WII_KERNEL.C

Win32/XAPI kernel services for the Wii engine build (HWI-015): handles,
events, mutexes, critical sections, interlocked operations, threads,
asynchronous procedure calls, time, heap memory and debug output.

It follows port/linux/src/xbox_kernel.c, the Linux implementation of the same
interfaces, over the POSIX threads devkitPPC's newlib provides on libogc's
threads. What differs on the Wii:
- No thread-local storage (libogc sets up no thread pointer): the last error
  and the queued asynchronous procedure calls are per-thread values kept
  with pthread keys.
- Every thread clears FPSCR[NI] before the engine's routine runs (ADR-018).
- Time comes from the PowerPC time base (gettime), a monotonic counter; the
  calendar time from the real-time clock (time()).
- Debug output goes to the SD log (wii_log.c).
*/

#include "platform.h"
#include "wii_platform.h"
#include "../runtime/runtime_start.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PLATFORM_HANDLE_SIGNATURE 0x686e646cUL /* 'hndl' */

/* libogc (declared here: its headers' BOOL and the XDK's differ) */
extern void LWP_YieldThread(void);
extern void *SYS_GetArena1Lo(void);
extern void *SYS_GetArena1Hi(void);
extern void *SYS_GetArena2Lo(void);
extern void *SYS_GetArena2Hi(void);

/* ---------- per-thread values */

/* A table keyed by pthread_self(): newlib's pthread keys did not keep a
value on libogc's main thread in Dolphin (a key set by SetLastError read
back 0), so the values live here. A thread's slot is released when it
exits (thread_main); the main thread keeps its own. */

enum
{
	_thread_value_last_error,
	_thread_value_apc,
	NUMBER_OF_THREAD_VALUES
};

#define MAXIMUM_THREAD_SLOTS 64

static struct
{
	pthread_t thread;
	int used;
	void *values[NUMBER_OF_THREAD_VALUES];
} thread_slots[MAXIMUM_THREAD_SLOTS];
static pthread_mutex_t thread_slots_lock = PTHREAD_MUTEX_INITIALIZER;

static void **thread_slot_values(int create)
{
	pthread_t self = pthread_self();
	void **values = NULL;
	int index, free_index = -1;

	pthread_mutex_lock(&thread_slots_lock);
	for (index = 0; index < MAXIMUM_THREAD_SLOTS; index++)
	{
		if (thread_slots[index].used && pthread_equal(thread_slots[index].thread, self))
		{
			values = thread_slots[index].values;
			break;
		}
		if (!thread_slots[index].used && free_index < 0)
			free_index = index;
	}
	if (!values && create && free_index >= 0)
	{
		memset(&thread_slots[free_index], 0, sizeof(thread_slots[free_index]));
		thread_slots[free_index].thread = self;
		thread_slots[free_index].used = 1;
		values = thread_slots[free_index].values;
	}
	pthread_mutex_unlock(&thread_slots_lock);
	return values;
}

static void *thread_value(int which)
{
	void **values = thread_slot_values(0);

	return values ? values[which] : NULL;
}

static void thread_value_set(int which, void *value)
{
	void **values = thread_slot_values(1);

	if (values)
		values[which] = value;
}

static void thread_slot_release(void)
{
	pthread_t self = pthread_self();
	int index;

	pthread_mutex_lock(&thread_slots_lock);
	for (index = 0; index < MAXIMUM_THREAD_SLOTS; index++)
	{
		if (thread_slots[index].used && pthread_equal(thread_slots[index].thread, self))
			thread_slots[index].used = 0;
	}
	pthread_mutex_unlock(&thread_slots_lock);
}

/* ---------- last error */

DWORD WINAPI GetLastError(void)
{
	return (DWORD)(uintptr_t)thread_value(_thread_value_last_error);
}

void WINAPI SetLastError(DWORD error)
{
	thread_value_set(_thread_value_last_error, (void *)(uintptr_t)error);
}

DWORD platform_set_last_error_from_errno(int error_number)
{
	DWORD error;

	switch (error_number)
	{
	case 0: error = ERROR_SUCCESS; break;
	case ENOENT: error = ERROR_FILE_NOT_FOUND; break;
	case ENOTDIR: error = ERROR_PATH_NOT_FOUND; break;
	case EACCES: case EPERM: case EROFS: error = ERROR_ACCESS_DENIED; break;
	case EEXIST: error = ERROR_ALREADY_EXISTS; break;
	case ENOTEMPTY: error = ERROR_DIR_NOT_EMPTY; break;
	case ENOSPC: error = ERROR_DISK_FULL; break;
	case ENOMEM: error = ERROR_NOT_ENOUGH_MEMORY; break;
	case EBADF: error = ERROR_INVALID_HANDLE; break;
	case EINVAL: error = ERROR_INVALID_PARAMETER; break;
	case EMFILE: case ENFILE: error = ERROR_TOO_MANY_OPEN_FILES; break;
	case EBUSY: error = ERROR_BUSY; break;
	default: error = ERROR_GEN_FAILURE; break;
	}
	SetLastError(error);
	return error;
}

/* ---------- handles */

struct platform_handle *platform_handle_new(long type, void *data,
	void (*destroy)(struct platform_handle *handle))
{
	struct platform_handle *handle = calloc(1, sizeof(*handle));

	if (!handle)
		return NULL;
	handle->signature = PLATFORM_HANDLE_SIGNATURE;
	handle->type = type;
	handle->data = data;
	handle->destroy = destroy;
	pthread_mutex_init(&handle->lock, NULL);
	pthread_cond_init(&handle->condition, NULL);
	return handle;
}

struct platform_handle *platform_handle_get(HANDLE handle, long type)
{
	struct platform_handle *result = (struct platform_handle *)handle;

	if (!result || (unsigned long)handle >= 0xfffff000UL ||
		result->signature != PLATFORM_HANDLE_SIGNATURE ||
		(type && result->type != type))
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return result;
}

void platform_handle_signal(struct platform_handle *handle)
{
	pthread_mutex_lock(&handle->lock);
	handle->signaled = TRUE;
	pthread_cond_broadcast(&handle->condition);
	pthread_mutex_unlock(&handle->lock);
}

static void handle_free(struct platform_handle *handle)
{
	handle->signature = 0;
	pthread_cond_destroy(&handle->condition);
	pthread_mutex_destroy(&handle->lock);
	free(handle);
}

BOOL WINAPI CloseHandle(HANDLE object)
{
	struct platform_handle *handle = platform_handle_get(object, 0);

	if (!handle)
		return FALSE;
	if (handle->type == _platform_handle_thread)
	{
		/* freed once both the handle is closed and the thread has exited */
		handle->destroy(handle);
		return TRUE;
	}
	if (handle->destroy)
		handle->destroy(handle);
	handle_free(handle);
	return TRUE;
}

/* ---------- asynchronous procedure calls */

struct platform_apc
{
	struct platform_apc *next;
	platform_apc_routine routine;
	void *context[3];
};

void platform_queue_apc(platform_apc_routine routine, void *context0, void *context1, void *context2)
{
	struct platform_apc *apc = calloc(1, sizeof(*apc));
	struct platform_apc *tail;

	if (!apc)
		return;
	apc->routine = routine;
	apc->context[0] = context0;
	apc->context[1] = context1;
	apc->context[2] = context2;
	tail = thread_value(_thread_value_apc);
	if (!tail)
	{
		thread_value_set(_thread_value_apc, apc);
		return;
	}
	while (tail->next)
		tail = tail->next;
	tail->next = apc;
}

long platform_run_apcs(void)
{
	long count = 0;
	struct platform_apc *apc;

	while ((apc = thread_value(_thread_value_apc)) != NULL)
	{
		thread_value_set(_thread_value_apc, apc->next);
		apc->routine(apc->context[0], apc->context[1], apc->context[2]);
		free(apc);
		count++;
	}
	return count;
}

/* ---------- waiting */

static void deadline_from_milliseconds(DWORD milliseconds, struct timespec *deadline)
{
	clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += milliseconds / 1000;
	deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L)
	{
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
}

static BOOL handle_try_acquire(struct platform_handle *handle)
{
	if (handle->type == _platform_handle_mutex)
	{
		if (handle->recursion == 0 || pthread_equal(handle->owner, pthread_self()))
		{
			handle->owner = pthread_self();
			handle->recursion++;
			return TRUE;
		}
		return FALSE;
	}
	if (!handle->signaled)
		return FALSE;
	if (handle->type == _platform_handle_event && !handle->manual_reset)
		handle->signaled = FALSE;
	return TRUE;
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE object, DWORD milliseconds, BOOL alertable)
{
	struct platform_handle *handle;
	struct timespec deadline;
	DWORD result = WAIT_OBJECT_0;

	if (alertable && platform_run_apcs())
		return WAIT_IO_COMPLETION;
	handle = platform_handle_get(object, 0);
	if (!handle)
		return WAIT_FAILED;
	if (handle->type != _platform_handle_event &&
		handle->type != _platform_handle_mutex &&
		handle->type != _platform_handle_thread)
	{
		return WAIT_OBJECT_0;
	}
	if (milliseconds != INFINITE)
		deadline_from_milliseconds(milliseconds, &deadline);
	pthread_mutex_lock(&handle->lock);
	while (!handle_try_acquire(handle))
	{
		if (milliseconds == 0)
		{
			result = WAIT_TIMEOUT;
			break;
		}
		if (milliseconds == INFINITE)
		{
			pthread_cond_wait(&handle->condition, &handle->lock);
		}
		else if (pthread_cond_timedwait(&handle->condition, &handle->lock, &deadline) == ETIMEDOUT)
		{
			if (!handle_try_acquire(handle))
				result = WAIT_TIMEOUT;
			break;
		}
	}
	pthread_mutex_unlock(&handle->lock);
	return result;
}

DWORD WINAPI WaitForSingleObject(HANDLE object, DWORD milliseconds)
{
	return WaitForSingleObjectEx(object, milliseconds, FALSE);
}

/* ---------- events */

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset,
	BOOL initial_state, LPCSTR name)
{
	struct platform_handle *handle = platform_handle_new(_platform_handle_event, NULL, NULL);

	(void)attributes;
	if (name)
		platform_log("CreateEventA: named event \"%s\" is process-local", name);
	if (!handle)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	handle->manual_reset = manual_reset;
	handle->signaled = initial_state;
	return handle;
}

BOOL WINAPI SetEvent(HANDLE event)
{
	struct platform_handle *handle = platform_handle_get(event, _platform_handle_event);

	if (!handle)
		return FALSE;
	platform_handle_signal(handle);
	return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE event)
{
	struct platform_handle *handle = platform_handle_get(event, _platform_handle_event);

	if (!handle)
		return FALSE;
	pthread_mutex_lock(&handle->lock);
	handle->signaled = FALSE;
	pthread_mutex_unlock(&handle->lock);
	return TRUE;
}

/* ---------- mutexes */

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCSTR name)
{
	struct platform_handle *handle = platform_handle_new(_platform_handle_mutex, NULL, NULL);

	(void)attributes;
	(void)name;
	if (!handle)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if (initial_owner)
	{
		handle->owner = pthread_self();
		handle->recursion = 1;
	}
	return handle;
}

BOOL WINAPI ReleaseMutex(HANDLE mutex)
{
	struct platform_handle *handle = platform_handle_get(mutex, _platform_handle_mutex);
	BOOL result = FALSE;

	if (!handle)
		return FALSE;
	pthread_mutex_lock(&handle->lock);
	if (handle->recursion > 0 && pthread_equal(handle->owner, pthread_self()))
	{
		if (--handle->recursion == 0)
			pthread_cond_broadcast(&handle->condition);
		result = TRUE;
	}
	else
	{
		SetLastError(ERROR_NOT_OWNER);
	}
	pthread_mutex_unlock(&handle->lock);
	return result;
}

/* ---------- critical sections (the XDK maps them onto the Rtl* exports) */

static pthread_mutex_t *critical_section_mutex(PRTL_CRITICAL_SECTION section)
{
	static pthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t **slot = (pthread_mutex_t **)section;

	if (!*slot)
	{
		pthread_mutex_lock(&creation_lock);
		if (!*slot)
		{
			pthread_mutexattr_t attributes;
			pthread_mutex_t *mutex = malloc(sizeof(*mutex));

			pthread_mutexattr_init(&attributes);
			pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
			pthread_mutex_init(mutex, &attributes);
			pthread_mutexattr_destroy(&attributes);
			*slot = mutex;
		}
		pthread_mutex_unlock(&creation_lock);
	}
	return *slot;
}

VOID NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION section)
{
	memset(section, 0, sizeof(*section));
	critical_section_mutex(section);
}

VOID NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	pthread_mutex_lock(critical_section_mutex(section));
}

VOID NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION section)
{
	pthread_mutex_unlock(critical_section_mutex(section));
}

DWORD NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	return pthread_mutex_trylock(critical_section_mutex(section)) == 0;
}

/* ---------- interlocked operations (halo_wii_prefix.h)

The 750CL has lwarx/stwcx.; GCC's __sync builtins use them. */

LONG WINAPI halo_linux_InterlockedIncrement(LPLONG addend)
{
	return __sync_add_and_fetch(addend, 1);
}

LONG WINAPI halo_linux_InterlockedDecrement(LPLONG addend)
{
	return __sync_sub_and_fetch(addend, 1);
}

LONG WINAPI halo_linux_InterlockedExchange(LPLONG target, LONG value)
{
	return __sync_lock_test_and_set(target, value);
}

LONG WINAPI halo_linux_InterlockedExchangeAdd(LPLONG addend, LONG value)
{
	return __sync_fetch_and_add(addend, value);
}

LONG WINAPI halo_linux_InterlockedCompareExchange(LPLONG destination, LONG exchange, LONG comparand)
{
	return __sync_val_compare_and_swap(destination, comparand, exchange);
}

/* MSVC's compiler barrier; sync orders memory too */
void _ReadWriteBarrier(void)
{
	__asm__ volatile("sync" : : : "memory");
}

/* ---------- threads */

/* the smallest stack an engine thread gets; libogc's own default is 8 KiB */
#define WII_MINIMUM_THREAD_STACK 0x10000

struct platform_thread
{
	struct platform_handle *handle;
	LPTHREAD_START_ROUTINE start;
	LPVOID parameter;
	DWORD exit_code;
	BOOL suspended;
	BOOL closed;
	BOOL finished;
	pthread_t thread;
};

static struct wii_thread_report thread_report;
static pthread_mutex_t thread_report_lock = PTHREAD_MUTEX_INITIALIZER;

void wii_thread_report_get(struct wii_thread_report *report)
{
	pthread_mutex_lock(&thread_report_lock);
	*report = thread_report;
	pthread_mutex_unlock(&thread_report_lock);
}

static void thread_release(struct platform_handle *handle)
{
	struct platform_thread *thread = handle->data;
	BOOL free_now;

	pthread_mutex_lock(&handle->lock);
	free_now = thread->finished;
	thread->closed = TRUE;
	pthread_mutex_unlock(&handle->lock);
	if (free_now)
	{
		free(thread);
		handle_free(handle);
	}
}

static void *thread_main(void *context)
{
	struct platform_thread *thread = context;
	struct platform_handle *handle = thread->handle;
	struct wii_runtime_start_record start;
	DWORD exit_code;
	BOOL free_now;
	int ieee;

	/* ADR-018: this thread's FPSCR, before any engine code */
	ieee = wii_runtime_start(&start);
	pthread_mutex_lock(&thread_report_lock);
	thread_report.fpscr_cleared += ieee != 0;
	thread_report.last_fpscr_before = start.fp_control_before;
	thread_report.last_fpscr_after = start.fp_control_after;
	pthread_mutex_unlock(&thread_report_lock);

	pthread_mutex_lock(&handle->lock);
	while (thread->suspended)
		pthread_cond_wait(&handle->condition, &handle->lock);
	pthread_mutex_unlock(&handle->lock);

	exit_code = thread->start(thread->parameter);

	pthread_mutex_lock(&handle->lock);
	thread->exit_code = exit_code;
	thread->finished = TRUE;
	handle->signaled = TRUE;
	pthread_cond_broadcast(&handle->condition);
	free_now = thread->closed;
	pthread_mutex_unlock(&handle->lock);
	thread_slot_release();
	if (free_now)
	{
		free(thread);
		handle_free(handle);
	}
	return NULL;
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES attributes, DWORD stack_size,
	LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	static LONG next_thread_id = 1;
	struct platform_thread *thread = calloc(1, sizeof(*thread));
	struct platform_handle *handle;
	pthread_attr_t thread_attributes;

	(void)attributes;
	if (!thread)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	handle = platform_handle_new(_platform_handle_thread, thread, thread_release);
	if (!handle)
	{
		free(thread);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	thread->handle = handle;
	thread->start = start;
	thread->parameter = parameter;
	thread->exit_code = STILL_ACTIVE;
	thread->suspended = (flags & CREATE_SUSPENDED) != 0;

	pthread_attr_init(&thread_attributes);
	pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&thread_attributes,
		stack_size > WII_MINIMUM_THREAD_STACK ? stack_size : WII_MINIMUM_THREAD_STACK);
	if (pthread_create(&thread->thread, &thread_attributes, thread_main, thread) != 0)
	{
		pthread_attr_destroy(&thread_attributes);
		free(thread);
		handle_free(handle);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	pthread_attr_destroy(&thread_attributes);
	pthread_mutex_lock(&thread_report_lock);
	thread_report.created++;
	pthread_mutex_unlock(&thread_report_lock);
	if (thread_id)
		*thread_id = (DWORD)halo_linux_InterlockedIncrement(&next_thread_id);
	return handle;
}

DWORD WINAPI ResumeThread(HANDLE object)
{
	struct platform_handle *handle = platform_handle_get(object, _platform_handle_thread);
	struct platform_thread *thread;
	DWORD previous;

	if (!handle)
		return (DWORD)-1;
	thread = handle->data;
	pthread_mutex_lock(&handle->lock);
	previous = thread->suspended ? 1 : 0;
	thread->suspended = FALSE;
	pthread_cond_broadcast(&handle->condition);
	pthread_mutex_unlock(&handle->lock);
	return previous;
}

BOOL WINAPI SetThreadPriority(HANDLE object, int priority)
{
	/* the Xbox scheduling hints are not needed for correctness; libogc
	threads keep the priority pthread_create gives them */
	(void)priority;
	return object == GetCurrentThread() || platform_handle_get(object, _platform_handle_thread) != NULL;
}

BOOL WINAPI GetExitCodeThread(HANDLE object, LPDWORD exit_code)
{
	struct platform_handle *handle = platform_handle_get(object, _platform_handle_thread);
	struct platform_thread *thread;

	if (!handle)
		return FALSE;
	thread = handle->data;
	pthread_mutex_lock(&handle->lock);
	*exit_code = thread->exit_code;
	pthread_mutex_unlock(&handle->lock);
	return TRUE;
}

BOOL WINAPI SwitchToThread(void)
{
	LWP_YieldThread();
	return TRUE;
}

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable)
{
	if (alertable && platform_run_apcs())
		return WAIT_IO_COMPLETION;
	if (milliseconds == 0)
	{
		LWP_YieldThread();
		return 0;
	}
	if (milliseconds == INFINITE)
	{
		for (;;)
			usleep(1000000);
	}
	usleep((useconds_t)milliseconds * 1000);
	if (alertable && platform_run_apcs())
		return WAIT_IO_COMPLETION;
	return 0;
}

VOID WINAPI Sleep(DWORD milliseconds)
{
	SleepEx(milliseconds, FALSE);
}

/* ---------- time */

/* the time base runs at a quarter of the 243 MHz bus clock: 60.75 ticks per
microsecond */
static unsigned long long time_base(void)
{
	unsigned long upper, lower, again;

	do
	{
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(again));
	} while (upper != again);
	return ((unsigned long long)upper << 32) | lower;
}

unsigned long long wii_time_microseconds(void)
{
	return time_base() * 4ULL / 243ULL;
}

DWORD WINAPI GetTickCount(void)
{
	return (DWORD)(wii_time_microseconds() / 1000ULL);
}

/* As on Linux, a microsecond counter rather than the Xbox's 733 MHz CPU
clock: 32-bit intermediate arithmetic in the game stays in range. */
#define PLATFORM_PERFORMANCE_FREQUENCY 1000000ULL

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *count)
{
	count->QuadPart = (LONGLONG)wii_time_microseconds();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = (LONGLONG)PLATFORM_PERFORMANCE_FREQUENCY;
	return TRUE;
}

#define FILETIME_UNIX_EPOCH_SECONDS 11644473600ULL

void platform_unix_time_to_filetime(unsigned long seconds, unsigned long nanoseconds, FILETIME *file_time)
{
	unsigned long long value = ((unsigned long long)seconds + FILETIME_UNIX_EPOCH_SECONDS) * 10000000ULL +
		nanoseconds / 100;

	file_time->dwLowDateTime = (DWORD)value;
	file_time->dwHighDateTime = (DWORD)(value >> 32);
}

void platform_filetime_to_unix_time(const FILETIME *file_time, unsigned long *seconds, unsigned long *nanoseconds)
{
	unsigned long long value = ((unsigned long long)file_time->dwHighDateTime << 32) | file_time->dwLowDateTime;
	unsigned long long total_seconds = value / 10000000ULL;

	*seconds = total_seconds > FILETIME_UNIX_EPOCH_SECONDS ? (unsigned long)(total_seconds - FILETIME_UNIX_EPOCH_SECONDS) : 0;
	*nanoseconds = (unsigned long)(value % 10000000ULL) * 100;
}

LONG WINAPI CompareFileTime(CONST FILETIME *time1, CONST FILETIME *time2)
{
	unsigned long long value1 = ((unsigned long long)time1->dwHighDateTime << 32) | time1->dwLowDateTime;
	unsigned long long value2 = ((unsigned long long)time2->dwHighDateTime << 32) | time2->dwLowDateTime;

	return value1 < value2 ? -1 : value1 > value2 ? 1 : 0;
}

static const int days_before_month[2][13] =
{
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

static int is_leap_year(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

BOOL WINAPI SystemTimeToFileTime(CONST SYSTEMTIME *system_time, LPFILETIME file_time)
{
	unsigned long long days = 0;
	unsigned long long value;
	int year;

	if (system_time->wYear < 1601 || system_time->wMonth < 1 || system_time->wMonth > 12 ||
		system_time->wDay < 1 || system_time->wDay > 31)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (year = 1601; year < system_time->wYear; year++)
		days += is_leap_year(year) ? 366 : 365;
	days += days_before_month[is_leap_year(system_time->wYear)][system_time->wMonth - 1];
	days += system_time->wDay - 1;
	value = ((days * 24 + system_time->wHour) * 60 + system_time->wMinute) * 60 + system_time->wSecond;
	value = value * 10000000ULL + (unsigned long long)system_time->wMilliseconds * 10000ULL;
	file_time->dwLowDateTime = (DWORD)value;
	file_time->dwHighDateTime = (DWORD)(value >> 32);
	return TRUE;
}

VOID WINAPI GetSystemTime(LPSYSTEMTIME system_time)
{
	unsigned long long seconds = (unsigned long long)time(NULL);
	unsigned long days = (unsigned long)(seconds / 86400);
	int year = 1970;
	int month = 0;
	int leap;

	system_time->wDayOfWeek = (WORD)((days + 4) % 7);
	for (;;)
	{
		unsigned long year_days = is_leap_year(year) ? 366 : 365;

		if (days < year_days)
			break;
		days -= year_days;
		year++;
	}
	leap = is_leap_year(year);
	while (month < 11 && days >= (unsigned long)days_before_month[leap][month + 1])
		month++;
	system_time->wYear = (WORD)year;
	system_time->wMonth = (WORD)(month + 1);
	system_time->wDay = (WORD)(days - days_before_month[leap][month] + 1);
	system_time->wHour = (WORD)((seconds / 3600) % 24);
	system_time->wMinute = (WORD)((seconds / 60) % 60);
	system_time->wSecond = (WORD)(seconds % 60);
	system_time->wMilliseconds = 0;
}

/* ---------- heap memory (GlobalAlloc / LocalAlloc family) */

struct global_block
{
	SIZE_T size;
	SIZE_T reserved;
};

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	struct global_block *block = (flags & GMEM_ZEROINIT) ?
		calloc(1, sizeof(*block) + 8 + size) :
		malloc(sizeof(*block) + 8 + size);

	if (!block)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	block->size = size;
	return (char *)block + sizeof(*block) + 8;
}

static struct global_block *global_block_from_pointer(HGLOBAL memory)
{
	return (struct global_block *)((char *)memory - sizeof(struct global_block) - 8);
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL memory, SIZE_T size, UINT flags)
{
	struct global_block *block;
	SIZE_T old_size;

	if (!memory)
		return GlobalAlloc(flags, size);
	block = global_block_from_pointer(memory);
	old_size = block->size;
	block = realloc(block, sizeof(*block) + 8 + size);
	if (!block)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if ((flags & GMEM_ZEROINIT) && size > old_size)
		memset((char *)block + sizeof(*block) + 8 + old_size, 0, size - old_size);
	block->size = size;
	return (char *)block + sizeof(*block) + 8;
}

HLOCAL WINAPI LocalFree(HLOCAL memory)
{
	if (memory)
		free(global_block_from_pointer(memory));
	return NULL;
}

SIZE_T WINAPI LocalSize(HLOCAL memory)
{
	return memory ? global_block_from_pointer(memory)->size : 0;
}

VOID WINAPI GlobalMemoryStatus(LPMEMORYSTATUS status)
{
	/* both arenas' free space, reported as at most an Xbox-sized 64 MB so
	size arithmetic in the game cannot overflow */
	unsigned long long available =
		(unsigned long long)((char *)SYS_GetArena1Hi() - (char *)SYS_GetArena1Lo()) +
		(unsigned long long)((char *)SYS_GetArena2Hi() - (char *)SYS_GetArena2Lo());
	SIZE_T total = (SIZE_T)64 * 1024 * 1024;
	SIZE_T free_bytes = available < total ? (SIZE_T)available : total;

	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	status->dwTotalPhys = total;
	status->dwAvailPhys = free_bytes;
	status->dwTotalVirtual = 0x7ffe0000;
	status->dwAvailVirtual = 0x7ffe0000;
	status->dwMemoryLoad = (DWORD)(100 - (unsigned long long)free_bytes * 100 / total);
}

/* ---------- debug output */

VOID WINAPI OutputDebugStringA(LPCSTR string)
{
	if (string)
		wii_log("debug: %s", string);
}

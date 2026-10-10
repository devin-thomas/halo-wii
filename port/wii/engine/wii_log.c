/*
WII_LOG.C

The Wii platform layer's log and its register of unsupported entry points
(HWI-015). platform_log and platform_unimplemented are the Linux platform
layer's reporting interface (port/linux/src/platform.h), which the reused
Linux files call; here they write to the SD log.
*/

#include "platform.h"
#include "wii_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static int log_failed;

int wii_log_open(const char *path)
{
	pthread_mutex_lock(&log_lock);
	if (!log_file)
		log_file = fopen(path, "a");
	pthread_mutex_unlock(&log_lock);
	return log_file != NULL;
}

void wii_log_close(void)
{
	pthread_mutex_lock(&log_lock);
	if (log_file && fclose(log_file) != 0)
		log_failed = 1;
	log_file = NULL;
	pthread_mutex_unlock(&log_lock);
}

static void log_line(const char *prefix, const char *format, va_list arguments)
{
	char line[1024];
	int length = 0;

	if (prefix)
		length = snprintf(line, sizeof(line), "%s", prefix);
	vsnprintf(line + length, sizeof(line) - (size_t)length, format, arguments);
	pthread_mutex_lock(&log_lock);
	if (log_file)
	{
		size_t size = strlen(line);

		if (fwrite(line, 1, size, log_file) != size || (size && line[size - 1] != '\n' && fputc('\n', log_file) == EOF) ||
			fflush(log_file) != 0)
		{
			log_failed = 1;
		}
	}
	pthread_mutex_unlock(&log_lock);
}

void wii_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	log_line(NULL, format, arguments);
	va_end(arguments);
}

int wii_log_healthy(void)
{
	return !log_failed;
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	log_line("platform: ", format, arguments);
	va_end(arguments);
}

/* ---------- unsupported entry points */

#define MAXIMUM_UNSUPPORTED_NAMES 256

static struct
{
	const char *subsystem;
	const char *name;
	char text[88];
} unsupported_names[MAXIMUM_UNSUPPORTED_NAMES];
static unsigned long unsupported_name_count;
static unsigned long unsupported_call_count;

void wii_unsupported(const char *subsystem, const char *name)
{
	unsigned long index;
	int first = 0;

	pthread_mutex_lock(&log_lock);
	unsupported_call_count++;
	for (index = 0; index < unsupported_name_count; index++)
	{
		if (!strcmp(unsupported_names[index].name, name))
			break;
	}
	if (index == unsupported_name_count && index < MAXIMUM_UNSUPPORTED_NAMES)
	{
		unsupported_names[index].subsystem = subsystem;
		unsupported_names[index].name = name;
		snprintf(unsupported_names[index].text, sizeof(unsupported_names[index].text), "%s:%s", subsystem, name);
		unsupported_name_count++;
		first = 1;
	}
	pthread_mutex_unlock(&log_lock);
	if (first)
		wii_log("UNSUPPORTED %s %s\n", subsystem, name);
}

unsigned long wii_unsupported_calls(void)
{
	return unsupported_call_count;
}

unsigned long wii_unsupported_names(void)
{
	return unsupported_name_count;
}

const char *wii_unsupported_name(unsigned long index)
{
	return index < unsupported_name_count ? unsupported_names[index].text : "";
}

void wii_unsupported_reset(void)
{
	pthread_mutex_lock(&log_lock);
	unsupported_name_count = 0;
	unsupported_call_count = 0;
	pthread_mutex_unlock(&log_lock);
}

/* the Linux layer's spelling, for the reused files */
void platform_unimplemented(const char *name)
{
	wii_unsupported("sdk", name);
}

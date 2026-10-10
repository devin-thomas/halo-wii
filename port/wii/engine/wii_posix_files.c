/*
WII_POSIX_FILES.C

The file-system half of port/linux/src/posix.h for the Wii (HWI-015), over
devkitPPC's newlib and libfat (the SD card is "sd:/"). It follows
port/linux/src/posix_files.c; what differs:
- FAT keeps no access time or creation time: both read as the modification
  time, as libfat's stat reports them.
- FAT has no permission bits: "read only" is the FAT attribute, which libfat
  does not expose through chmod; posix_set_read_only fails with ENOTSUP and
  read-only files are never reported.
- newlib on libogc has no pread/pwrite (xbox_files.c's positioned reads
  and writes): they are a seek, the transfer and a seek back, under a lock
  (the descriptor's position is shared, as with POSIX pread it is not).
- File times cannot be set (newlib on libogc has no utimensat); the call
  fails with ENOTSUP. The game only sets them when copying saves.
*/

#include "posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

static void split64(unsigned long long value, posix_ulong *low, posix_ulong *high)
{
	*low = (posix_ulong)(value & 0xffffffffULL);
	*high = (posix_ulong)(value >> 32);
}

static void fill_information(const struct stat *st, struct posix_file_information *information)
{
	memset(information, 0, sizeof(*information));
	if (S_ISDIR(st->st_mode))
		information->flags |= _posix_file_is_directory;
	split64((unsigned long long)st->st_size, &information->size_low, &information->size_high);
	information->modification_seconds = (posix_ulong)st->st_mtime;
	information->access_seconds = (posix_ulong)st->st_mtime;
	information->creation_seconds = (posix_ulong)st->st_mtime;
}

int posix_stat(const char *path, struct posix_file_information *information)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_fstat(int descriptor, struct posix_file_information *information)
{
	struct stat st;

	if (fstat(descriptor, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds)
{
	(void)path;
	(void)access_seconds;
	(void)access_nanoseconds;
	(void)modification_seconds;
	(void)modification_nanoseconds;
	errno = ENOTSUP;
	return -1;
}

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	off_t offset = (off_t)(((unsigned long long)(posix_ulong)offset_high << 32) | (posix_ulong)offset_low);
	off_t result = lseek(descriptor, offset, whence);

	if (result == (off_t)-1)
		return -1;
	split64((unsigned long long)result, position_low, position_high);
	return 0;
}

int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	return ftruncate(descriptor, (off_t)(((unsigned long long)size_high << 32) | size_low));
}

int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high)
{
	struct statvfs st;

	if (statvfs(path, &st) != 0)
		return -1;
	split64((unsigned long long)st.f_bavail * st.f_frsize, free_low, free_high);
	split64((unsigned long long)st.f_blocks * st.f_frsize, total_low, total_high);
	return 0;
}

int posix_set_read_only(const char *path, int read_only)
{
	(void)path;
	(void)read_only;
	errno = ENOTSUP;
	return -1;
}

int posix_make_directory(const char *path)
{
	return mkdir(path, 0755);
}

void *posix_directory_open(const char *path)
{
	return opendir(path);
}

int posix_directory_next(void *directory, char *name, posix_ulong name_size)
{
	struct dirent *entry;

	if (!directory)
		return 0;
	while ((entry = readdir((DIR *)directory)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (strlen(entry->d_name) + 1 > name_size)
			continue;
		strcpy(name, entry->d_name);
		return 1;
	}
	return 0;
}

void posix_directory_close(void *directory)
{
	if (directory)
		closedir((DIR *)directory);
}

int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size)
{
	DIR *handle = opendir(*directory ? directory : ".");
	struct dirent *entry;
	int found = 0;

	if (!handle)
		return 0;
	while ((entry = readdir(handle)) != NULL)
	{
		if (!strcasecmp(entry->d_name, name) && strlen(entry->d_name) + 1 <= result_size)
		{
			strcpy(result, entry->d_name);
			found = 1;
			break;
		}
	}
	closedir(handle);
	return found;
}

/* ---------- positioned transfers */

static pthread_mutex_t positioned_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t positioned(int descriptor, void *read_buffer, const void *write_buffer, size_t count, off_t offset)
{
	off_t previous;
	ssize_t result = -1;

	pthread_mutex_lock(&positioned_lock);
	previous = lseek(descriptor, 0, SEEK_CUR);
	if (previous != (off_t)-1 && lseek(descriptor, offset, SEEK_SET) == offset)
	{
		result = read_buffer ? read(descriptor, read_buffer, count) : write(descriptor, write_buffer, count);
		if (lseek(descriptor, previous, SEEK_SET) != previous)
			result = -1;
	}
	pthread_mutex_unlock(&positioned_lock);
	return result;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, buffer, NULL, count, offset);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, NULL, buffer, count, offset);
}

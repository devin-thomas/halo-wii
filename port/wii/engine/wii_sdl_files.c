/*
WII_SDL_FILES.C

The four SDL file functions port/linux/src/port_config.c uses (see
port/wii/engine/sdl/SDL3/SDL.h), with stdio on the SD card.
*/

#include "SDL3/SDL.h"
#include "wii_platform.h"

#include <stdio.h>
#include <stdlib.h>

const char *SDL_GetBasePath(void)
{
	return WII_ENGINE_ROOT "/";
}

void *SDL_LoadFile(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	char *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length + 1);
		if (data && fread(data, 1, (size_t)length, file) == (size_t)length)
		{
			data[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(data);
			data = NULL;
		}
	}
	fclose(file);
	return data;
}

int SDL_SaveFile(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
	written = fwrite(data, 1, size, file) == size;
	return (fclose(file) == 0) && written;
}

void SDL_free(void *memory)
{
	free(memory);
}

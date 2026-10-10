/*
SDL3/SDL.H (Wii engine build)

Not SDL. port/linux/src/port_config.c, the native ports' settings file
(config.toml), is reused unchanged by the Wii engine build; it calls these
four SDL file functions. port/wii/engine/wii_sdl_files.c implements them
with stdio on the SD card. Nothing else of SDL exists on the Wii.
*/

#ifndef HALO_WII_SDL_FILES_H
#define HALO_WII_SDL_FILES_H

#include <stddef.h>

/* the folder config.toml is in, with its separator: WII_ENGINE_ROOT "/" */
const char *SDL_GetBasePath(void);
/* the whole file, or NULL; *size is its length; SDL_free() it */
void *SDL_LoadFile(const char *path, size_t *size);
/* 1 if the whole text was written */
int SDL_SaveFile(const char *path, const void *data, size_t size);
void SDL_free(void *memory);

#endif

// Larger stdio buffers for engine file reads (Subsystems sheet: io_buffer; Build sheet ld_extra --wrap=fopen).
// newlib's default buffer is tiny, so every lump read became many small romfs/SD requests (profiler: up to
// 800 ms of fread per burst). Read-only streams get BD3DS_IO_BUFFER_KB of buffer instead.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "bd3ds_sheets.h"

extern "C"
{
	FILE *__real_fopen(const char *path, const char *mode);
	FILE *__wrap_fopen(const char *path, const char *mode)
	{
		FILE *f = __real_fopen(path, mode);
		if (f != NULL && mode != NULL && mode[0] == 'r' && strchr(mode, '+') == NULL)
		{
			setvbuf(f, NULL, _IOFBF, BD3DS_IO_BUFFER_KB * 1024);
		}
		return f;
	}
}

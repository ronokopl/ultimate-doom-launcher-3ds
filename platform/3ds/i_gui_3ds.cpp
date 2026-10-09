// 3DS GUI stubs (replaces posix/sdl/i_gui.cpp). Subsystems sheet: gui. No mouse cursor on the 3DS.
#include "i_system.h"

bool I_SetCursor(FTexture *cursorpic)
{
	return false;
}

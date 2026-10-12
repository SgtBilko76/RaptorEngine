/*
 *  VirtualKeyboard.h
 *  On-screen keyboard for VR menus (no physical keyboard in the headset).
 */

#pragma once

#include "PlatformSpecific.h"

#ifdef SDL2
	#include <SDL2/SDL.h>
#else
	#include <SDL/SDL.h>
#endif


namespace VirtualKeyboard
{
	// Shown in VR menus whenever a text box is selected (until the player hides it).
	bool Visible( void );
	void Draw( void );
	
	// Consumes laser clicks on the keyboard; call after Mouse.TrackEvent (panel pixel coordinates).
	bool HandleEvent( SDL_Event *event );
}

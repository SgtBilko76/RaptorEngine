/*
 *  QuestInput.h
 *  Meta Quest Touch controllers -> virtual joystick (flight) + pointer mouse / keys (menus).
 */

#pragma once

#ifdef XWR_OPENXR

namespace QuestInput
{
	// Joystick device type the game binds the Touch controllers by (see XWingGame::SetDefaultControls).
	extern const char *DeviceType;
	
	// Call once per frame after XR::BeginFrame.
	void Update( void );
}

#endif

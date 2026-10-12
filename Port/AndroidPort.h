/*
 *  AndroidPort.h
 *  Meta Quest (Android) platform glue for RaptorEngine.
 */

#pragma once

#ifdef __ANDROID__

#include <string>

namespace Raptor
{
	// Redirect stdout/stderr to logcat, chdir into the game data folder, return false if data is missing.
	bool AndroidSetup( void );
	
	// Folder holding Models/, Textures/, Sounds/... (the app's external files dir).
	std::string AndroidDataDir( void );
	
	// gl4es asks for the size of the default framebuffer (SDL's window surface).
	void AndroidMainFBSize( int *w, int *h );
}

#endif

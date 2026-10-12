/*
 *  OpenXR.h
 *  Minimal OpenXR backend for HeadState on Meta Quest (Android, OpenGL ES via gl4es).
 */

#pragma once

#ifdef XWR_OPENXR

#include <cstdint>

namespace XR
{
	struct EyeView
	{
		// Pose in engine coordinates (X = forward, Y = up, Z = right), meters, relative to the tracking origin.
		double Pos[ 3 ];
		double Fwd[ 3 ];
		double Up[ 3 ];
		// Asymmetric field of view as tangents: left (negative), right, down (negative), up.
		double TanLeft, TanRight, TanDown, TanUp;
	};
	
	bool Init( int *eye_w, int *eye_h );
	void Shutdown( void );
	bool Initialized( void );
	
	// Polls OpenXR events, waits for and begins the next frame. Returns true if the eyes should be rendered.
	bool BeginFrame( void );
	bool SessionRunning( void );
	bool Focused( void );
	bool ExitRequested( void );
	
	// Center-eye (head) pose, and each eye's pose + FOV, for the frame begun by BeginFrame.
	const EyeView *Head( void );
	const EyeView *Eye( int eye );
	
	// Acquires the eye's swapchain image and returns the (gl4es-visible) framebuffer name to draw into.
	unsigned int AcquireEye( int eye );
	void ReleaseEye( int eye );
	void EndFrame( void );
	
	// Re-anchors the tracking origin to the current head position and yaw.
	void Recenter( void );
	
	// Refresh rate of the headset display.
	float RefreshRate( void );
}

#endif

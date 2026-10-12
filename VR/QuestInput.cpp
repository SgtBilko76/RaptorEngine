/*
 *  QuestInput.cpp
 *  Meta Quest Touch controllers -> virtual joystick (flight) + pointer mouse / keys (menus).
 *
 *  Flight: a virtual SDL joystick named "VRTouch Controllers" carries every stick, trigger and button,
 *  so the game's own joystick binding system (and its Controls menu) handles them like any joystick.
 *  Menus (whenever the game shows its mouse cursor): the right controller points at the head-locked UI,
 *  trigger = left click, right stick = mouse wheel, B / Menu = Escape.
 */

#ifdef XWR_OPENXR

#include "QuestInput.h"
#include "OpenXR.h"

#include <SDL2/SDL.h>
#include <cmath>
#include <cstring>

#include "RaptorGame.h"


const char *QuestInput::DeviceType = "VRTouch";

namespace
{
	enum
	{
		AXIS_STEER_X = 0,  // left stick X: yaw
		AXIS_STEER_Y,      // left stick Y: pitch
		AXIS_ROLL,         // right stick X: roll
		AXIS_COUNT
	};
	
	enum
	{
		BUTTON_A = 0, BUTTON_B, BUTTON_X, BUTTON_Y,
		BUTTON_RIGHT_TRIGGER, BUTTON_LEFT_TRIGGER,
		BUTTON_RIGHT_GRIP, BUTTON_LEFT_GRIP,
		BUTTON_RIGHT_STICK, BUTTON_LEFT_STICK,
		BUTTON_MENU,
		BUTTON_THROTTLE_UP, BUTTON_THROTTLE_DOWN,  // right stick up/down
		BUTTON_COUNT
	};
	
	SDL_Joystick *Joystick = NULL;
	int JoystickIndex = -1;
	
	bool PrevClick = false, PrevBack = false, PrevMenu = false;
	double WheelAccum = 0.;
	Uint32 RecenterStart = 0;
	bool RecenterDone = false;
	
	
	void AttachJoystick( void )
	{
		if( JoystickIndex >= 0 )
			return;
		SDL_VirtualJoystickDesc desc;
		memset( &desc, 0, sizeof(desc) );
		desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
		desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
		desc.naxes = AXIS_COUNT;
		desc.nbuttons = BUTTON_COUNT;
		desc.nhats = 0;
		desc.vendor_id = 0x2833;  // Oculus
		desc.product_id = 0x7872;
		desc.name = "VRTouch Controllers";
		JoystickIndex = SDL_JoystickAttachVirtualEx( &desc );
		if( JoystickIndex < 0 )
			fprintf( stderr, "QuestInput: can't attach virtual joystick: %s\n", SDL_GetError() );
		else
			printf( "QuestInput: virtual joystick attached\n" );
	}
	
	
	Sint16 AxisValue( float v )
	{
		if( v > 1.f ) v = 1.f;
		if( v < -1.f ) v = -1.f;
		return (Sint16)( v * 32767.f );
	}
	
	
	void PushMouseMotion( int x, int y )
	{
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = SDL_MOUSEMOTION;
		event.motion.x = x;
		event.motion.y = y;
		SDL_PushEvent( &event );
	}
	
	void PushMouseButton( bool down, int x, int y )
	{
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		event.button.button = SDL_BUTTON_LEFT;
		event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
		event.button.clicks = 1;
		event.button.x = x;
		event.button.y = y;
		SDL_PushEvent( &event );
	}
	
	void PushWheel( int amount, int x, int y )
	{
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = SDL_MOUSEWHEEL;
		event.wheel.y = amount;
		event.wheel.mouseX = x;
		event.wheel.mouseY = y;
		SDL_PushEvent( &event );
	}
	
	void PushKey( SDL_Keycode key, SDL_Scancode scancode )
	{
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = SDL_KEYDOWN;
		event.key.state = SDL_PRESSED;
		event.key.keysym.sym = key;
		event.key.keysym.scancode = scancode;
		SDL_PushEvent( &event );
		event.type = SDL_KEYUP;
		event.key.state = SDL_RELEASED;
		SDL_PushEvent( &event );
	}
}


void QuestInput::Update( void )
{
	AttachJoystick();
	const XR::HandState *left = XR::Hand( 0 );
	const XR::HandState *right = XR::Hand( 1 );
	
	// Flight controls: everything goes to the virtual joystick.
	if( JoystickIndex >= 0 )
	{
		if( ! Joystick )
			Joystick = SDL_JoystickOpen( JoystickIndex );
		if( Joystick )
		{
			// SDL convention: stick up is negative (the game binds pitch like a flight stick).
			// Left stick flies (pitch/yaw), right stick does roll and throttle.
			SDL_JoystickSetVirtualAxis( Joystick, AXIS_STEER_X, AxisValue( left->StickX ) );
			SDL_JoystickSetVirtualAxis( Joystick, AXIS_STEER_Y, AxisValue( - left->StickY ) );
			SDL_JoystickSetVirtualAxis( Joystick, AXIS_ROLL,    AxisValue( right->StickX ) );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_A, right->ButtonLower );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_B, right->ButtonUpper );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_X, left->ButtonLower );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_Y, left->ButtonUpper );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_RIGHT_TRIGGER, right->Trigger > 0.5f );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_LEFT_TRIGGER,  left->Trigger > 0.5f );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_RIGHT_GRIP, right->Grip > 0.6f );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_LEFT_GRIP,  left->Grip > 0.6f );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_RIGHT_STICK, right->StickClick );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_LEFT_STICK,  left->StickClick );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_MENU, left->Menu );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_THROTTLE_UP,   right->StickY >  0.5f );
			SDL_JoystickSetVirtualButton( Joystick, BUTTON_THROTTLE_DOWN, right->StickY < -0.5f );
		}
	}
	
	// Hold both stick clicks for a second to recenter the view.
	if( left->StickClick && right->StickClick )
	{
		if( ! RecenterStart )
			RecenterStart = SDL_GetTicks();
		else if( ! RecenterDone && (SDL_GetTicks() - RecenterStart > 1000) )
		{
			Raptor::Game->Head.Recenter();
			XR::Vibrate( 0, 0.5f, 0.1f );
			XR::Vibrate( 1, 0.5f, 0.1f );
			RecenterDone = true;
		}
	}
	else
	{
		RecenterStart = 0;
		RecenterDone = false;
	}
	
	// Menus: only while the game shows its mouse cursor.
	MouseState *mouse = &(Raptor::Game->Mouse);
	bool click = right->Trigger > 0.6f;
	bool back = right->ButtonUpper;
	bool menu = left->Menu;
	if( mouse->ShowCursor )
	{
		int px = 0, py = 0;
		bool pointing = XR::PanelPointer( 1, &px, &py );
		int x = px - mouse->OffsetX, y = py - mouse->OffsetY;
		if( pointing && ((px != mouse->X) || (py != mouse->Y)) )
			PushMouseMotion( x, y );
		
		if( pointing && (click != PrevClick) )
		{
			PushMouseButton( click, x, y );
			if( click )
				XR::Vibrate( 1, 0.25f, 0.02f );
		}
		
		// Right stick up/down scrolls lists.
		WheelAccum += right->StickY * Raptor::Game->FrameTime * 12.;
		if( fabs( right->StickY ) < 0.3f )
			WheelAccum = 0.;
		while( WheelAccum >= 1. )  { PushWheel( 1, x, y );  WheelAccum -= 1.; }
		while( WheelAccum <= -1. ) { PushWheel( -1, x, y ); WheelAccum += 1.; }
		
		if( (back && ! PrevBack) || (menu && ! PrevMenu) )
			PushKey( SDLK_ESCAPE, SDL_SCANCODE_ESCAPE );
	}
	else if( PrevClick && ! click )
		PushMouseButton( false, mouse->X - mouse->OffsetX, mouse->Y - mouse->OffsetY );
	
	PrevClick = click;
	PrevBack = back;
	PrevMenu = menu;
}

#endif

/*
 *  HeadState.cpp
 */

#include "HeadState.h"

#include "RaptorGame.h"
#include "Notification.h"

#ifdef XWR_OPENXR
	#include "OpenXR.h"
	#include "QuestInput.h"
#endif


HeadState::HeadState( void )
{
	Initialized = false;
	VR = false;
	EyeL = NULL;
	EyeR = NULL;
	
#ifndef NO_VR
	m_pHMD = NULL;
	m_pRenderModels = NULL;
#endif
#ifdef XWR_OPENXR
	EyeHandleL = EyeHandleR = 0;
#endif
	
	Recenter();
}

HeadState::~HeadState()
{
	Initialized = false;
	StopVR();
}


void HeadState::Initialize( void )
{
	StartVR();
	Recenter();
	Initialized = VR;
}


void HeadState::StartVR( void )
{
	if( Raptor::Game->Gfx.VSync || ! Raptor::Game->Gfx.Framebuffers )
	{
		// Make sure we enable framebuffers, and reduce VR stutter by disabling vsync to main monitor.
		Raptor::Game->Gfx.Framebuffers = true;
		Raptor::Game->Gfx.VSync = false;
		Raptor::Game->Gfx.Restart();
	}
	
#ifdef XWR_OPENXR
	int w = 0, h = 0;
	if( ! XR::Init( &w, &h ) )
	{
		Raptor::Game->Console.Print( "Unable to start OpenXR.", TextConsole::MSG_ERROR );
		return;
	}
	
	// The engine's eye framebuffers keep their own texture (unused), but render into the OpenXR swapchain
	// images: HeadState::Draw swaps in the acquired image's framebuffer name every frame.
	EyeL = Raptor::Game->Res.GetFramebuffer( "vr_l", w, h );
	EyeR = Raptor::Game->Res.GetFramebuffer( "vr_r", w, h );
	if( !( EyeL && EyeR ) )
		return;
	EyeHandleL = EyeL->FramebufferHandle;
	EyeHandleR = EyeR->FramebufferHandle;
	EyeL->UseFrustum = EyeR->UseFrustum = true;
	EyeL->OffsetX = EyeR->OffsetX = 0;
	
	VR = true;
#elif ! defined(NO_VR)
	vr::EVRInitError eError = vr::VRInitError_None;
	m_pHMD = vr::VR_Init( &eError, vr::VRApplication_Scene );
	
	if( eError != vr::VRInitError_None )
	{
		m_pHMD = NULL;
		Raptor::Game->Console.Print( std::string("Unable to init VR runtime: ") + std::string(vr::VR_GetVRInitErrorAsEnglishDescription(eError)), TextConsole::MSG_ERROR );
		Raptor::Game->Layers.Add( new Notification( std::string("Unable to init VR runtime:\n") + std::string(vr::VR_GetVRInitErrorAsEnglishDescription(eError)) ) );
		return;
	}
	
	m_pRenderModels = (vr::IVRRenderModels *) vr::VR_GetGenericInterface( vr::IVRRenderModels_Version, &eError );
	if( ! m_pRenderModels )
	{
		m_pHMD = NULL;
		vr::VR_Shutdown();
		Raptor::Game->Console.Print( std::string("Unable to get render model interface: ") + std::string(vr::VR_GetVRInitErrorAsEnglishDescription(eError)), TextConsole::MSG_ERROR );
		Raptor::Game->Layers.Add( new Notification( std::string("Unable to get render model interface:\n") + std::string(vr::VR_GetVRInitErrorAsEnglishDescription(eError)) ) );
		return;
	}
	
	uint32_t w = 1080, h = 1200;
	m_pHMD->GetRecommendedRenderTargetSize( &w, &h );
	
	EyeL = Raptor::Game->Res.GetFramebuffer( "vr_l", w, h );
	EyeR = Raptor::Game->Res.GetFramebuffer( "vr_r", w, h );
	if( !( EyeL && EyeR ) )
		return;  // FIXME: Show error message and call StopVR()?  Or did I expect these framebuffers to become available later for some reason?
	
	vr::VRCompositor()->SetTrackingSpace( vr::TrackingUniverseSeated );
	
	// Make sure we actually have a VR device connected.
	vr::VRCompositor()->WaitGetPoses( m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0 );
	if( ! m_rTrackedDevicePose[ vr::k_unTrackedDeviceIndex_Hmd ].bDeviceIsConnected )
	{
		Raptor::Game->Console.Print( "No VR headset detected.", TextConsole::MSG_ERROR );
		Raptor::Game->Layers.Add( new Notification( "No VR headset detected." ) );
		StopVR();
		return;
	}
	
	VR = true;
#endif
}


void HeadState::StopVR( void )
{
#ifdef XWR_OPENXR
	// Give the engine framebuffers their own names back before they are deleted.
	if( EyeL && EyeHandleL )
		EyeL->FramebufferHandle = EyeHandleL;
	if( EyeR && EyeHandleR )
		EyeR->FramebufferHandle = EyeHandleR;
	EyeHandleL = EyeHandleR = 0;
#elif ! defined(NO_VR)
	if( m_pHMD )
	{
		vr::VR_Shutdown();
		m_pHMD = NULL;
		m_pRenderModels = NULL;
	}
#endif
	
	if( EyeL )
		delete EyeL;
	if( EyeR )
		delete EyeR;
	
	VR = false;
	EyeL = NULL;
	EyeR = NULL;
	
	if( Initialized )  // Make sure we don't use deleted Cam when quitting.
	{
		Raptor::Game->Cam.Offset.SetPos(0,0,0);
		Raptor::Game->Cam.Offset.SetFwdVec(1,0,0);
		Raptor::Game->Cam.Offset.SetUpVec(0,1,0);
	}
	
	Initialized = false;
}


void HeadState::RestartVR( void )
{
#ifdef XWR_OPENXR
	// The OpenXR session lives as long as the app; nothing to restart once it runs.
	if( VR )
		return;
#endif
	StopVR();
	StartVR();
}


void HeadState::Recenter( void )
{
#ifdef XWR_OPENXR
	if( VR )
		XR::Recenter();
#elif ! defined(NO_VR)
	if( VR )
		vr::VRSystem()->ResetSeatedZeroPose();
#endif
	
	Basis.SetPos(0,0,0);
	Basis.SetFwdVec(1,0,0);
	Basis.SetUpVec(0,1,0);
	Current.Copy( &Basis );
	
	if( Initialized )  // Make sure we don't use uninitialized Cam when starting.
		Raptor::Game->Cam.Offset.Copy( &Basis );
}


#ifdef XWR_OPENXR
static void SetViewOffset( Pos3D *offset, const XR::EyeView *view )
{
	offset->SetPos( view->Pos[0], view->Pos[1], view->Pos[2] );
	offset->SetUpVec( view->Up[0], view->Up[1], view->Up[2] );
	offset->SetFwdVec( view->Fwd[0], view->Fwd[1], view->Fwd[2] );
}
#endif


void HeadState::Draw( void )
{
	if( ! Initialized )
		Initialize();
	
#ifdef XWR_OPENXR
	if( ! VR )
		return;
	
	if( XR::ExitRequested() )
	{
		Raptor::Game->Quit();
		return;
	}
	
	bool render = XR::BeginFrame();
	QuestInput::Update();
	if( ! render )
	{
		// Session not visible (or not running yet): keep the game ticking without drawing.
		XR::EndFrame();
		return;
	}
	
	const XR::EyeView *head = XR::Head();
	Current.SetPos( head->Pos[0], head->Pos[1], head->Pos[2] );
	Current.SetUpVec( head->Up[0], head->Up[1], head->Up[2] );
	Current.SetFwdVec( head->Fwd[0], head->Fwd[1], head->Fwd[2] );
	
	double frame_time = Raptor::Game->FrameTime;
	Framebuffer *eyes[ 2 ] = { EyeL, EyeR };
	for( int e = 0; e < 2; e ++ )
	{
		Framebuffer *fb = eyes[ e ];
		unsigned int fbo = XR::AcquireEye( e );
		if( ! fbo )
			continue;
		
		// Each eye renders from its own tracked pose with its own asymmetric frustum (no separation/offset hacks).
		const XR::EyeView *view = XR::Eye( e );
		fb->FramebufferHandle = fbo;
		fb->FrustumLeft  = view->TanLeft;
		fb->FrustumRight = view->TanRight;
		fb->FrustumDown  = view->TanDown;
		fb->FrustumUp    = view->TanUp;
		SetViewOffset( &(Raptor::Game->Cam.Offset), view );
		
		Raptor::Game->Gfx.DrawTo = fb;
		Raptor::Game->Gfx.SelectDefaultFramebuffer();
		Raptor::Game->Draw();
		Raptor::Game->FrameTime = 0.;  // Prevent multi-render side effects on the second eye.
		
		XR::ReleaseEye( e );
	}
	
	// Leave the head (center eye) pose for game logic that reads the camera offset.
	SetViewOffset( &(Raptor::Game->Cam.Offset), head );
	
	XR::EndFrame();
	
	// Restore default draw conditions.
	EyeL->FramebufferHandle = EyeHandleL;
	EyeR->FramebufferHandle = EyeHandleR;
	Raptor::Game->FrameTime = frame_time;
	Raptor::Game->Gfx.DrawTo = NULL;
	Raptor::Game->Gfx.SelectDefaultFramebuffer();
#elif ! defined(NO_VR)
	if( ! VR )
		return;
	
	// Make sure the OpenGL context is still valid.
	if( EyeL->LoadedTime.ElapsedSeconds() > Raptor::Game->Res.ResetTime.ElapsedSeconds() )
		EyeL->Reload();
	if( EyeR->LoadedTime.ElapsedSeconds() > Raptor::Game->Res.ResetTime.ElapsedSeconds() )
		EyeR->Reload();
	
	// For now, just clear the VR event queue without acting on anything.
	vr::VREvent_t vr_event;
	while( m_pHMD->PollNextEvent( &vr_event, sizeof(vr_event) ) ) {}
	
	vr::VRCompositor()->WaitGetPoses( m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0 );
	
	if( ! m_rTrackedDevicePose[ vr::k_unTrackedDeviceIndex_Hmd ].bDeviceIsConnected )
	{
		StopVR();
		return;
	}
	
	if( m_rTrackedDevicePose[ vr::k_unTrackedDeviceIndex_Hmd ].bPoseIsValid )
	{
		const vr::HmdMatrix34_t *matrix = &(m_rTrackedDevicePose[ vr::k_unTrackedDeviceIndex_Hmd ].mDeviceToAbsoluteTracking);
		Current.SetPos(   -(matrix->m[2][3]),   matrix->m[1][3],    matrix->m[0][3]  );
		Current.SetUpVec( -(matrix->m[2][1]),   matrix->m[1][1],    matrix->m[0][1]  );
		Current.SetFwdVec(  matrix->m[2][2],  -(matrix->m[1][2]), -(matrix->m[0][2]) );
	}
	
	Raptor::Game->Cam.Offset.SetPos( Current.DistAlong( &(Basis.Fwd), &Basis ), Current.DistAlong( &(Basis.Up), &Basis ), Current.DistAlong( &(Basis.Right), &Basis ) );
	Raptor::Game->Cam.Offset.SetUpVec( Current.Up.X, Current.Up.Y, Current.Up.Z );
	Raptor::Game->Cam.Offset.SetFwdVec( Current.Fwd.X, Current.Fwd.Y, Current.Fwd.Z );
	
	// Left Eye
	double separation = Raptor::Game->Cfg.SettingAsDouble( "vr_separation", 0.0625 );
	EyeL->OffsetX = Raptor::Game->Cfg.SettingAsInt( "vr_offset", 87 );
	Raptor::Game->Cam.Offset.MoveAlong( &(Raptor::Game->Cam.Offset.Right), separation / -2. );
	Raptor::Game->Gfx.DrawTo = EyeL;
	Raptor::Game->Gfx.SelectDefaultFramebuffer();
	Raptor::Game->Draw();
	vr::Texture_t leftEyeTexture = { (void*)(uintptr_t)(EyeL->Texture), vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit( vr::Eye_Left, &leftEyeTexture );
	
	double frame_time = Raptor::Game->FrameTime;
	Raptor::Game->FrameTime = 0.;  // Prevent multi-render side effects.
	
	// Right Eye
	EyeR->OffsetX = -(EyeL->OffsetX);
	Raptor::Game->Cam.Offset.MoveAlong( &(Raptor::Game->Cam.Offset.Right), separation );
	Raptor::Game->Gfx.DrawTo = EyeR;
	Raptor::Game->Gfx.SelectDefaultFramebuffer();
	Raptor::Game->Draw();
	vr::Texture_t rightEyeTexture = { (void*)(uintptr_t)(EyeR->Texture), vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit( vr::Eye_Right, &rightEyeTexture );
	
	// Restore default draw conditions.
	Raptor::Game->FrameTime = frame_time;
	Raptor::Game->Gfx.DrawTo = NULL;
	Raptor::Game->Gfx.SelectDefaultFramebuffer();
#endif
}

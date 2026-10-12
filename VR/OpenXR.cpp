/*
 *  OpenXR.cpp
 *  Minimal OpenXR backend for HeadState on Meta Quest (Android, OpenGL ES via gl4es).
 *
 *  The engine keeps its own per-eye render loop (HeadState::Draw); this file only provides the
 *  session, the head/eye poses + FOVs, and one framebuffer per swapchain image to render into.
 */

#ifdef XWR_OPENXR

#include "OpenXR.h"

#include "RaptorGL.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <EGL/egl.h>
#include <jni.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifndef GL_SRGB8_ALPHA8
	#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_FRAMEBUFFER_SRGB_EXT
	#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif
#ifndef GL_DEPTH_COMPONENT24
	#define GL_DEPTH_COMPONENT24 0x81A6
#endif

#define XR_CHECK(expr) do { XrResult res_ = (expr); if( XR_FAILED(res_) ) { fprintf( stderr, "OpenXR: %s failed (%d)\n", #expr, (int) res_ ); return false; } } while(0)


namespace
{
	// Swapchain images belong to the GLES driver, not gl4es (which virtualizes texture names),
	// so attach them with the driver's own entry points. Framebuffer names pass through gl4es.
	void   (*gles_FramebufferTexture2D)( GLenum, GLenum, GLenum, GLuint, GLint ) = NULL;
	void   (*gles_FramebufferRenderbuffer)( GLenum, GLenum, GLenum, GLuint ) = NULL;
	void   (*gles_GenRenderbuffers)( GLsizei, GLuint* ) = NULL;
	void   (*gles_BindRenderbuffer)( GLenum, GLuint ) = NULL;
	void   (*gles_RenderbufferStorage)( GLenum, GLenum, GLsizei, GLsizei ) = NULL;
	void   (*gles_DeleteRenderbuffers)( GLsizei, const GLuint* ) = NULL;
	GLenum (*gles_CheckFramebufferStatus)( GLenum ) = NULL;
	void   (*gles_Disable)( GLenum ) = NULL;
	const GLubyte* (*gles_GetString)( GLenum ) = NULL;
	
	struct EyeSwapchain
	{
		XrSwapchain Handle;
		int W, H;
		std::vector<XrSwapchainImageOpenGLESKHR> Images;
		std::vector<GLuint> FBOs;
		GLuint Depth;
		bool Acquired;
	};
	
	XrInstance Instance = XR_NULL_HANDLE;
	XrSystemId System = XR_NULL_SYSTEM_ID;
	XrSession Session = XR_NULL_HANDLE;
	XrSpace BaseSpace = XR_NULL_HANDLE;   // raw LOCAL
	XrSpace AppSpace = XR_NULL_HANDLE;    // LOCAL re-anchored by Recenter
	XrSpace ViewSpace = XR_NULL_HANDLE;
	XrSessionState State = XR_SESSION_STATE_UNKNOWN;
	bool Running = false;
	bool Exit = false;
	bool FrameBegun = false;
	bool ShouldRender = false;
	bool RecenterPending = true;
	XrTime DisplayTime = 0;
	float DisplayRefresh = 72.f;
	bool PerfSettings = false;
	bool BDControllers = false;
	
	// Touch controller input.
	XrActionSet ActionSet = XR_NULL_HANDLE;
	XrAction ActTrigger = XR_NULL_HANDLE, ActGrip = XR_NULL_HANDLE, ActStick = XR_NULL_HANDLE, ActStickClick = XR_NULL_HANDLE;
	XrAction ActLower = XR_NULL_HANDLE, ActUpper = XR_NULL_HANDLE, ActMenu = XR_NULL_HANDLE, ActAim = XR_NULL_HANDLE, ActHaptic = XR_NULL_HANDLE;
	XrPath HandPaths[ 2 ] = { XR_NULL_PATH, XR_NULL_PATH };
	XrSpace AimSpaces[ 2 ] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XR::HandState Hands[ 2 ];
	
	EyeSwapchain Eyes[ 3 ];  // left, right, menu panel
	bool PanelUsed = false;
	bool EyesSubmitted = false;
	XrPosef AimPoses[ 2 ];
	const float PanelDist = 1.8f, PanelWidth = 1.9f;  // meters
	XrView Views[ 2 ];
	XrCompositionLayerProjectionView LayerViews[ 2 ];
	XR::EyeView EyeViews[ 2 ];
	XR::EyeView HeadView;
	
	
	bool LoadGLES( void )
	{
		void *lib = dlopen( "libGLESv3.so", RTLD_LAZY | RTLD_LOCAL );
		if( ! lib )
			lib = dlopen( "libGLESv2.so", RTLD_LAZY | RTLD_LOCAL );
		if( ! lib )
			return false;
		*(void**) &gles_FramebufferTexture2D    = dlsym( lib, "glFramebufferTexture2D" );
		*(void**) &gles_FramebufferRenderbuffer = dlsym( lib, "glFramebufferRenderbuffer" );
		*(void**) &gles_GenRenderbuffers        = dlsym( lib, "glGenRenderbuffers" );
		*(void**) &gles_BindRenderbuffer        = dlsym( lib, "glBindRenderbuffer" );
		*(void**) &gles_RenderbufferStorage     = dlsym( lib, "glRenderbufferStorage" );
		*(void**) &gles_DeleteRenderbuffers     = dlsym( lib, "glDeleteRenderbuffers" );
		*(void**) &gles_CheckFramebufferStatus  = dlsym( lib, "glCheckFramebufferStatus" );
		*(void**) &gles_Disable                 = dlsym( lib, "glDisable" );
		*(void**) &gles_GetString               = dlsym( lib, "glGetString" );
		return gles_FramebufferTexture2D && gles_FramebufferRenderbuffer && gles_GenRenderbuffers && gles_BindRenderbuffer
			&& gles_RenderbufferStorage && gles_DeleteRenderbuffers && gles_CheckFramebufferStatus && gles_Disable && gles_GetString;
	}
	
	
	bool HasExtension( const std::vector<XrExtensionProperties> &props, const char *name )
	{
		for( size_t i = 0; i < props.size(); i ++ )
			if( strcmp( props[ i ].extensionName, name ) == 0 )
				return true;
		return false;
	}
	
	
	bool InitInstance( void )
	{
		JNIEnv *env = (JNIEnv*) SDL_AndroidGetJNIEnv();
		JavaVM *vm = NULL;
		env->GetJavaVM( &vm );
		jobject activity = (jobject) SDL_AndroidGetActivity();
		
		PFN_xrInitializeLoaderKHR init_loader = NULL;
		XR_CHECK( xrGetInstanceProcAddr( XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*) &init_loader ) );
		XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
		loader_info.applicationVM = vm;
		loader_info.applicationContext = activity;
		XR_CHECK( init_loader( (const XrLoaderInitInfoBaseHeaderKHR*) &loader_info ) );
		
		uint32_t count = 0;
		XR_CHECK( xrEnumerateInstanceExtensionProperties( NULL, 0, &count, NULL ) );
		std::vector<XrExtensionProperties> props( count, { XR_TYPE_EXTENSION_PROPERTIES } );
		XR_CHECK( xrEnumerateInstanceExtensionProperties( NULL, count, &count, props.data() ) );
		
		std::vector<const char*> enabled;
		enabled.push_back( XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME );
		enabled.push_back( XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME );
		if( HasExtension( props, "XR_BD_controller_interaction" ) )
		{
			// PICO 4 / Neo 3 controllers (same button layout as Touch).
			enabled.push_back( "XR_BD_controller_interaction" );
			BDControllers = true;
		}
		if( HasExtension( props, "XR_FB_display_refresh_rate" ) )
			enabled.push_back( "XR_FB_display_refresh_rate" );
		if( HasExtension( props, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME ) )
		{
			enabled.push_back( XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME );
			PerfSettings = true;
		}
		
		XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
		android_info.applicationVM = vm;
		android_info.applicationActivity = activity;
		
		XrInstanceCreateInfo create = { XR_TYPE_INSTANCE_CREATE_INFO };
		create.next = &android_info;
		create.enabledExtensionCount = enabled.size();
		create.enabledExtensionNames = enabled.data();
		strcpy( create.applicationInfo.applicationName, "X-Wing Revival VR" );
		strcpy( create.applicationInfo.engineName, "RaptorEngine" );
		create.applicationInfo.applicationVersion = 1;
		create.applicationInfo.engineVersion = 1;
		create.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 34 );
		XR_CHECK( xrCreateInstance( &create, &Instance ) );
		
		XrSystemGetInfo sys = { XR_TYPE_SYSTEM_GET_INFO };
		sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XR_CHECK( xrGetSystem( Instance, &sys, &System ) );
		
		XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
		if( XR_SUCCEEDED( xrGetInstanceProperties( Instance, &ip ) ) )
			printf( "OpenXR runtime: %s\n", ip.runtimeName );
		return true;
	}
	
	

	XrPath Path( const char *str )
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath( Instance, str, &path );
		return path;
	}
	
	bool CreateAction( XrAction *action, XrActionType type, const char *name, const char *localized )
	{
		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
		info.actionType = type;
		strcpy( info.actionName, name );
		strcpy( info.localizedActionName, localized );
		info.countSubactionPaths = 2;
		info.subactionPaths = HandPaths;
		XR_CHECK( xrCreateAction( ActionSet, &info, action ) );
		return true;
	}
	
	bool InitActions( void )
	{
		XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy( set_info.actionSetName, "gameplay" );
		strcpy( set_info.localizedActionSetName, "Gameplay" );
		XR_CHECK( xrCreateActionSet( Instance, &set_info, &ActionSet ) );
		
		HandPaths[ 0 ] = Path( "/user/hand/left" );
		HandPaths[ 1 ] = Path( "/user/hand/right" );
		
		if( ! ( CreateAction( &ActTrigger,    XR_ACTION_TYPE_FLOAT_INPUT,       "trigger",     "Trigger" )
		     && CreateAction( &ActGrip,       XR_ACTION_TYPE_FLOAT_INPUT,       "grip",        "Grip" )
		     && CreateAction( &ActStick,      XR_ACTION_TYPE_VECTOR2F_INPUT,    "thumbstick",  "Thumbstick" )
		     && CreateAction( &ActStickClick, XR_ACTION_TYPE_BOOLEAN_INPUT,     "stick_click", "Thumbstick click" )
		     && CreateAction( &ActLower,      XR_ACTION_TYPE_BOOLEAN_INPUT,     "button_lower","A / X" )
		     && CreateAction( &ActUpper,      XR_ACTION_TYPE_BOOLEAN_INPUT,     "button_upper","B / Y" )
		     && CreateAction( &ActMenu,       XR_ACTION_TYPE_BOOLEAN_INPUT,     "menu",        "Menu" )
		     && CreateAction( &ActAim,        XR_ACTION_TYPE_POSE_INPUT,        "aim",         "Aim" )
		     && CreateAction( &ActHaptic,     XR_ACTION_TYPE_VIBRATION_OUTPUT,  "haptic",      "Haptic" ) ) )
			return false;
		
		struct { XrAction action; const char *path; } binds[] = {
			{ ActTrigger,    "/user/hand/left/input/trigger/value" },
			{ ActTrigger,    "/user/hand/right/input/trigger/value" },
			{ ActGrip,       "/user/hand/left/input/squeeze/value" },
			{ ActGrip,       "/user/hand/right/input/squeeze/value" },
			{ ActStick,      "/user/hand/left/input/thumbstick" },
			{ ActStick,      "/user/hand/right/input/thumbstick" },
			{ ActStickClick, "/user/hand/left/input/thumbstick/click" },
			{ ActStickClick, "/user/hand/right/input/thumbstick/click" },
			{ ActLower,      "/user/hand/left/input/x/click" },
			{ ActLower,      "/user/hand/right/input/a/click" },
			{ ActUpper,      "/user/hand/left/input/y/click" },
			{ ActUpper,      "/user/hand/right/input/b/click" },
			{ ActMenu,       "/user/hand/left/input/menu/click" },
			{ ActAim,        "/user/hand/left/input/aim/pose" },
			{ ActAim,        "/user/hand/right/input/aim/pose" },
			{ ActHaptic,     "/user/hand/left/output/haptic" },
			{ ActHaptic,     "/user/hand/right/output/haptic" },
		};
		std::vector<XrActionSuggestedBinding> suggested;
		for( size_t i = 0; i < sizeof(binds) / sizeof(binds[0]); i ++ )
			suggested.push_back( { binds[ i ].action, Path( binds[ i ].path ) } );
		XrInteractionProfileSuggestedBinding profile = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		profile.interactionProfile = Path( "/interaction_profiles/oculus/touch_controller" );
		profile.countSuggestedBindings = suggested.size();
		profile.suggestedBindings = suggested.data();
		XR_CHECK( xrSuggestInteractionProfileBindings( Instance, &profile ) );
		
		// PICO: the same paths on the ByteDance profiles (optional, runtime decides which one is active).
		if( BDControllers )
		{
			const char *pico_profiles[] = { "/interaction_profiles/bytedance/pico4_controller", "/interaction_profiles/bytedance/pico_neo3_controller" };
			for( size_t i = 0; i < sizeof(pico_profiles) / sizeof(pico_profiles[0]); i ++ )
			{
				profile.interactionProfile = Path( pico_profiles[ i ] );
				XrResult result = xrSuggestInteractionProfileBindings( Instance, &profile );
				printf( "OpenXR: %s controller bindings for %s\n", XR_SUCCEEDED(result) ? "added" : "no", pico_profiles[ i ] );
			}
		}
		return true;
	}
	
	bool AttachActions( void )
	{
		for( int h = 0; h < 2; h ++ )
		{
			XrActionSpaceCreateInfo space = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
			space.action = ActAim;
			space.subactionPath = HandPaths[ h ];
			space.poseInActionSpace.orientation.w = 1.f;
			XR_CHECK( xrCreateActionSpace( Session, &space, &AimSpaces[ h ] ) );
		}
		XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach.countActionSets = 1;
		attach.actionSets = &ActionSet;
		XR_CHECK( xrAttachSessionActionSets( Session, &attach ) );
		return true;
	}
	
	float GetFloat( XrAction action, int h )
	{
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		info.subactionPath = HandPaths[ h ];
		XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
		if( XR_SUCCEEDED( xrGetActionStateFloat( Session, &info, &state ) ) && state.isActive )
			return state.currentState;
		return 0.f;
	}
	
	bool GetBool( XrAction action, int h )
	{
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		info.subactionPath = HandPaths[ h ];
		XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		if( XR_SUCCEEDED( xrGetActionStateBoolean( Session, &info, &state ) ) && state.isActive )
			return state.currentState;
		return false;
	}
	
	void SyncInput( void );
	
	bool InitSession( void )
	{
		EGLDisplay display = eglGetCurrentDisplay();
		EGLContext context = eglGetCurrentContext();
		if( (display == EGL_NO_DISPLAY) || (context == EGL_NO_CONTEXT) )
		{
			fprintf( stderr, "OpenXR: no current EGL context\n" );
			return false;
		}
		EGLint config_id = 0, n = 0;
		eglQueryContext( display, context, EGL_CONFIG_ID, &config_id );
		EGLConfig config = NULL;
		const EGLint attrs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
		if( ! eglChooseConfig( display, attrs, &config, 1, &n ) || (n != 1) )
		{
			fprintf( stderr, "OpenXR: could not recover EGLConfig\n" );
			return false;
		}
		
		PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = NULL;
		XR_CHECK( xrGetInstanceProcAddr( Instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*) &get_reqs ) );
		XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
		XR_CHECK( get_reqs( Instance, System, &reqs ) );
		
		XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
		binding.display = display;
		binding.config = config;
		binding.context = context;
		XrSessionCreateInfo create = { XR_TYPE_SESSION_CREATE_INFO };
		create.next = &binding;
		create.systemId = System;
		XR_CHECK( xrCreateSession( Instance, &create, &Session ) );
		
		XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		space.poseInReferenceSpace.orientation.w = 1.f;
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &BaseSpace ) );
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &AppSpace ) );
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &ViewSpace ) );
		
		// Prefer 90 Hz when the headset offers it.
		PFN_xrEnumerateDisplayRefreshRatesFB enum_rates = NULL;
		PFN_xrRequestDisplayRefreshRateFB request_rate = NULL;
		PFN_xrGetDisplayRefreshRateFB get_rate = NULL;
		if( XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*) &enum_rates ) ) && enum_rates
		&&  XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*) &request_rate ) ) && request_rate
		&&  XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*) &get_rate ) ) && get_rate )
		{
			uint32_t rate_count = 0;
			enum_rates( Session, 0, &rate_count, NULL );
			std::vector<float> rates( rate_count );
			enum_rates( Session, rate_count, &rate_count, rates.data() );
			float best = 0.f;
			for( size_t i = 0; i < rates.size(); i ++ )
				if( (rates[ i ] <= 72.5f) && (rates[ i ] > best) )  // The engine draws each eye separately; 72 Hz is its realistic target.
					best = rates[ i ];
			if( best > 0.f )
				request_rate( Session, best );
			get_rate( Session, &DisplayRefresh );
		}
		
		// Ask for the highest sustained CPU/GPU clocks.
		PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;
		if( PerfSettings && XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction*) &set_level ) ) && set_level )
		{
			set_level( Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT );
			set_level( Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT );
			printf( "OpenXR: CPU boost, GPU sustained high\n" );
		}
		return true;
	}
	
	
	bool InitSwapchains( int *eye_w, int *eye_h )
	{
		uint32_t count = 0;
		XR_CHECK( xrEnumerateViewConfigurationViews( Instance, System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL ) );
		if( count != 2 )
			return false;
		XrViewConfigurationView config_views[ 2 ] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
		XR_CHECK( xrEnumerateViewConfigurationViews( Instance, System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, config_views ) );
		
		XR_CHECK( xrEnumerateSwapchainFormats( Session, 0, &count, NULL ) );
		std::vector<int64_t> formats( count );
		XR_CHECK( xrEnumerateSwapchainFormats( Session, count, &count, formats.data() ) );
		
		// The engine's colors are already gamma-encoded: use sRGB only if its write conversion can be turned off.
		const char *gl_ext = (const char*) gles_GetString( GL_EXTENSIONS );
		bool srgb_write_control = gl_ext && strstr( gl_ext, "GL_EXT_sRGB_write_control" );
		int64_t format = 0;
		for( size_t i = 0; i < formats.size(); i ++ )
		{
			if( (formats[ i ] == GL_SRGB8_ALPHA8) && srgb_write_control )
				format = formats[ i ];
			else if( (formats[ i ] == GL_RGBA8) && ! format )
				format = formats[ i ];
		}
		if( ! format )
			format = formats[ 0 ];
		if( format == GL_SRGB8_ALPHA8 )
			gles_Disable( GL_FRAMEBUFFER_SRGB_EXT );
		printf( "OpenXR: swapchain format 0x%X (sRGB write control %s)\n", (unsigned int) format, srgb_write_control ? "yes" : "no" );
		
		for( int e = 0; e < 3; e ++ )
		{
			EyeSwapchain *eye = &Eyes[ e ];
			eye->W = config_views[ e % 2 ].recommendedImageRectWidth;
			eye->H = config_views[ e % 2 ].recommendedImageRectHeight;
			
			// Some runtime settings recommend heavy supersampling (e.g. 2800x2933); stay near native Quest 3 resolution.
			double scale = std::min( 1., std::min( 2064. / eye->W, 2208. / eye->H ) );
			eye->W = (int)( eye->W * scale );
			eye->H = (int)( eye->H * scale );
			
			XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			info.format = format;
			info.sampleCount = 1;
			info.width = eye->W;
			info.height = eye->H;
			info.faceCount = 1;
			info.arraySize = 1;
			info.mipCount = 1;
			XR_CHECK( xrCreateSwapchain( Session, &info, &eye->Handle ) );
			
			uint32_t image_count = 0;
			XR_CHECK( xrEnumerateSwapchainImages( eye->Handle, 0, &image_count, NULL ) );
			eye->Images.assign( image_count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR } );
			XR_CHECK( xrEnumerateSwapchainImages( eye->Handle, image_count, &image_count, (XrSwapchainImageBaseHeader*) eye->Images.data() ) );
			
			gles_GenRenderbuffers( 1, &eye->Depth );
			gles_BindRenderbuffer( GL_RENDERBUFFER, eye->Depth );
			gles_RenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eye->W, eye->H );
			gles_BindRenderbuffer( GL_RENDERBUFFER, 0 );
			
			eye->FBOs.assign( image_count, 0 );
			glGenFramebuffers( image_count, eye->FBOs.data() );
			for( uint32_t i = 0; i < image_count; i ++ )
			{
				glBindFramebuffer( GL_FRAMEBUFFER, eye->FBOs[ i ] );
				gles_FramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye->Images[ i ].image, 0 );
				gles_FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, eye->Depth );
				GLenum status = gles_CheckFramebufferStatus( GL_FRAMEBUFFER );
				if( status != GL_FRAMEBUFFER_COMPLETE )
				{
					fprintf( stderr, "OpenXR: eye %i framebuffer %i incomplete (0x%X)\n", e, i, status );
					return false;
				}
			}
			glBindFramebuffer( GL_FRAMEBUFFER, 0 );
			printf( "OpenXR: eye %i swapchain %ix%i, %u images\n", e, eye->W, eye->H, image_count );
		}
		
		*eye_w = Eyes[ 0 ].W;
		*eye_h = Eyes[ 0 ].H;
		for( int e = 0; e < 2; e ++ )
			Views[ e ] = { XR_TYPE_VIEW };
		return true;
	}
	
	
	// OpenXR (x right, y up, -z forward) to engine (X forward, Y up, Z right).
	void ToEngine( const XrVector3f &v, double *out )
	{
		out[ 0 ] = -v.z;
		out[ 1 ] = v.y;
		out[ 2 ] = v.x;
	}
	
	XrVector3f Rotate( const XrQuaternionf &q, float x, float y, float z )
	{
		// v' = v + 2w(q x v) + 2(q x (q x v))
		float tx = 2.f * (q.y * z - q.z * y);
		float ty = 2.f * (q.z * x - q.x * z);
		float tz = 2.f * (q.x * y - q.y * x);
		XrVector3f r;
		r.x = x + q.w * tx + (q.y * tz - q.z * ty);
		r.y = y + q.w * ty + (q.z * tx - q.x * tz);
		r.z = z + q.w * tz + (q.x * ty - q.y * tx);
		return r;
	}
	
	void PoseToView( const XrPosef &pose, XR::EyeView *view )
	{
		ToEngine( pose.position, view->Pos );
		ToEngine( Rotate( pose.orientation, 0.f, 0.f, -1.f ), view->Fwd );
		ToEngine( Rotate( pose.orientation, 0.f, 1.f, 0.f ), view->Up );
	}
	
	
	void DoRecenter( void )
	{
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if( XR_FAILED( xrLocateSpace( ViewSpace, BaseSpace, DisplayTime, &loc ) ) )
			return;
		const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if( (loc.locationFlags & needed) != needed )
			return;
		
		// Keep the head position and yaw only, so the horizon stays level.
		XrVector3f fwd = Rotate( loc.pose.orientation, 0.f, 0.f, -1.f );
		float yaw = atan2f( -fwd.x, -fwd.z );
		XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		space.poseInReferenceSpace.orientation.y = sinf( yaw / 2.f );
		space.poseInReferenceSpace.orientation.w = cosf( yaw / 2.f );
		space.poseInReferenceSpace.position = loc.pose.position;
		XrSpace new_space = XR_NULL_HANDLE;
		if( XR_SUCCEEDED( xrCreateReferenceSpace( Session, &space, &new_space ) ) )
		{
			if( AppSpace != XR_NULL_HANDLE )
				xrDestroySpace( AppSpace );
			AppSpace = new_space;
			RecenterPending = false;
			printf( "OpenXR: recentered (yaw %.1f)\n", yaw * 180.f / M_PI );
		}
	}
	
	
	void SyncInput( void )
	{
		memset( Hands, 0, sizeof(Hands) );
		if( State != XR_SESSION_STATE_FOCUSED )
			return;
		XrActiveActionSet active = { ActionSet, XR_NULL_PATH };
		XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		if( XR_FAILED( xrSyncActions( Session, &sync ) ) )
			return;
		
		for( int h = 0; h < 2; h ++ )
		{
			XR::HandState *hand = &Hands[ h ];
			hand->Trigger = GetFloat( ActTrigger, h );
			hand->Grip = GetFloat( ActGrip, h );
			
			XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
			info.action = ActStick;
			info.subactionPath = HandPaths[ h ];
			XrActionStateVector2f stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
			if( XR_SUCCEEDED( xrGetActionStateVector2f( Session, &info, &stick ) ) && stick.isActive )
			{
				hand->StickX = stick.currentState.x;
				hand->StickY = stick.currentState.y;
			}
			
			hand->StickClick = GetBool( ActStickClick, h );
			hand->ButtonLower = GetBool( ActLower, h );
			hand->ButtonUpper = GetBool( ActUpper, h );
			hand->Menu = (h == 0) && GetBool( ActMenu, h );
			
			XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
			if( XR_SUCCEEDED( xrLocateSpace( AimSpaces[ h ], AppSpace, DisplayTime, &loc ) )
			&&  (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) )
			{
				AimPoses[ h ] = loc.pose;
				ToEngine( loc.pose.position, hand->AimPos );
				ToEngine( Rotate( loc.pose.orientation, 0.f, 0.f, -1.f ), hand->AimFwd );
				ToEngine( Rotate( loc.pose.orientation, 0.f, 1.f, 0.f ), hand->AimUp );
				hand->AimValid = true;
			}
		}
	}
	
	
	void PollEvents( void )
	{
		XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
		while( xrPollEvent( Instance, &event ) == XR_SUCCESS )
		{
			if( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
			{
				const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged*) &event;
				State = changed->state;
				printf( "OpenXR: session state %i\n", (int) State );
				if( State == XR_SESSION_STATE_READY )
				{
					XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if( XR_SUCCEEDED( xrBeginSession( Session, &begin ) ) )
						Running = true;
					RecenterPending = true;
				}
				else if( State == XR_SESSION_STATE_STOPPING )
				{
					Running = false;
					xrEndSession( Session );
				}
				else if( (State == XR_SESSION_STATE_EXITING) || (State == XR_SESSION_STATE_LOSS_PENDING) )
				{
					Running = false;
					Exit = true;
				}
			}
			else if( event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING )
				Exit = true;
			else if( event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING )
			{
				// System recenter (hold the Meta button) moved LOCAL: drop our own offset so they don't stack.
				XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
				space.poseInReferenceSpace.orientation.w = 1.f;
				XrSpace new_space = XR_NULL_HANDLE;
				if( XR_SUCCEEDED( xrCreateReferenceSpace( Session, &space, &new_space ) ) )
				{
					xrDestroySpace( AppSpace );
					AppSpace = new_space;
				}
			}
			event = { XR_TYPE_EVENT_DATA_BUFFER };
		}
	}
}


bool XR::Init( int *eye_w, int *eye_h )
{
	if( Session != XR_NULL_HANDLE )
	{
		*eye_w = Eyes[ 0 ].W;
		*eye_h = Eyes[ 0 ].H;
		return true;
	}
	
	if( ! LoadGLES() )
	{
		fprintf( stderr, "OpenXR: can't load GLES driver entry points\n" );
		return false;
	}
	if( ! (InitInstance() && InitActions() && InitSession() && AttachActions() && InitSwapchains( eye_w, eye_h )) )
	{
		Shutdown();
		return false;
	}
	printf( "OpenXR: ready, %.0f Hz\n", DisplayRefresh );
	return true;
}


void XR::Shutdown( void )
{
	for( int e = 0; e < 3; e ++ )
	{
		EyeSwapchain *eye = &Eyes[ e ];
		if( eye->FBOs.size() )
			glDeleteFramebuffers( eye->FBOs.size(), eye->FBOs.data() );
		if( eye->Depth && gles_DeleteRenderbuffers )
			gles_DeleteRenderbuffers( 1, &eye->Depth );
		if( eye->Handle != XR_NULL_HANDLE )
			xrDestroySwapchain( eye->Handle );
		eye->FBOs.clear();
		eye->Images.clear();
		eye->Depth = 0;
		eye->Handle = XR_NULL_HANDLE;
		eye->Acquired = false;
	}
	for( int h = 0; h < 2; h ++ )
	{
		if( AimSpaces[ h ] != XR_NULL_HANDLE )
			xrDestroySpace( AimSpaces[ h ] );
		AimSpaces[ h ] = XR_NULL_HANDLE;
	}
	if( ActionSet != XR_NULL_HANDLE )
		xrDestroyActionSet( ActionSet );  // Also destroys its actions.
	ActionSet = XR_NULL_HANDLE;
	if( ViewSpace != XR_NULL_HANDLE ) xrDestroySpace( ViewSpace );
	if( AppSpace != XR_NULL_HANDLE ) xrDestroySpace( AppSpace );
	if( BaseSpace != XR_NULL_HANDLE ) xrDestroySpace( BaseSpace );
	if( Session != XR_NULL_HANDLE ) xrDestroySession( Session );
	if( Instance != XR_NULL_HANDLE ) xrDestroyInstance( Instance );
	ViewSpace = AppSpace = BaseSpace = XR_NULL_HANDLE;
	Session = XR_NULL_HANDLE;
	Instance = XR_NULL_HANDLE;
	System = XR_NULL_SYSTEM_ID;
	Running = false;
	FrameBegun = false;
	ShouldRender = false;
}


bool XR::Initialized( void )
{
	return Session != XR_NULL_HANDLE;
}


bool XR::SessionRunning( void )
{
	return Running;
}


bool XR::Focused( void )
{
	return State == XR_SESSION_STATE_FOCUSED;
}


bool XR::ExitRequested( void )
{
	return Exit;
}


float XR::RefreshRate( void )
{
	return DisplayRefresh;
}


bool XR::BeginFrame( void )
{
	if( Session == XR_NULL_HANDLE )
		return false;
	if( FrameBegun )
		EndFrame();
	
	ShouldRender = false;
	PollEvents();
	if( ! Running )
	{
		SDL_Delay( 10 );
		return false;
	}
	
	XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
	if( XR_FAILED( xrWaitFrame( Session, &wait_info, &frame_state ) ) )
		return false;
	DisplayTime = frame_state.predictedDisplayTime;
	
	XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
	if( XR_FAILED( xrBeginFrame( Session, &begin_info ) ) )
		return false;
	FrameBegun = true;
	
	if( RecenterPending )
		DoRecenter();
	SyncInput();
	
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = DisplayTime;
	locate.space = AppSpace;
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	uint32_t view_count = 2;
	bool views_valid = false;
	if( XR_SUCCEEDED( xrLocateViews( Session, &locate, &view_state, 2, &view_count, Views ) ) )
		views_valid = (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
	
	if( views_valid )
	{
		for( int e = 0; e < 2; e ++ )
		{
			PoseToView( Views[ e ].pose, &EyeViews[ e ] );
			EyeViews[ e ].TanLeft  = tan( Views[ e ].fov.angleLeft );
			EyeViews[ e ].TanRight = tan( Views[ e ].fov.angleRight );
			EyeViews[ e ].TanDown  = tan( Views[ e ].fov.angleDown );
			EyeViews[ e ].TanUp    = tan( Views[ e ].fov.angleUp );
		}
		XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
		if( XR_SUCCEEDED( xrLocateSpace( ViewSpace, AppSpace, DisplayTime, &head ) ) )
			PoseToView( head.pose, &HeadView );
	}
	
	ShouldRender = frame_state.shouldRender && views_valid;
	return ShouldRender;
}


const XR::EyeView *XR::Head( void )
{
	return &HeadView;
}


const XR::EyeView *XR::Eye( int eye )
{
	return &EyeViews[ eye ? 1 : 0 ];
}


unsigned int XR::AcquireEye( int e )
{
	EyeSwapchain *eye = &Eyes[ e ? 1 : 0 ];
	if( ! ShouldRender )
		return 0;
	
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if( XR_FAILED( xrAcquireSwapchainImage( eye->Handle, &acquire, &index ) ) )
		return 0;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	if( XR_FAILED( xrWaitSwapchainImage( eye->Handle, &wait ) ) )
		return 0;
	eye->Acquired = true;
	EyesSubmitted = true;
	
	XrCompositionLayerProjectionView *view = &LayerViews[ e ? 1 : 0 ];
	*view = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
	view->pose = Views[ e ? 1 : 0 ].pose;
	view->fov = Views[ e ? 1 : 0 ].fov;
	view->subImage.swapchain = eye->Handle;
	view->subImage.imageRect.offset.x = 0;
	view->subImage.imageRect.offset.y = 0;
	view->subImage.imageRect.extent.width = eye->W;
	view->subImage.imageRect.extent.height = eye->H;
	view->subImage.imageArrayIndex = 0;
	
	return eye->FBOs[ index ];
}


void XR::ReleaseEye( int e )
{
	EyeSwapchain *eye = &Eyes[ e ? 1 : 0 ];
	if( ! eye->Acquired )
		return;
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage( eye->Handle, &release );
	eye->Acquired = false;
}


void XR::EndFrame( void )
{
	if( ! FrameBegun )
		return;
	ReleaseEye( 0 );
	ReleaseEye( 1 );
	ReleasePanel();
	
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = AppSpace;
	layer.viewCount = 2;
	layer.views = LayerViews;
	
	EyeSwapchain *panel = &Eyes[ 2 ];
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	quad.space = AppSpace;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage.swapchain = panel->Handle;
	quad.subImage.imageRect.extent.width = panel->W;
	quad.subImage.imageRect.extent.height = panel->H;
	quad.pose.orientation.w = 1.f;
	quad.pose.position.z = - PanelDist;
	quad.size.width = PanelWidth;
	quad.size.height = PanelWidth * panel->H / (float) panel->W;
	
	// Stereo surroundings first, the menu panel (if any) on top.
	const XrCompositionLayerBaseHeader *layers[ 2 ];
	uint32_t layer_count = 0;
	if( EyesSubmitted )
		layers[ layer_count ++ ] = (const XrCompositionLayerBaseHeader*) &layer;
	if( PanelUsed )
		layers[ layer_count ++ ] = (const XrCompositionLayerBaseHeader*) &quad;
	
	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	end.displayTime = DisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = ShouldRender ? layer_count : 0;
	end.layers = (ShouldRender && layer_count) ? layers : NULL;
	xrEndFrame( Session, &end );
	
	PanelUsed = false;
	EyesSubmitted = false;
	FrameBegun = false;
	ShouldRender = false;
}


unsigned int XR::AcquirePanel( void )
{
	EyeSwapchain *panel = &Eyes[ 2 ];
	if( (! ShouldRender) || (panel->Handle == XR_NULL_HANDLE) )
		return 0;
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if( XR_FAILED( xrAcquireSwapchainImage( panel->Handle, &acquire, &index ) ) )
		return 0;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	if( XR_FAILED( xrWaitSwapchainImage( panel->Handle, &wait ) ) )
		return 0;
	panel->Acquired = true;
	PanelUsed = true;
	return panel->FBOs[ index ];
}


void XR::ReleasePanel( void )
{
	EyeSwapchain *panel = &Eyes[ 2 ];
	if( ! panel->Acquired )
		return;
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage( panel->Handle, &release );
	panel->Acquired = false;
}


bool XR::PanelPointer( int hand, int *x, int *y )
{
	hand = hand ? 1 : 0;
	if( ! Hands[ hand ].AimValid )
		return false;
	const XrPosef &pose = AimPoses[ hand ];
	XrVector3f dir = Rotate( pose.orientation, 0.f, 0.f, -1.f );
	if( dir.z > -0.05f )
		return false;
	float t = (- PanelDist - pose.position.z) / dir.z;
	if( t <= 0.f )
		return false;
	float hit_x = pose.position.x + dir.x * t;
	float hit_y = pose.position.y + dir.y * t;
	const EyeSwapchain *panel = &Eyes[ 2 ];
	float panel_h = PanelWidth * panel->H / (float) panel->W;
	*x = (int)( (hit_x / PanelWidth + 0.5f) * panel->W );
	*y = (int)( (0.5f - hit_y / panel_h) * panel->H );
	return true;
}


const XR::HandState *XR::Hand( int hand )
{
	return &Hands[ hand ? 1 : 0 ];
}


void XR::Vibrate( int hand, float amplitude, float seconds )
{
	if( (Session == XR_NULL_HANDLE) || (State != XR_SESSION_STATE_FOCUSED) )
		return;
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = amplitude;
	vibration.duration = (XrDuration)( seconds * 1e9 );
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = ActHaptic;
	info.subactionPath = HandPaths[ hand ? 1 : 0 ];
	xrApplyHapticFeedback( Session, &info, (const XrHapticBaseHeader*) &vibration );
}


void XR::Recenter( void )
{
	RecenterPending = true;
}

#endif

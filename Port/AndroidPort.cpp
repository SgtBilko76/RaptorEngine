/*
 *  AndroidPort.cpp
 *  Meta Quest (Android) platform glue for RaptorEngine.
 */

#ifdef __ANDROID__

#include "AndroidPort.h"

#include <SDL2/SDL.h>
#include <android/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include "RaptorGame.h"


static int LogPipe[ 2 ] = { -1, -1 };

static void *LogThread( void *arg )
{
	// Forward everything written to stdout/stderr to logcat, one line at a time.
	char buffer[ 1024 ];
	size_t used = 0;
	for(;;)
	{
		ssize_t got = read( LogPipe[ 0 ], buffer + used, sizeof(buffer) - 1 - used );
		if( got <= 0 )
			break;
		used += got;
		buffer[ used ] = '\0';
		char *start = buffer;
		char *newline = NULL;
		while( (newline = strchr( start, '\n' )) )
		{
			*newline = '\0';
			__android_log_write( ANDROID_LOG_INFO, "XWingRevival", start );
			start = newline + 1;
		}
		used = strlen( start );
		if( used >= sizeof(buffer) - 1 )
		{
			__android_log_write( ANDROID_LOG_INFO, "XWingRevival", start );
			used = 0;
		}
		else
			memmove( buffer, start, used );
	}
	return NULL;
}


static bool IsDir( const std::string &path )
{
	struct stat st;
	return (stat( path.c_str(), &st ) == 0) && S_ISDIR( st.st_mode );
}


std::string Raptor::AndroidDataDir( void )
{
	// A plain folder on the headset storage, easy to fill from a PC (needs "All files access").
	return "/sdcard/XWingRevival";
}


bool Raptor::AndroidSetup( void )
{
	setvbuf( stdout, NULL, _IOLBF, 0 );
	setvbuf( stderr, NULL, _IONBF, 0 );
	if( pipe( LogPipe ) == 0 )
	{
		dup2( LogPipe[ 1 ], STDOUT_FILENO );
		dup2( LogPipe[ 1 ], STDERR_FILENO );
		pthread_t thread;
		if( pthread_create( &thread, NULL, LogThread, NULL ) == 0 )
			pthread_detach( thread );
	}
	
	std::string dir = AndroidDataDir();
	mkdir( dir.c_str(), 0775 );
	chdir( dir.c_str() );
	printf( "Data folder: %s\n", dir.c_str() );
	
	return IsDir( dir + "/Models" ) && IsDir( dir + "/Textures" ) && IsDir( dir + "/Sounds" );
}


void Raptor::AndroidMainFBSize( int *w, int *h )
{
	*w = 0;
	*h = 0;
	if( Raptor::Game && Raptor::Game->Gfx.Screen )
		SDL_GL_GetDrawableSize( Raptor::Game->Gfx.Screen, w, h );
	if( (*w <= 0) || (*h <= 0) )
	{
		*w = 1920;
		*h = 1080;
	}
}

#endif

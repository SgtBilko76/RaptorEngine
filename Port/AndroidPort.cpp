/*
 *  AndroidPort.cpp
 *  Meta Quest (Android) platform glue for RaptorEngine.
 */

#ifdef __ANDROID__

#include "AndroidPort.h"

#include <SDL2/SDL.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <pthread.h>
#include <errno.h>
#include <cstdlib>
#include <vector>
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


static jobject GetAssetManager( AAssetManager **assets )
{
	// Activity.getAssets() through SDL's JNI environment; returns a global ref the caller deletes.
	*assets = NULL;
	JNIEnv *env = (JNIEnv*) SDL_AndroidGetJNIEnv();
	jobject activity = (jobject) SDL_AndroidGetActivity();
	if( ! env || ! activity )
		return NULL;
	jclass cls = env->GetObjectClass( activity );
	jmethodID get_assets = env->GetMethodID( cls, "getAssets", "()Landroid/content/res/AssetManager;" );
	jobject local = get_assets ? env->CallObjectMethod( activity, get_assets ) : NULL;
	env->DeleteLocalRef( cls );
	env->DeleteLocalRef( activity );
	if( env->ExceptionCheck() )
		env->ExceptionClear();
	if( ! local )
		return NULL;
	jobject global = env->NewGlobalRef( local );
	env->DeleteLocalRef( local );
	*assets = AAssetManager_fromJava( env, global );
	return global;
}


static bool MakeDirs( const std::string &path )
{
	for( size_t i = 1; i <= path.size(); i ++ )
	{
		if( (i == path.size()) || (path[ i ] == '/') )
		{
			std::string part = path.substr( 0, i );
			if( (mkdir( part.c_str(), 0775 ) != 0) && (errno != EEXIST) )
				return false;
		}
	}
	return true;
}


static bool WaitForWriteAccess( const std::string &dir )
{
	// The "All files access" screen opens on first start; wait until the player allows it.
	std::string probe = dir + "/.write_test";
	for( int tries = 0; tries < 600; tries ++ )
	{
		MakeDirs( dir );
		FILE *f = fopen( probe.c_str(), "wb" );
		if( f )
		{
			fclose( f );
			unlink( probe.c_str() );
			return true;
		}
		if( tries == 0 )
			printf( "Waiting for storage access to %s ...\n", dir.c_str() );
		usleep( 500 * 1000 );
	}
	return false;
}


static bool ExtractBundledData( AAssetManager *assets, const std::string &list, const std::string &dir )
{
	// gamedata.idx: one "<size>\t<path>" line per file, path relative to assets/gamedata/.
	size_t files = 0, written = 0, failed = 0;
	long long bytes = 0;
	std::vector<char> buffer( 1024 * 1024 );
	size_t pos = 0;
	while( pos < list.size() )
	{
		size_t end = list.find( '\n', pos );
		if( end == std::string::npos )
			end = list.size();
		std::string line = list.substr( pos, end - pos );
		pos = end + 1;
		if( (! line.empty()) && (line[ line.size() - 1 ] == '\r') )
			line.erase( line.size() - 1 );
		size_t tab = line.find( '\t' );
		if( tab == std::string::npos )
			continue;
		long long size = atoll( line.substr( 0, tab ).c_str() );
		std::string rel = line.substr( tab + 1 );
		files ++;
		
		std::string out = dir + "/" + rel;
		struct stat st;
		if( (stat( out.c_str(), &st ) == 0) && S_ISREG( st.st_mode ) && (st.st_size == size) )
			continue;
		
		if( ! written )
			printf( "Unpacking bundled game data to %s ...\n", dir.c_str() );
		
		size_t slash = out.rfind( '/' );
		MakeDirs( out.substr( 0, slash ) );
		AAsset *in = AAssetManager_open( assets, ("gamedata/" + rel).c_str(), AASSET_MODE_STREAMING );
		std::string tmp = out + ".part";
		FILE *f = in ? fopen( tmp.c_str(), "wb" ) : NULL;
		bool ok = (in && f);
		if( ok )
		{
			int got = 0;
			while( (got = AAsset_read( in, &buffer[ 0 ], buffer.size() )) > 0 )
			{
				if( fwrite( &buffer[ 0 ], 1, got, f ) != (size_t) got )
				{
					ok = false;
					break;
				}
				bytes += got;
			}
			if( got < 0 )
				ok = false;
		}
		if( f && (fclose( f ) != 0) )
			ok = false;
		if( in )
			AAsset_close( in );
		if( ok && (rename( tmp.c_str(), out.c_str() ) == 0) )
			written ++;
		else
		{
			unlink( tmp.c_str() );
			failed ++;
			printf( "Could not unpack %s\n", rel.c_str() );
		}
	}
	printf( "Bundled game data: %zu files, %zu unpacked (%lld MB), %zu failed.\n", files, written, bytes / (1024 * 1024), failed );
	return (files > 0) && (failed == 0);
}


static std::string IndexStamp( const std::string &list )
{
	// FNV-1a of the index: changes whenever a bundled file is added, removed or resized.
	unsigned long long h = 1469598103934665603ULL;
	for( size_t i = 0; i < list.size(); i ++ )
		h = (h ^ (unsigned char) list[ i ]) * 1099511628211ULL;
	char buf[ 32 ];
	snprintf( buf, sizeof(buf), "%016llx", h );
	return buf;
}


static void UnpackIfNeeded( AAssetManager *assets, const std::string &list, const std::string &dir )
{
	// Skip the per-file check once this exact data set has been unpacked completely.
	std::string stamp = IndexStamp( list ), marker = dir + "/.gamedata", old;
	FILE *f = fopen( marker.c_str(), "rb" );
	if( f )
	{
		char buf[ 64 ] = "";
		size_t got = fread( buf, 1, sizeof(buf) - 1, f );
		buf[ got ] = '\0';
		old = buf;
		fclose( f );
	}
	if( old == stamp )
		return;
	if( ExtractBundledData( assets, list, dir ) && (f = fopen( marker.c_str(), "wb" )) )
	{
		fputs( stamp.c_str(), f );
		fclose( f );
	}
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
	
	// Full APK: unpack the bundled game data into the data folder (only missing or changed files).
	AAssetManager *assets = NULL;
	jobject asset_obj = GetAssetManager( &assets );
	if( assets )
	{
		AAsset *index = AAssetManager_open( assets, "gamedata.idx", AASSET_MODE_BUFFER );
		if( index )
		{
			std::string list( (const char*) AAsset_getBuffer( index ), AAsset_getLength( index ) );
			AAsset_close( index );
			if( WaitForWriteAccess( dir ) )
				UnpackIfNeeded( assets, list, dir );
			else
				printf( "No write access to %s, bundled data not unpacked.\n", dir.c_str() );
		}
	}
	if( asset_obj )
		((JNIEnv*) SDL_AndroidGetJNIEnv())->DeleteGlobalRef( asset_obj );
	
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

/*
 *  GLUMini.cpp
 *  The three GLU functions RaptorEngine uses, for Android (gl4es has no GLU).
 */

#ifdef __ANDROID__

#include "RaptorGL.h"
#include <cmath>
#include <cstring>

extern "C" void gluPerspective( GLdouble fovy, GLdouble aspect, GLdouble z_near, GLdouble z_far )
{
	GLdouble f = 1. / tan( fovy * M_PI / 360. );
	GLdouble m[ 16 ] = { 0 };
	m[ 0 ] = f / aspect;
	m[ 5 ] = f;
	m[ 10 ] = (z_far + z_near) / (z_near - z_far);
	m[ 11 ] = -1.;
	m[ 14 ] = (2. * z_far * z_near) / (z_near - z_far);
	glMultMatrixd( m );
}

static void Normalize( GLdouble *v )
{
	GLdouble len = sqrt( v[0]*v[0] + v[1]*v[1] + v[2]*v[2] );
	if( len > 0. )
	{
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

static void Cross( const GLdouble *a, const GLdouble *b, GLdouble *out )
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

extern "C" void gluLookAt( GLdouble eye_x, GLdouble eye_y, GLdouble eye_z, GLdouble center_x, GLdouble center_y, GLdouble center_z, GLdouble up_x, GLdouble up_y, GLdouble up_z )
{
	GLdouble fwd[3] = { center_x - eye_x, center_y - eye_y, center_z - eye_z };
	GLdouble up[3] = { up_x, up_y, up_z };
	GLdouble side[3], real_up[3];
	Normalize( fwd );
	Cross( fwd, up, side );
	Normalize( side );
	Cross( side, fwd, real_up );
	
	GLdouble m[ 16 ] = {
		side[0], real_up[0], -fwd[0], 0.,
		side[1], real_up[1], -fwd[1], 0.,
		side[2], real_up[2], -fwd[2], 0.,
		0.,      0.,         0.,      1. };
	glMultMatrixd( m );
	glTranslated( -eye_x, -eye_y, -eye_z );
}

extern "C" GLint gluBuild2DMipmaps( GLenum target, GLint internal_format, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *data )
{
	glTexImage2D( target, 0, internal_format, width, height, 0, format, type, data );
	glGenerateMipmap( target );
	return 0;
}

#endif

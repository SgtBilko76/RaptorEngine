/*
 *  VirtualKeyboard.cpp
 *  On-screen keyboard for VR menus: shown under the menu whenever a text box is selected,
 *  clicked with the laser pointer. Keys become normal SDL text/key events for the text box.
 */

#include "VirtualKeyboard.h"

#include <cstring>
#include <string>
#include <vector>

#include "RaptorGame.h"
#include "TextBox.h"
#ifdef XWR_OPENXR
	#include "OpenXR.h"
#endif


namespace
{
	enum KeyAction { KEY_CHAR = 0, KEY_SHIFT, KEY_BACKSPACE, KEY_SPACE, KEY_ENTER, KEY_CLOSE };
	
	struct Key
	{
		std::string Lower, Upper;
		int Action;
		double Width;  // in key units
		int X1, Y1, X2, Y2;  // pixels, set by Layout
	};
	
	std::vector< std::vector<Key> > Rows;
	bool Shift = false;
	bool Hidden = false;            // Closed by the user until the next text box is selected.
	const void *HiddenFor = NULL;
	int PressedRow = -1, PressedKey = -1;
	int BoxX1 = 0, BoxY1 = 0, BoxX2 = 0, BoxY2 = 0;
	
	
	void AddRow( const char *lower, const char *upper )
	{
		std::vector<Key> row;
		for( size_t i = 0; lower[ i ]; i ++ )
		{
			Key key;
			key.Lower = std::string( 1, lower[ i ] );
			key.Upper = std::string( 1, upper[ i ] );
			key.Action = KEY_CHAR;
			key.Width = 1.;
			row.push_back( key );
		}
		Rows.push_back( row );
	}
	
	void AddSpecial( std::vector<Key> *row, const char *label, int action, double width )
	{
		Key key;
		key.Lower = key.Upper = label;
		key.Action = action;
		key.Width = width;
		row->push_back( key );
	}
	
	void Build( void )
	{
		if( Rows.size() )
			return;
		AddRow( "1234567890-", "!@#$%^&*()_" );
		AddRow( "qwertyuiop'", "QWERTYUIOP\"" );
		AddRow( "asdfghjkl:.", "ASDFGHJKL;," );
		AddRow( "zxcvbnm/[]=", "ZXCVBNM?{}+" );
		Rows[ 3 ].insert( Rows[ 3 ].begin(), Key() );
		Rows[ 3 ][ 0 ].Lower = Rows[ 3 ][ 0 ].Upper = "Shift";
		Rows[ 3 ][ 0 ].Action = KEY_SHIFT;
		Rows[ 3 ][ 0 ].Width = 1.5;
		AddSpecial( &(Rows[ 0 ]), "Back", KEY_BACKSPACE, 1.5 );
		std::vector<Key> bottom;
		AddSpecial( &bottom, "Hide", KEY_CLOSE, 2. );
		AddSpecial( &bottom, "Space", KEY_SPACE, 6. );
		AddSpecial( &bottom, "Enter", KEY_ENTER, 2.5 );
		Rows.push_back( bottom );
	}
	
	
	// The selected text box, if any (searched through the layer stack).
	TextBox *FindSelected( Layer *layer )
	{
		if( ! layer || ! layer->Visible )
			return NULL;
		if( layer->Selected && layer->Selected->Visible )
		{
			// Skip always-focused chat inputs (e.g. the lobby chat line), they never get a keyboard.
			TextBox *box = dynamic_cast<TextBox*>( layer->Selected );
			if( box && (box->ReturnDeselects || box->EscDeselects) )
				return box;
			if( TextBox *inner = FindSelected( layer->Selected ) )
				return inner;
		}
		for( std::list<Layer*>::iterator iter = layer->Elements.begin(); iter != layer->Elements.end(); iter ++ )
			if( TextBox *box = FindSelected( *iter ) )
				return box;
		return NULL;
	}
	
	TextBox *SelectedTextBox( void )
	{
		std::list<Layer*> *layers = &(Raptor::Game->Layers.Layers);
		for( std::list<Layer*>::reverse_iterator iter = layers->rbegin(); iter != layers->rend(); iter ++ )
			if( TextBox *box = FindSelected( *iter ) )
				return box;
		return NULL;
	}
	
	
	void Layout( void )
	{
		Build();
		double scale = Raptor::Game->UIScale;
		int unit = (int)( 60. * scale ), gap = (int)( 6. * scale );
		double widest = 0.;
		for( size_t r = 0; r < Rows.size(); r ++ )
		{
			double w = 0.;
			for( size_t k = 0; k < Rows[ r ].size(); k ++ )
				w += Rows[ r ][ k ].Width;
			if( w > widest )
				widest = w;
		}
		int total_w = (int)( widest * unit ) + gap * 2;
		int total_h = (int)Rows.size() * unit + gap * 2;
		
		// Under the 640x480 menu area in the middle of the screen.
		BoxX1 = Raptor::Game->Gfx.W / 2 - total_w / 2;
		BoxX2 = BoxX1 + total_w;
		BoxY1 = Raptor::Game->Gfx.H / 2 + (int)( 250. * scale );
		BoxY2 = BoxY1 + total_h;
		
		for( size_t r = 0; r < Rows.size(); r ++ )
		{
			double row_w = 0.;
			for( size_t k = 0; k < Rows[ r ].size(); k ++ )
				row_w += Rows[ r ][ k ].Width;
			double x = BoxX1 + gap + (widest - row_w) * unit / 2.;
			int y = BoxY1 + gap + (int) r * unit;
			for( size_t k = 0; k < Rows[ r ].size(); k ++ )
			{
				Key *key = &(Rows[ r ][ k ]);
				key->X1 = (int) x + gap / 2;
				key->X2 = (int)( x + key->Width * unit ) - gap / 2;
				key->Y1 = y + gap / 2;
				key->Y2 = y + unit - gap / 2;
				x += key->Width * unit;
			}
		}
	}
	
	
	Key *KeyAt( int x, int y, int *row_out = NULL, int *key_out = NULL )
	{
		for( size_t r = 0; r < Rows.size(); r ++ )
			for( size_t k = 0; k < Rows[ r ].size(); k ++ )
			{
				Key *key = &(Rows[ r ][ k ]);
				if( (x >= key->X1) && (x <= key->X2) && (y >= key->Y1) && (y <= key->Y2) )
				{
					if( row_out ) *row_out = r;
					if( key_out ) *key_out = k;
					return key;
				}
			}
		return NULL;
	}
	
	
	void PushKey( SDL_Keycode sym, SDL_Scancode scancode )
	{
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = SDL_KEYDOWN;
		event.key.state = SDL_PRESSED;
		event.key.keysym.sym = sym;
		event.key.keysym.scancode = scancode;
		SDL_PushEvent( &event );
		event.type = SDL_KEYUP;
		event.key.state = SDL_RELEASED;
		SDL_PushEvent( &event );
	}
	
	void PushText( const std::string &text )
	{
		// SDL on Android disables text events unless its own (unusable in VR) screen keyboard is open.
		SDL_EventState( SDL_TEXTINPUT, SDL_ENABLE );
		SDL_Event event;
		memset( &event, 0, sizeof(event) );
		event.type = SDL_TEXTINPUT;
		strncpy( event.text.text, text.c_str(), sizeof(event.text.text) - 1 );
		SDL_PushEvent( &event );
	}
	
	
	void Press( Key *key )
	{
		#ifdef XWR_OPENXR
			XR::Vibrate( 1, 0.2f, 0.015f );
		#endif
		switch( key->Action )
		{
			case KEY_CHAR:
				PushText( Shift ? key->Upper : key->Lower );
				Shift = false;
				break;
			case KEY_SHIFT:
				Shift = ! Shift;
				break;
			case KEY_BACKSPACE:
				PushKey( SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE );
				break;
			case KEY_SPACE:
				PushText( " " );
				break;
			case KEY_ENTER:
				PushKey( SDLK_RETURN, SDL_SCANCODE_RETURN );
				break;
			case KEY_CLOSE:
				Hidden = true;
				HiddenFor = SelectedTextBox();
				break;
		}
	}
}


bool VirtualKeyboard::Visible( void )
{
	if( ! (Raptor::Game->Head.VR && Raptor::Game->Mouse.ShowCursor) )
		return false;
	TextBox *box = SelectedTextBox();
	if( ! box )
	{
		Hidden = false;
		HiddenFor = NULL;
		return false;
	}
	if( Hidden && (box == HiddenFor) )
		return false;
	Hidden = false;
	return true;
}


void VirtualKeyboard::Draw( void )
{
	if( ! Visible() )
	{
		BoxX1 = BoxX2 = 0;
		return;
	}
	Layout();
	
	Font *font = Raptor::Game->Res.GetFont( "Verdana.ttf", 22 );
	float scale = Raptor::Game->UIScale;
	Raptor::Game->Gfx.Setup2D();
	Raptor::Game->Gfx.DrawRect2D( BoxX1, BoxY1, BoxX2, BoxY2, 0, 0.f, 0.f, 0.f, 0.8f );
	Raptor::Game->Gfx.DrawBox2D( BoxX1, BoxY1, BoxX2, BoxY2, 1.5f * scale, 1.f, 1.f, 1.f, 0.5f );
	
	int mx = Raptor::Game->Mouse.X, my = Raptor::Game->Mouse.Y;
	for( size_t r = 0; r < Rows.size(); r ++ )
		for( size_t k = 0; k < Rows[ r ].size(); k ++ )
		{
			Key *key = &(Rows[ r ][ k ]);
			bool hover = (mx >= key->X1) && (mx <= key->X2) && (my >= key->Y1) && (my <= key->Y2);
			bool pressed = ((int) r == PressedRow) && ((int) k == PressedKey);
			bool lit = pressed || ((key->Action == KEY_SHIFT) && Shift);
			float bg = lit ? 0.6f : (hover ? 0.35f : 0.15f);
			Raptor::Game->Gfx.DrawRect2D( key->X1, key->Y1, key->X2, key->Y2, 0, bg, bg, bg + 0.1f, 0.9f );
			Raptor::Game->Gfx.DrawBox2D( key->X1, key->Y1, key->X2, key->Y2, 1.f * scale, 0.f, 1.f, 1.f, hover ? 1.f : 0.4f );
			const std::string &label = Shift ? key->Upper : key->Lower;
			font->DrawText( label, (key->X1 + key->X2) / 2, (key->Y1 + key->Y2) / 2, Font::ALIGN_MIDDLE_CENTER, 1.f, 1.f, 1.f, 1.f, scale );
		}
}


bool VirtualKeyboard::HandleEvent( SDL_Event *event )
{
	if( (event->type != SDL_MOUSEBUTTONDOWN) && (event->type != SDL_MOUSEBUTTONUP) )
		return false;
	if( event->button.button != SDL_BUTTON_LEFT )
		return false;
	if( ! Visible() || (BoxX2 <= BoxX1) )
	{
		PressedRow = PressedKey = -1;
		return false;
	}
	// Uses the layout from the last Draw: Gfx.W/H only match the menu panel while it is being drawn.
	
	int x = event->button.x, y = event->button.y;
	bool inside = (x >= BoxX1) && (x <= BoxX2) && (y >= BoxY1) && (y <= BoxY2);
	
	if( event->type == SDL_MOUSEBUTTONDOWN )
	{
		PressedRow = PressedKey = -1;
		if( ! inside )
			return false;
		KeyAt( x, y, &PressedRow, &PressedKey );
		return true;  // Keep clicks on the keyboard away from the menu (they would deselect the text box).
	}
	
	// Button up.
	if( PressedRow >= 0 )
	{
		int row = -1, index = -1;
		Key *key = KeyAt( x, y, &row, &index );
		if( key && (row == PressedRow) && (index == PressedKey) )
			Press( key );
		PressedRow = PressedKey = -1;
		return true;
	}
	return inside;
}

/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "DeviceContext.h"
#include "Window.h"
#include "UserInterfaceLocal.h"
#include "PathTraceMenu.h"

/*

The path tracer's settings, as a panel of the game's menus.

The game's main menu is a file of its data, which is not part of this source
and cannot be given out changed. So the panel is not in a file: it is written
here, in the language the menus are written in, from the table below, and
handed to the parser in one of two ways:

  in the main menu: as the menu is read, the panel and a button for it are
  added to the end of its desktop. The button sits under the menu's own
  Controls, Game Options, System and Defaults, and is there when they are.
  The panel lies over whichever of the menu's own is open. PT_MenuFrame keeps
  the two in step with the menu, which knows nothing of them.

  on its own (guis/pathtrace.gui, which is no file): the panel alone, over
  the running game, so that a change is seen as it is made. See the
  pathTraceMenu command.

It uses the pictures of the menu's own rows and sliders, by name.

*/

typedef struct {
	const char *	label;
	const char *	cvar;
	const char *	choices;		// for a choice, else NULL: it is a slider
	float			low, high, step;
} ptMenuRow_t;

static const ptMenuRow_t ptMenuRows[] = {
	{ "Path Tracing",			"r_pathTrace",		"Off;On" },
	{ "Quality",				"r_ptPreset",		"Custom;Low;Medium;High;Ultra" },
	{ "Render Scale",			"r_ptScale",		NULL,	0.25f,	1.0f,	0.05f },
	{ "Smooth Edges",			"r_ptAntialias",	"Off;On" },
	{ "Sharpen",				"r_ptSharpen",		NULL,	0.0f,	1.0f,	0.05f },
	{ "Paths per Pixel",		"r_ptSamples",		NULL,	1.0f,	8.0f,	1.0f },
	{ "Bounces",				"r_ptBounces",		NULL,	1.0f,	8.0f,	1.0f },
	{ "Lights Weighed",			"r_ptLightSamples",	NULL,	1.0f,	32.0f,	1.0f },
	{ "Noise Filter",			"r_ptDenoise",		NULL,	0.0f,	4.0f,	1.0f },
	{ "Exposure",				"r_ptExposure",		NULL,	0.25f,	4.0f,	0.05f },
	{ "Auto Exposure",			"r_ptAutoExposure",	"Off;On" },
	{ "Light Brightness",		"r_ptLightScale",	NULL,	0.25f,	4.0f,	0.05f },
	{ "Specular",				"r_ptSpecular",		NULL,	0.0f,	4.0f,	0.1f },
	{ "Roughness",				"r_ptGloss",		NULL,	0.05f,	1.0f,	0.05f },
	{ "Sky Brightness",			"r_ptSky",			NULL,	0.0f,	2.0f,	0.05f },
	{ "Glass",					"r_ptGlass",		"Off;On" },
	{ "Refraction",				"r_ptRefraction",	"Off;On" },
	{ "Screens and Particles",	"r_ptOverlay",		"Off;On" },
};
static const int ptMenuNumRows = sizeof( ptMenuRows ) / sizeof( ptMenuRows[0] );

// What Reset to Defaults puts back besides the rows: the rest of the path
// tracer's settings. Whether it is on is left as it is.
static const char *ptMenuAlsoReset[] = {
	"r_ptFireflyClamp", "r_ptGlow", "r_ptShadowFaces", "r_ptMaterials", "r_ptFilter", "r_ptHistory",
	"r_ptTonemap", "r_ptContrast", "r_ptSaturation", "r_ptGlassTint", "r_ptEntities", "r_ptView",
};

/*
================
PT_MenuPanel

The panel: a window named OptionsPathTrace, where the main menu's own panels
are.
================
*/
static void PT_MenuPanel( idStr &s, bool visible, bool closeButton ) {
	s += "windowDef OptionsPathTrace {\n"
		"	rect 165, 30, 390, 370\n"
		"	backcolor 0.02, 0.04, 0.07, 0.96\n"
		"	bordercolor 0.6, 0.8, 1, 0.3\n"
		"	bordersize 1\n";
	s += va( "	visible %d\n", visible ? 1 : 0 );
	s += "	windowDef PTTitleText {\n"
		"		rect 20, 5, 320, 20\n"
		"		text \"PATH TRACING\"\n"
		"		textscale 0.32\n"
		"		textaligny -3\n"
		"		forecolor 1, 1, 1, 1\n"
		"		noevents 1\n"
		"	}\n"
		"	windowDef PTTitleBar {\n"
		"		rect 5, 23, 375, 2\n"
		"		backcolor 0.6, 0.8, 1, 0.2\n"
		"		noevents 1\n"
		"	}\n";

	for ( int i = 0; i < ptMenuNumRows; i++ ) {
		const ptMenuRow_t &row = ptMenuRows[i];

		s += va( "	windowDef PT%d {\n		rect 20, %d, 350, 16\n", i, 29 + i * 17 );
		s += va( "		windowDef PT%dL { rect 0, 0, 8, 16\n background \"gui/mainmenu/select_edge\"\n matcolor 0, 0, 0, 1\n noevents 1\n }\n", i );
		s += va( "		windowDef PT%dM { rect 8, 0, 334, 16\n background \"gui/mainmenu/select_border\"\n matcolor 0, 0, 0, 1\n noevents 1\n }\n", i );
		s += va( "		windowDef PT%dR { rect 342, 0, 8, 16\n background \"gui/mainmenu/select_angle\"\n matcolor 0, 0, 0, 1\n noevents 1\n }\n", i );
		s += va( "		windowDef PT%dTitle {\n			rect 1, 1, 180, 14\n			text \"%s\"\n", i, row.label );
		s += "			font \"fonts/bank\"\n			textscale 0.25\n			textalign 2\n			textaligny -6\n			forecolor 1, 1, 1, 1\n			noevents 1\n		}\n";
		if ( row.choices ) {
			s += va( "		choiceDef PT%dPrimary {\n			rect 210, 1, 110, 14\n			choices \"%s\"\n			cvar \"%s\"\n", i, row.choices, row.cvar );
			s += "			choiceType 0\n			font \"fonts/bank\"\n			textscale 0.25\n			textalign 1\n			textaligny -6\n			forecolor 0.6, 0.8, 0.8, 1\n		}\n";
		} else {
			s += va( "		windowDef PT%dSliderBG { rect 210, 4, 128, 16\n background \"guis/assets/mainmenu/slider_bg\"\n matcolor 0.6, 0.8, 0.8, 0.5\n noevents 1\n }\n", i );
			s += va( "		sliderDef PT%dPrimary {\n			rect 210, 5, 71, 8\n			forecolor 0.8, 1, 1, 1\n			matcolor 1, 1, 1, 1\n", i );
			s += va( "			low %g\n			high %g\n			step %g\n", row.low, row.high, row.step );
			s += va( "			thumbShader \"guis/assets/mainmenu/slider_bar\"\n			cvar \"%s\"\n		}\n", row.cvar );
		}
		s += "	}\n";
	}

	s += "	windowDef PTNote {\n"
		"		rect 20, 337, 350, 14\n"
		"		text \"Changes are kept. Quality sets the four rows under it.\"\n"
		"		font \"fonts/bank\"\n"
		"		textscale 0.2\n"
		"		textalign 1\n"
		"		forecolor 0.6, 0.8, 0.8, 1\n"
		"		noevents 1\n"
		"	}\n";
	s += va( "	windowDef PTReset {\n		rect %d, 351, 170, 16\n", closeButton ? 30 : 110 );
	s += "		text \"RESET TO DEFAULTS\"\n"
		"		textscale 0.26\n"
		"		textalign 1\n"
		"		textaligny -3\n"
		"		forecolor 0.6, 1, 1, 1\n"
		"		onMouseEnter { transition \"forecolor\" \"0.6 1 1 1\" \"1 1 1 1\" \"250\" ; }\n"
		"		onMouseExit { transition \"forecolor\" \"1 1 1 1\" \"0.6 1 1 1\" \"250\" ; }\n"
		"		onAction { set \"cmd\" \"" PT_MENU_RESET "\" ; }\n"
		"	}\n";
	if ( closeButton ) {
		s += "	windowDef PTClose {\n"
			"		rect 230, 351, 120, 16\n"
			"		text \"CLOSE\"\n"
			"		textscale 0.26\n"
			"		textalign 1\n"
			"		textaligny -3\n"
			"		forecolor 0.6, 1, 1, 1\n"
			"		onMouseEnter { transition \"forecolor\" \"0.6 1 1 1\" \"1 1 1 1\" \"250\" ; }\n"
			"		onMouseExit { transition \"forecolor\" \"1 1 1 1\" \"0.6 1 1 1\" \"250\" ; }\n"
			"		onAction { set \"cmd\" \"close\" ; }\n"
			"	}\n";
	}
	s += "}\n";
}

/*
================
PT_MenuForMainMenu

What is added to the end of the main menu's desktop.
================
*/
void PT_MenuForMainMenu( idStr &s ) {
	s += "\n"
		"windowDef PathTraceBtnText {\n"
		"	rect 3, 432, 124, 14\n"
		"	backcolor 0.45, 0.55, 0.7, 0.3\n"
		"	bordercolor 0.55, 0.65, 0.75, 0.5\n"
		"	bordersize 1\n"
		"	text \"PATH TRACING\"\n"
		"	textscale 0.25\n"
		"	textalignx 2\n"
		"	textaligny -4\n"
		"	forecolor 0.6, 1, 1, 1\n"
		"	visible 0\n"
		"	onMouseEnter { transition \"forecolor\" \"0.6 1 1 1\" \"1 1 1 1\" \"250\" ; }\n"
		"	onMouseExit { transition \"forecolor\" \"1 1 1 1\" \"0.6 1 1 1\" \"250\" ; }\n"
		"	onAction {\n"
		"		set \"cmd\" \"play guisounds_menuclickup\" ;\n"
		"		set \"gui::pt_click\" \"1\" ;\n"
		"	}\n"
		"}\n";
	PT_MenuPanel( s, false, false );
}

/*
================
PT_MenuAlone

The whole of guis/pathtrace.gui.
================
*/
void PT_MenuAlone( idStr &s ) {
	s = "windowDef Desktop {\n"
		"	rect 0, 0, 640, 480\n"
		"	backcolor 0, 0, 0, 0\n"
		"	menugui 1\n"
		"	onESC { set \"cmd\" \"close\" ; }\n";
	PT_MenuPanel( s, true, true );
	s += "}\n";
}

/*
================
PT_MenuReset

Reset to Defaults: all of the path tracer's settings but whether it is on.
================
*/
void PT_MenuReset( void ) {
	for ( int i = 0; i < ptMenuNumRows; i++ ) {
		if ( idStr::Icmp( ptMenuRows[i].cvar, "r_pathTrace" ) && idStr::Icmp( ptMenuRows[i].cvar, "r_ptPreset" ) ) {
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "reset %s\n", ptMenuRows[i].cvar ) );
		}
	}
	for ( int i = 0; i < sizeof( ptMenuAlsoReset ) / sizeof( ptMenuAlsoReset[0] ); i++ ) {
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "reset %s\n", ptMenuAlsoReset[i] ) );
	}
	// (a reset alone does not have the settings saved again)
	cvarSystem->SetModifiedFlags( CVAR_ARCHIVE );
}

/*
================
PT_MenuFrame

Each frame the main menu is drawn: the button is there when the menu's own
options buttons are, and the panel is open from a click on the button until
another, or until the menu moves on to another of its panels or away.
================
*/
void PT_MenuFrame( idUserInterfaceLocal *gui ) {
	static bool		open;
	static float	openAt;

	idWindow *desktop = gui->GetDesktop();
	if ( !desktop ) {
		return;
	}

	drawWin_t *button = desktop->FindChildByName( "PathTraceBtnText" );
	drawWin_t *panel = desktop->FindChildByName( "OptionsPathTrace" );
	drawWin_t *beside = desktop->FindChildByName( "ResetDefaultsBtnText" );
	drawWin_t *parent = desktop->FindChildByName( "OptionsMenuParent" );
	if ( !button || !button->win || !panel || !panel->win || !beside || !beside->win ) {
		return;
	}

	// which of its own panels the menu has open
	float selected = 0.0f;
	if ( parent && parent->win ) {
		idWinVar *var = parent->win->GetWinVarByName( "optionsSelected" );
		if ( var ) {
			selected = atof( var->c_str() );
		}
	}
	idWinBool *besideVisible = (idWinBool *) beside->win->GetWinVarByName( "visible" );
	idWinBool *buttonVisible = (idWinBool *) button->win->GetWinVarByName( "visible" );
	idWinBool *panelVisible = (idWinBool *) panel->win->GetWinVarByName( "visible" );
	if ( !besideVisible || !buttonVisible || !panelVisible ) {
		return;
	}
	const bool shown = *besideVisible;

	if ( gui->State().GetInt( "pt_click" ) ) {
		gui->SetStateInt( "pt_click", 0 );
		open = !open;
		openAt = selected;
	}
	if ( open && ( !shown || selected != openAt ) ) {
		open = false;
	}
	*buttonVisible = shown;
	*panelVisible = open;
}

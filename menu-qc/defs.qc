// defs.qc -- what menu QuakeC has from the engine: FTE's menu API (its
// entry points, builtins by FTE's numbers for menus, key codes), and the one
// builtin of SoftWorld's own, isfullscreen

#pragma target fte

// the version the program tells (CMake's -DSW_VERSION)
#ifndef SW_VERSION
#define SW_VERSION "dev"
#endif

// the system globals
entity		self;
entity		other;
float		time;			// the host's realtime, as the engine calls in
float		frametime;
void		end_sys_globals;
void		end_sys_fields;

#define	TRUE	1
#define	FALSE	0

//
// builtins
//
void(string text, ...) print = #4;
void(string text, ...) dprint = #16;
void(string cmd) localcmd = #13;
float(string name) cvar = #14;
void(string name, string value) cvar_set = #15;
string(float f) ftos = #17;
float(string s) stof = #21;
float(float f) floor = #35;
float(string s) strlen = #52;
string(string s, float start, float length) substring = #54;
float(string s) tokenize = #58;
string(float n) argv = #59;
float() clientstate = #62;
void(string sample) localsound = #65;
string(string format, ...) sprintf = #627;
float(string s, float index) str2chr = #222;
__variant*(int size) memalloc = #384;
void(__variant *ptr) memfree = #385;
void(string cmd) registercommand = #352;

string(string name, optional float flags) precache_pic = #452;
float(vector pos, float char, vector scale, vector rgb, float alpha, optional float flag) drawcharacter = #454;
float(vector pos, string text, vector scale, vector rgb, float alpha, optional float flag) drawrawstring = #455;
float(vector pos, string pic, vector size, vector rgb, float alpha, optional float flag) drawpic = #456;
float(vector pos, vector size, vector rgb, float alpha, optional float flag) drawfill = #457;
float(vector pos, string text, vector scale, vector rgb, float alpha, optional float flag) drawstring = #467;
float(string text, float usecolours, optional vector fontsize) stringwidth = #468;
vector(string pic) drawgetimagesize = #460;
void(string name, int width, int height, void *pixels, optional int datasize, optional int format) r_uploadimage =
	#0:r_uploadimage;
int*(string file, __out int width, __out int height) r_readimage = #0:r_readimage;

void(float dest) setkeydest = #601;
float() getkeydest = #602;
string(float key) keynumtostring = #609;
string(string command, optional float bindmap) findkeysforcommand = #610;
string(float key, optional float bindmap) getkeybind = #342;
float(float key, string binding, optional float bindmap) setkeybind = #630;

// the clipboard (cliptype 0), FTE's by name
void(float cliptype, string text) clipboard_set = #0:clipboard_set;

// SoftWorld's: the window fills the screen
float() isfullscreen = #0:isfullscreen;

// setkeydest and getkeydest
#define	KEY_GAME		0
#define	KEY_MENU		2

//
// FTE's key codes, as m_keydown and the key builtins have them
//
#define	K_TAB			9
#define	K_ENTER			13
#define	K_ESCAPE		27
#define	K_SPACE			32
#define	K_BACKSPACE		127
#define	K_UPARROW		128
#define	K_DOWNARROW		129
#define	K_LEFTARROW		130
#define	K_RIGHTARROW	131
#define	K_DEL			148

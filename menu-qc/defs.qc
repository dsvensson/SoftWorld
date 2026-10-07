// defs.qc -- what menu QuakeC has from the engine: FTE's menu API (its
// entry points, builtins by FTE's numbers for menus, key codes), and
// SoftWorld's own builtins, by name. Strings live as long as anything refers
// to them (SoftWorld's VM collects them): none is zoned.

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
vector(string s) stov = #55;
float(float f) floor = #35;
float(string s) strlen = #52;
string(string s, float start, float length) substring = #54;
float(string s) tokenize = #58;
string(float n) argv = #59;
float() isserver = #60;
float() clientstate = #62;
void(string sample) localsound = #65;
string(string format, ...) sprintf = #627;
float(string s, float index) str2chr = #222;
__variant*(int size) memalloc = #384;
void(__variant *ptr) memfree = #385;
void(string cmd) registercommand = #352;
string(string s, ...) strcat = #53;
string(string name) cvar_string = #71;
float(string haystack, string needle, optional float offset) strstrofs = #221;
string(float c, ...) chr2str = #223;
float(string a, string b) strcasecmp = #229;
string(string s) strtolower = #480;
string(string s) strzone = #56;
void(string s) strunzone = #57;
float(string s) strlennocol = #476;
string(string s) strdecolorize = #477;
float() gettime = #67;
float(float f) fabs = #18;
float(float f) ceil = #36;
float(float f) sin = #38;
float(float a, float b, ...) min = #43;
float(float a, float b, ...) max = #44;
float(float lo, float v, float hi) bound = #45;
float(string a, string b, float n) strncasecmp = #230;
float(string s, string separator, ...) tokenizebyseparator = #479;
string(string search, string replace, string subject) strreplace = #484;
string(float pad, string s, ...) strpad = #225;
string(string name) cvar_defstring = #89;
string(string name) cvar_description = #518;

// the server list (FTE's hostcache)
float(float type) gethostcachevalue = #611;
string(float key, float hostnr) gethostcachestring = #612;
void() resethostcachemasks = #615;
void(float mask, float key, string text, float op) sethostcachemaskstring = #616;
void(float mask, float key, float number, float op) sethostcachemasknumber = #617;
void() resorthostcache = #618;
void(float key, float flags) sethostcachesort = #619;
void(optional float purge) refreshhostcache = #620;
float(float key, float hostnr) gethostcachenumber = #621;
float(string key) gethostcacheindexforkey = #622;

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

// SoftWorld's, the server list's: a server's view index (-1 not shown); a
// server asked again ahead of the rest; whether a source named a server; a
// source's name, type, location, marked, servers, state or age; its mark;
// a source added (its index) and removed; a server added by hand
float(string address) gethostcacheindexforaddress = #0:gethostcacheindexforaddress;
void(float hostnr) refreshhostcacheentry = #0:refreshhostcacheentry;
float(float hostnr, float source) hostcacheinsource = #0:hostcacheinsource;
string(float source, string field) gethostcachesource = #0:gethostcachesource;
void(float source, float marked) sethostcachesourcemark = #0:sethostcachesourcemark;
float(string type, string name, string location) addhostcachesource = #0:addhostcachesource;
void(float source) removehostcachesource = #0:removehostcachesource;
float(string address) addhostcacheserver = #0:addhostcacheserver;

// gethostcachevalue's: FTE's, and SoftWorld's from 100
#define	SLIST_HOSTCACHEVIEWCOUNT	0
#define	SLIST_HOSTCACHETOTALCOUNT	1
#define	SLIST_SCANNING				100
#define	SLIST_PINGSSENT				103
#define	SLIST_PINGSTOTAL			104
#define	SLIST_GENERATION			108
#define	SLIST_SOURCES				109
#define	SLIST_MARKEDSERVERS			113
#define	SLIST_SCANS					114

// FTE's mask tests (sethostcachemask*), and the OR of mask 512
#define	SLIST_TEST_CONTAINS			0
#define	SLIST_TEST_LESSEQUAL		2
#define	SLIST_TEST_EQUAL			4
#define	SLIST_TEST_GREATER			5
#define	SLIST_TEST_NOTEQUAL			7
#define	SLIST_TEST_NOTSTARTSWITH	9
#define	SLIST_MASK_AND				0
#define	SLIST_MASK_OR				512

// sethostcachesort's flags: FTE's, and SoftWorld's key after the others
#define	SLIST_SORT_DESCENDING		1
#define	SLIST_SORT_APPEND			8

// a server's state (SoftWorld's key "state")
#define	SL_DEAD		2		// never answered

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
#define	K_ALT			132
#define	K_CTRL			133
#define	K_SHIFT			134
#define	K_F1			135
#define	K_INS			147
#define	K_DEL			148
#define	K_PGDN			149
#define	K_PGUP			150
#define	K_HOME			151
#define	K_END			152
#define	K_MOUSE1		512
#define	K_MWHEELUP		515
#define	K_MWHEELDOWN	516

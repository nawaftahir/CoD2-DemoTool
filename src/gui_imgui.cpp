// CoD2-DemoTool — ImGui GUI front-end (CoD2 menu styled).
//
// A clean windowed app so the tool is usable without the command line: drag a demo in
// (or Browse), pick an operation, click Run, read the result. Double-click target on
// Windows; the console build (cod2-demotool.exe) is unchanged.
//
// This file IS the GUI translation unit: it #defines GUI_BUILD and #includes the whole
// tool (reader.cpp), so every Cmd_* function/global is called directly in-process.
// reader.cpp's main() is compiled out under GUI_BUILD.
//
// Backend: Win32 + OpenGL3 (cross-compiles with the dockcross mingw toolchain — no
// DirectX, no MSVC, no GLFW). Build: ./build-win-gui.sh  ->  bin/cod2-demotool-gui.exe

#define GUI_BUILD
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <GL/gl.h>

// stb_image FIRST, before reader.cpp — the CoD2 engine headers define a global `cm`
// (the clipMap_t collision map) that would otherwise shadow stb_image's local `cm`
// and break its compile.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "imgui/stb_image.h"

#include "reader.cpp"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include "gui_assets.h"   // g_logoJpg + g_robotoTtf (embedded)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND, UINT, WPARAM, LPARAM );

// fonts (loaded in WinMain), logo texture (built once on first frame)
static ImFont *g_fontBody = NULL;
static ImFont *g_fontH1   = NULL;
static GLuint  g_logoTex  = 0;
static int     g_logoW = 0, g_logoH = 0;

// ---- operations -------------------------------------------------------------------
enum { OP_INFO = 0, OP_SKIPDEAD, OP_CUT, OP_REMOVEHUD, OP_CLEAN, OP_OVERVIEW, OP_COUNT };
static const char *kOpNames[ OP_COUNT ] = {
	"Info  -  what is this demo?",
	"Skip dead-time",
	"Cut to a time range",
	"Remove HUD",
	"Clean text  (chat / prints)",
	"Match overview  (HTML report)",
};
static const char *kOpBlurb[ OP_COUNT ] = {
	"Read the demo and show its version, map, gametype, length and players. Doesn't change anything.",
	"Cuts out every death -> respawn stretch (the dead-stare, the killcam, and any spectating) and re-times the demo so the action plays back-to-back. Tick the box below to keep the kill replays.",
	"Keep only the chosen time range and play it from the start. Times are mm:ss, plain seconds, or the words start / end.",
	"Strip the server-added HUD overlays (kill cards, server logos, score pop-ups). The ammo and compass are drawn by the game, not stored in the demo, so they stay.",
	"Remove chat, the big centre prints, and/or the bottom-left console feed. Scores and gameplay are kept.",
	"Build a shareable HTML match report - the killfeed with weapon icons, chat, and the final score - and open it in your browser.",
};
static const char *kOpSuffix[ OP_COUNT ] = { "", "skipdead", "cut", "nohud", "clean", "" };

// ---- UI state ---------------------------------------------------------------------
static char  g_input[ 1024 ]   = { 0 };
static char  g_output[ 1024 ]  = { 0 };
static int   g_op              = OP_INFO;
static bool  g_keepKillcam     = false;
static char  g_cutStart[ 64 ]  = "start";
static char  g_cutEnd[ 64 ]    = "end";
static bool  g_cleanChat       = true;
static bool  g_cleanCenter     = true;
static bool  g_cleanWhite      = true;
static char  g_log[ 1 << 16 ]  = "Drag a .dm_1 demo onto the window, pick what to do, and press Run.";
static bool  g_busy            = false;

static HGLRC g_glrc = NULL;
static HDC   g_hdc  = NULL;

// ---- helpers ----------------------------------------------------------------------

// Default output path beside the input: <name>_<suffix>.dm_1 (or .html for overview).
static void RecomputeOutput( void )
{
	if ( !g_input[ 0 ] ) { g_output[ 0 ] = 0; return; }
	if ( g_op == OP_OVERVIEW ) { HtmlPathFor( g_input, g_output, sizeof( g_output ) ); return; }
	if ( g_op == OP_INFO )     { g_output[ 0 ] = 0; return; }

	Q_strncpyz( g_output, g_input, sizeof( g_output ) );
	char *dot = strrchr( g_output, '.' );
	char *sl  = strrchr( g_output, '/' );
	char *bs  = strrchr( g_output, '\\' );
	if ( bs > sl ) sl = bs;
	if ( dot && dot > sl ) *dot = 0;
	int len = (int)strlen( g_output );
	snprintf( g_output + len, sizeof( g_output ) - len, "_%s.dm_1", kOpSuffix[ g_op ] );
}

static void SetInput( const char *path )
{
	Q_strncpyz( g_input, path, sizeof( g_input ) );
	RecomputeOutput();
}

static void BrowseInput( HWND hwnd )
{
	char file[ 1024 ] = { 0 };
	OPENFILENAMEA ofn; memset( &ofn, 0, sizeof( ofn ) );
	ofn.lStructSize = sizeof( ofn );
	ofn.hwndOwner   = hwnd;
	ofn.lpstrFilter = "CoD2 demo (*.dm_1)\0*.dm_1\0All files\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof( file );
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if ( GetOpenFileNameA( &ofn ) ) SetInput( file );
}

// Read the tool's captured stdout (temp file) into the log buffer (\n stays — ImGui
// renders \n fine in a multiline text widget).
static void LoadLog( const char *path )
{
	FILE *f = fopen( path, "rb" );
	if ( !f ) { Q_strncpyz( g_log, "(no output)", sizeof( g_log ) ); return; }
	long n = (long)fread( g_log, 1, sizeof( g_log ) - 1, f );
	if ( n < 0 ) n = 0;
	g_log[ n ] = 0;
	fclose( f );
	if ( !n ) Q_strncpyz( g_log, "(done)", sizeof( g_log ) );
}

// Run the selected operation in-process, capturing stdout into the log. The GUI has no
// console, so stdout is freopen'd to a temp file for the op (no edits to the tool's
// printf sites), then read back. Full state hygiene runs first (the GUI reuses one
// process; see reader.cpp ResetFilters + the decoder bookkeeping reset).
static void RunSelected( HWND hwnd )
{
	if ( g_busy ) return;
	if ( !g_input[ 0 ] ) { Q_strncpyz( g_log, "Drag a .dm_1 demo in, or use Browse.", sizeof( g_log ) ); return; }
	if ( g_op != OP_INFO && !g_output[ 0 ] ) { Q_strncpyz( g_log, "Set an output filename first.", sizeof( g_log ) ); return; }

	char tmp[ 1024 ], tdir[ 800 ] = { 0 };
	GetTempPathA( sizeof( tdir ), tdir );
	snprintf( tmp, sizeof( tmp ), "%scod2dt_out.txt", tdir );

	g_busy = true;
	FILE *redir = freopen( tmp, "w", stdout ); (void)redir;

	ResetFilters();
	memset( &demo, 0, sizeof( demo ) );
	g_sfValid = qfalse; g_sfCur = 0; g_ovNumEvents = 0;
	g_quietLog = 1;

	int rc = 0;
	switch ( g_op )
	{
	case OP_INFO:      rc = Cmd_Info( g_input ); break;
	case OP_SKIPDEAD:  rc = Cmd_SkipDead( g_input, g_output, g_keepKillcam ? 1 : 0 ); break;
	case OP_CUT:       rc = Cmd_Cut( g_input, g_output,
	                                 g_cutStart[ 0 ] ? g_cutStart : "start",
	                                 g_cutEnd[ 0 ]   ? g_cutEnd   : "end" ); break;
	case OP_REMOVEHUD: g_removeHud = 1; rc = Cmd_Copy( g_input, g_output ); break;
	case OP_CLEAN:     g_removeChat = g_cleanChat; g_removeCenterText = g_cleanCenter;
	                   g_removeWhiteText = g_cleanWhite; rc = Cmd_Copy( g_input, g_output ); break;
	case OP_OVERVIEW:  rc = Cmd_Overview( g_input, g_output ); break;
	}

	if ( rc != 0 )                            printf( "\n[failed] could not run this on the demo.\n" );
	else if ( g_op != OP_INFO && g_output[0] ) printf( "\n[done] wrote %s\n", g_output );
	fflush( stdout );
	freopen( "NUL", "w", stdout );

	LoadLog( tmp );
	DeleteFileA( tmp );

	if ( g_op == OP_OVERVIEW && rc == 0 && g_output[ 0 ] )
		ShellExecuteA( hwnd, "open", g_output, NULL, NULL, SW_SHOWNORMAL );

	g_busy = false;
}

// ---- CoD2 menu theme --------------------------------------------------------------
// Palette lifted from the stock CoD2 menu macros: amber/gold accent (.50 .45 0), dark
// blue-grey panels (.1747 .1903 .2335), near-black backgrounds, off-white text.
static void ApplyCoD2Theme( void )
{
	ImGuiStyle &s = ImGui::GetStyle();
	s.WindowRounding   = 0.0f;
	s.FrameRounding    = 2.0f;
	s.GrabRounding     = 2.0f;
	s.WindowBorderSize = 0.0f;
	s.FrameBorderSize  = 1.0f;
	s.WindowPadding    = ImVec2( 16, 14 );
	s.FramePadding     = ImVec2( 8, 6 );
	s.ItemSpacing      = ImVec2( 10, 10 );

	ImVec4 *c = s.Colors;
	const ImVec4 amber   = ImVec4( 0.78f, 0.62f, 0.12f, 1.00f );  // CoD2 gold accent
	const ImVec4 amberHi = ImVec4( 0.95f, 0.78f, 0.22f, 1.00f );
	const ImVec4 panel   = ImVec4( 0.105f, 0.115f, 0.140f, 1.0f );
	const ImVec4 panelHi = ImVec4( 0.175f, 0.190f, 0.234f, 1.0f );
	const ImVec4 bg      = ImVec4( 0.055f, 0.060f, 0.070f, 1.0f );
	const ImVec4 text    = ImVec4( 0.90f, 0.90f, 0.88f, 1.0f );

	c[ ImGuiCol_WindowBg ]        = bg;
	c[ ImGuiCol_ChildBg ]         = panel;
	c[ ImGuiCol_PopupBg ]         = panel;
	c[ ImGuiCol_Text ]            = text;
	c[ ImGuiCol_TextDisabled ]    = ImVec4( 0.5f, 0.5f, 0.5f, 1.0f );
	c[ ImGuiCol_Border ]          = ImVec4( 0, 0, 0, 0.5f );
	c[ ImGuiCol_FrameBg ]         = panel;
	c[ ImGuiCol_FrameBgHovered ]  = panelHi;
	c[ ImGuiCol_FrameBgActive ]   = panelHi;
	c[ ImGuiCol_Button ]          = panelHi;
	c[ ImGuiCol_ButtonHovered ]   = ImVec4( 0.50f, 0.45f, 0.0f, 1.0f );   // CoD2 hover gold
	c[ ImGuiCol_ButtonActive ]    = amber;
	c[ ImGuiCol_Header ]          = panelHi;
	c[ ImGuiCol_HeaderHovered ]   = ImVec4( 0.50f, 0.45f, 0.0f, 1.0f );
	c[ ImGuiCol_HeaderActive ]    = amber;
	c[ ImGuiCol_CheckMark ]       = amberHi;
	c[ ImGuiCol_SliderGrab ]      = amber;
	c[ ImGuiCol_SliderGrabActive ]= amberHi;
	c[ ImGuiCol_TitleBg ]         = bg;
	c[ ImGuiCol_TitleBgActive ]   = bg;
	c[ ImGuiCol_ScrollbarBg ]     = bg;
	c[ ImGuiCol_ScrollbarGrab ]   = panelHi;
	c[ ImGuiCol_Separator ]       = ImVec4( 0.25f, 0.22f, 0.05f, 1.0f );
}

// Decode the embedded logo JPEG and upload it as an OpenGL texture (once). Safe to call
// every frame — it no-ops after the first success.
static void EnsureLogoTexture( void )
{
	if ( g_logoTex ) return;
	int n = 0;
	unsigned char *px = stbi_load_from_memory( g_logoJpg, (int)g_logoJpg_len,
		&g_logoW, &g_logoH, &n, 4 );
	if ( !px ) return;
	glGenTextures( 1, &g_logoTex );
	glBindTexture( GL_TEXTURE_2D, g_logoTex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, g_logoW, g_logoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px );
	stbi_image_free( px );
}

// ---- the panel --------------------------------------------------------------------
static void DrawUI( HWND hwnd )
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos( vp->WorkPos );
	ImGui::SetNextWindowSize( vp->WorkSize );
	ImGui::Begin( "##main", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus );

	EnsureLogoTexture();

	// ---- header: logo + title + tagline -------------------------------------------
	float logoSz = 44.0f;
	if ( g_logoTex )
	{
		ImGui::Image( (ImTextureID)(intptr_t)g_logoTex, ImVec2( logoSz, logoSz ) );
		ImGui::SameLine( 0.0f, 14.0f );
	}
	ImGui::BeginGroup();
	if ( g_fontH1 ) ImGui::PushFont( g_fontH1 );
	ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.95f, 0.78f, 0.22f, 1.0f ) );  // CoD2 gold
	ImGui::TextUnformatted( "CoD2 Demo Tool" );
	ImGui::PopStyleColor();
	if ( g_fontH1 ) ImGui::PopFont();
	ImGui::TextDisabled( "Trim dead-time, cut clips, and read your demos - no command line." );
	ImGui::EndGroup();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// input / drop zone
	ImGui::TextDisabled( "DEMO" );
	ImGui::PushItemWidth( -110 );
	ImGui::InputText( "##in", g_input, sizeof( g_input ), ImGuiInputTextFlags_ReadOnly );
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if ( ImGui::Button( "Browse...", ImVec2( 100, 0 ) ) ) BrowseInput( hwnd );
	if ( !g_input[ 0 ] )
		ImGui::TextDisabled( "  ...or drag a .dm_1 file anywhere onto this window." );
	ImGui::Spacing();

	// operation
	ImGui::TextDisabled( "DO" );
	ImGui::PushItemWidth( -1 );
	if ( ImGui::Combo( "##op", &g_op, kOpNames, OP_COUNT ) ) RecomputeOutput();
	ImGui::PopItemWidth();
	ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.6f, 0.6f, 0.58f, 1.0f ) );
	ImGui::TextWrapped( "%s", kOpBlurb[ g_op ] );
	ImGui::PopStyleColor();
	ImGui::Spacing();

	// per-op extras
	if ( g_op == OP_SKIPDEAD )
		ImGui::Checkbox( "Keep the kill replays (only cut the dead-stare)", &g_keepKillcam );
	else if ( g_op == OP_CUT )
	{
		ImGui::PushItemWidth( 120 );
		ImGui::InputText( "from", g_cutStart, sizeof( g_cutStart ) );
		ImGui::SameLine();
		ImGui::InputText( "to", g_cutEnd, sizeof( g_cutEnd ) );
		ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::TextDisabled( "(mm:ss, seconds, or start/end)" );
	}
	else if ( g_op == OP_CLEAN )
	{
		ImGui::Checkbox( "chat", &g_cleanChat ); ImGui::SameLine();
		ImGui::Checkbox( "centre prints", &g_cleanCenter ); ImGui::SameLine();
		ImGui::Checkbox( "console prints", &g_cleanWhite );
	}

	// output
	if ( g_op != OP_INFO )
	{
		ImGui::Spacing();
		ImGui::TextDisabled( "SAVE AS" );
		ImGui::PushItemWidth( -1 );
		ImGui::InputText( "##out", g_output, sizeof( g_output ) );
		ImGui::PopItemWidth();
	}

	ImGui::Spacing(); ImGui::Spacing();

	// run
	ImGui::BeginDisabled( g_busy );
	if ( ImGui::Button( g_busy ? "Working..." : "RUN", ImVec2( -1, 38 ) ) )
		RunSelected( hwnd );
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::TextDisabled( "RESULT" );
	ImGui::InputTextMultiline( "##log", g_log, sizeof( g_log ),
		ImVec2( -1, -1 ), ImGuiInputTextFlags_ReadOnly );

	ImGui::End();
}

// ---- Win32 + OpenGL plumbing ------------------------------------------------------
static bool CreateGLContext( HWND hwnd )
{
	g_hdc = GetDC( hwnd );
	PIXELFORMATDESCRIPTOR pfd; memset( &pfd, 0, sizeof( pfd ) );
	pfd.nSize = sizeof( pfd ); pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
	int pf = ChoosePixelFormat( g_hdc, &pfd );
	if ( !pf || !SetPixelFormat( g_hdc, pf, &pfd ) ) return false;
	g_glrc = wglCreateContext( g_hdc );
	if ( !g_glrc ) return false;
	wglMakeCurrent( g_hdc, g_glrc );
	return true;
}

static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wp, lp ) )
		return true;

	switch ( msg )
	{
	case WM_DROPFILES:
	{
		char path[ 1024 ] = { 0 };
		if ( DragQueryFileA( (HDROP)wp, 0, path, sizeof( path ) ) ) SetInput( path );
		DragFinish( (HDROP)wp );
		return 0;
	}
	case WM_SIZE:
		if ( g_glrc && wp != SIZE_MINIMIZED )
			glViewport( 0, 0, LOWORD( lp ), HIWORD( lp ) );
		return 0;
	case WM_DESTROY:
		PostQuitMessage( 0 );
		return 0;
	}
	return DefWindowProcA( hwnd, msg, wp, lp );
}

int WINAPI WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow )
{
	(void)hPrev; (void)nShow;

	WNDCLASSEXA wc; memset( &wc, 0, sizeof( wc ) );
	wc.cbSize = sizeof( wc );
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor( NULL, IDC_ARROW );
	wc.hIcon = LoadIcon( hInst, MAKEINTRESOURCE( 1 ) );
	wc.lpszClassName = "CoD2DemoToolImGui";
	RegisterClassExA( &wc );

	HWND hwnd = CreateWindowExA( WS_EX_ACCEPTFILES, wc.lpszClassName, "CoD2 Demo Tool",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 660, 680,
		NULL, NULL, hInst, NULL );

	if ( !CreateGLContext( hwnd ) )
	{
		MessageBoxA( hwnd, "Could not create an OpenGL context.", "CoD2 Demo Tool", MB_OK | MB_ICONERROR );
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL;   // don't litter an imgui.ini beside the exe

	// Bundled Roboto (embedded) for a clean, crisp UI — no system-font dependency. The
	// font bytes are owned by the static array, so tell ImGui not to free them.
	ImFontConfig fc;
	fc.FontDataOwnedByAtlas = false;
	g_fontBody = io.Fonts->AddFontFromMemoryTTF( (void *)g_robotoTtf, (int)g_robotoTtf_len, 17.0f, &fc );
	g_fontH1   = io.Fonts->AddFontFromMemoryTTF( (void *)g_robotoTtf, (int)g_robotoTtf_len, 28.0f, &fc );
	if ( !g_fontBody ) g_fontBody = io.Fonts->AddFontDefault();   // fallback

	ApplyCoD2Theme();
	ImGui_ImplWin32_InitForOpenGL( hwnd );
	ImGui_ImplOpenGL3_Init( "#version 130" );

	DragAcceptFiles( hwnd, TRUE );
	ShowWindow( hwnd, SW_SHOWNORMAL );
	UpdateWindow( hwnd );

	// preload a demo dropped onto the exe itself
	if ( lpCmdLine && lpCmdLine[ 0 ] )
	{
		char p[ 1024 ]; Q_strncpyz( p, lpCmdLine, sizeof( p ) );
		char *q = p;
		if ( q[ 0 ] == '"' ) { q++; char *e = strrchr( q, '"' ); if ( e ) *e = 0; }
		SetInput( q );
	}

	bool running = true;
	while ( running )
	{
		MSG m;
		while ( PeekMessage( &m, NULL, 0, 0, PM_REMOVE ) )
		{
			if ( m.message == WM_QUIT ) running = false;
			TranslateMessage( &m );
			DispatchMessage( &m );
		}
		if ( !running ) break;

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawUI( hwnd );
		ImGui::Render();

		glClearColor( 0.04f, 0.045f, 0.05f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
		SwapBuffers( g_hdc );
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	wglMakeCurrent( NULL, NULL );
	wglDeleteContext( g_glrc );
	ReleaseDC( hwnd, g_hdc );
	return 0;
}

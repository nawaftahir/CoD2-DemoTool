// CoD2-DemoTool — Win32 GUI front-end.
//
// A small native window so the tool is usable without the command line: drag a demo in
// (or Browse), pick an operation, click Run, read the result. Double-click target on
// Windows; the console build (cod2-demotool.exe) is unchanged and stays the scriptable
// path.
//
// This file IS the GUI translation unit: it #defines GUI_BUILD and #includes the whole
// tool (reader.cpp, which pulls in writer.h + icons_embed.h), so every Cmd_* function,
// global, and helper compiles in here and is called directly in-process. reader.cpp's
// main() is compiled out under GUI_BUILD; this file provides WinMain instead.
//
// Build: ./build-win-gui.sh  (dockcross mingw, -mwindows). Link libs: comdlg32 (file
// dialog), shell32 (drag-drop), gdi32/user32 (window + controls).

#define GUI_BUILD
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "reader.cpp"

// ---- control IDs ------------------------------------------------------------------
#define IDC_INPUT    1001
#define IDC_BROWSE   1002
#define IDC_OP       1003
#define IDC_OUTPUT   1004
#define IDC_OUTBROWSE 1005
#define IDC_RUN      1006
#define IDC_LOG      1007
// per-op extras
#define IDC_KEEPKC   1010   // skip-dead: keep killcam
#define IDC_START    1011   // cut: start time
#define IDC_END      1012   // cut: end time
#define IDC_CHAT     1013   // clean: chat
#define IDC_CENTER   1014   // clean: centertext
#define IDC_WHITE    1015   // clean: whitetext
#define IDC_LBL_OUT  1016   // "Output:" label (hidden for Info)
#define IDC_LBL_KC   1017
#define IDC_LBL_CUT  1018
#define IDC_LBL_CLEAN 1019

// ---- operations -------------------------------------------------------------------
enum { OP_INFO = 0, OP_SKIPDEAD, OP_CUT, OP_REMOVEHUD, OP_CLEAN, OP_OVERVIEW, OP_COUNT };
static const char *kOpNames[ OP_COUNT ] = {
	"Info (what is this demo?)",
	"Skip dead-time",
	"Cut to time range",
	"Remove HUD",
	"Clean text (chat / prints)",
	"Match overview (HTML)",
};
// output filename suffix per op (Info/Overview handled specially)
static const char *kOpSuffix[ OP_COUNT ] = { "", "skipdead", "cut", "nohud", "clean", "" };

static HWND g_inputE, g_opC, g_outputE, g_outLbl, g_logE, g_runB, g_outBrowse;
static HWND g_keepKc, g_kcLbl, g_startE, g_endE, g_cutLbl, g_chat, g_center, g_white, g_cleanLbl;
static HFONT g_font;
static int  g_running = 0;

// ---- helpers ----------------------------------------------------------------------

static void GetText( HWND h, char *out, int n )
{
	GetWindowTextA( h, out, n );
}

// Default output path beside the input: <dir>/<name>_<suffix>.dm_1 (or .html for overview).
static void DefaultOutput( const char *in, int op, char *out, int n )
{
	if ( !in[ 0 ] ) { out[ 0 ] = 0; return; }
	if ( op == OP_OVERVIEW ) { HtmlPathFor( in, out, n ); return; }
	if ( op == OP_INFO )     { out[ 0 ] = 0; return; }

	Q_strncpyz( out, in, n );
	char *dot = strrchr( out, '.' );
	char *sl  = strrchr( out, '/' );
	char *bs  = strrchr( out, '\\' );
	if ( bs > sl ) sl = bs;
	if ( dot && dot > sl ) *dot = 0;
	int len = (int)strlen( out );
	snprintf( out + len, n - len, "_%s.dm_1", kOpSuffix[ op ] );
}

// Show/hide the per-op extra controls for the selected operation.
static void UpdateOpPanel( void )
{
	int op = (int)SendMessageA( g_opC, CB_GETCURSEL, 0, 0 );
	int sd = ( op == OP_SKIPDEAD ), ct = ( op == OP_CUT ), cl = ( op == OP_CLEAN );
	int hasOut = ( op != OP_INFO );

	ShowWindow( g_keepKc, sd ? SW_SHOW : SW_HIDE );
	ShowWindow( g_kcLbl,  sd ? SW_SHOW : SW_HIDE );
	ShowWindow( g_startE, ct ? SW_SHOW : SW_HIDE );
	ShowWindow( g_endE,   ct ? SW_SHOW : SW_HIDE );
	ShowWindow( g_cutLbl, ct ? SW_SHOW : SW_HIDE );
	ShowWindow( g_chat,   cl ? SW_SHOW : SW_HIDE );
	ShowWindow( g_center, cl ? SW_SHOW : SW_HIDE );
	ShowWindow( g_white,  cl ? SW_SHOW : SW_HIDE );
	ShowWindow( g_cleanLbl, cl ? SW_SHOW : SW_HIDE );
	ShowWindow( g_outputE,   hasOut ? SW_SHOW : SW_HIDE );
	ShowWindow( g_outLbl,    hasOut ? SW_SHOW : SW_HIDE );
	ShowWindow( g_outBrowse, hasOut ? SW_SHOW : SW_HIDE );
}

static void RecomputeOutput( void )
{
	char in[ 1024 ], out[ 1024 ];
	GetText( g_inputE, in, sizeof( in ) );
	int op = (int)SendMessageA( g_opC, CB_GETCURSEL, 0, 0 );
	DefaultOutput( in, op, out, sizeof( out ) );
	SetWindowTextA( g_outputE, out );
}

static void SetInputPath( const char *path )
{
	SetWindowTextA( g_inputE, path );
	RecomputeOutput();
}

// Native "open a .dm_1" dialog.
static void BrowseInput( HWND hwnd )
{
	char file[ 1024 ] = { 0 };
	OPENFILENAMEA ofn;
	memset( &ofn, 0, sizeof( ofn ) );
	ofn.lStructSize = sizeof( ofn );
	ofn.hwndOwner   = hwnd;
	ofn.lpstrFilter = "CoD2 demo (*.dm_1)\0*.dm_1\0All files\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof( file );
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if ( GetOpenFileNameA( &ofn ) )
		SetInputPath( file );
}

static void BrowseOutput( HWND hwnd )
{
	char file[ 1024 ] = { 0 };
	GetText( g_outputE, file, sizeof( file ) );
	OPENFILENAMEA ofn;
	memset( &ofn, 0, sizeof( ofn ) );
	ofn.lStructSize = sizeof( ofn );
	ofn.hwndOwner   = hwnd;
	ofn.lpstrFilter = "Demo / page\0*.dm_1;*.html\0All files\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof( file );
	ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
	if ( GetSaveFileNameA( &ofn ) )
		SetWindowTextA( g_outputE, file );
}

// Append captured tool output to the log pane (the tool emits \n; EDIT wants \r\n).
static void SetLogFromFile( const char *path )
{
	FILE *f = fopen( path, "rb" );
	if ( !f ) { SetWindowTextA( g_logE, "(no output)" ); return; }
	fseek( f, 0, SEEK_END );
	long sz = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( sz < 0 ) sz = 0;
	char *raw = (char *)malloc( sz + 1 );
	long n = (long)fread( raw, 1, sz, f );
	raw[ n ] = 0;
	fclose( f );

	// translate \n -> \r\n
	char *crlf = (char *)malloc( n * 2 + 1 );
	int j = 0;
	for ( long i = 0; i < n; i++ )
	{
		if ( raw[ i ] == '\n' && ( i == 0 || raw[ i - 1 ] != '\r' ) ) crlf[ j++ ] = '\r';
		crlf[ j++ ] = raw[ i ];
	}
	crlf[ j ] = 0;
	SetWindowTextA( g_logE, j ? crlf : "(done)" );
	free( raw );
	free( crlf );
}

// Run the selected operation in-process, capturing the tool's stdout into the log pane.
// The GUI has no console, so stdout is freopen'd to a temp file for the duration of the
// op (zero edits to the tool's ~133 printf sites), then read back.
static void RunSelected( HWND hwnd )
{
	if ( g_running ) return;

	char in[ 1024 ], out[ 1024 ];
	GetText( g_inputE, in, sizeof( in ) );
	GetText( g_outputE, out, sizeof( out ) );
	if ( !in[ 0 ] )
	{
		MessageBoxA( hwnd, "Drag a .dm_1 demo in, or use Browse.", "No demo", MB_OK | MB_ICONWARNING );
		return;
	}
	int op = (int)SendMessageA( g_opC, CB_GETCURSEL, 0, 0 );

	// temp capture file in %TEMP%
	char tmp[ 1024 ];
	char tdir[ 800 ] = { 0 };
	GetTempPathA( sizeof( tdir ), tdir );
	snprintf( tmp, sizeof( tmp ), "%scod2dt_out.txt", tdir );

	g_running = 1;
	EnableWindow( g_runB, FALSE );
	SetWindowTextA( g_logE, "working...\r\n" );
	SetWindowTextA( g_runB, "Working..." );
	UpdateWindow( hwnd );

	FILE *redir = freopen( tmp, "w", stdout );
	(void)redir;

	// Full state hygiene before every op (the GUI reuses one process; the CLI got a
	// fresh one per run). ResetFilters() zeros the filter flags; we also reset the
	// decoder bookkeeping that lives outside cl/clc (the demo handle, the skip-dead
	// ping-pong ring, the overview event buffer) and silence the verbose per-frame
	// log — without g_quietLog a single op writes a ~28 MB <demo>.log and takes ~14 s.
	ResetFilters();
	memset( &demo, 0, sizeof( demo ) );
	g_sfValid = qfalse;
	g_sfCur   = 0;
	g_ovNumEvents = 0;
	g_quietLog = 1;

	int rc = 0;
	switch ( op )
	{
	case OP_INFO:
		rc = Cmd_Info( in );
		break;
	case OP_SKIPDEAD:
		rc = Cmd_SkipDead( in, out, SendMessageA( g_keepKc, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
		break;
	case OP_CUT:
	{
		char s[ 64 ], e[ 64 ];
		GetText( g_startE, s, sizeof( s ) );
		GetText( g_endE,   e, sizeof( e ) );
		if ( !s[ 0 ] ) Q_strncpyz( s, "start", sizeof( s ) );
		if ( !e[ 0 ] ) Q_strncpyz( e, "end",   sizeof( e ) );
		rc = Cmd_Cut( in, out, s, e );
		break;
	}
	case OP_REMOVEHUD:
		g_removeHud = 1;
		rc = Cmd_Copy( in, out );
		break;
	case OP_CLEAN:
		g_removeChat       = ( SendMessageA( g_chat,   BM_GETCHECK, 0, 0 ) == BST_CHECKED );
		g_removeCenterText = ( SendMessageA( g_center, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
		g_removeWhiteText  = ( SendMessageA( g_white,  BM_GETCHECK, 0, 0 ) == BST_CHECKED );
		rc = Cmd_Copy( in, out );
		break;
	case OP_OVERVIEW:
		rc = Cmd_Overview( in, out );
		break;
	}

	fflush( stdout );
	freopen( "NUL", "w", stdout );   // detach the temp file so we can read it

	SetLogFromFile( tmp );
	DeleteFileA( tmp );

	// Overview: open the generated HTML in the browser.
	if ( op == OP_OVERVIEW && rc == 0 && out[ 0 ] )
		ShellExecuteA( hwnd, "open", out, NULL, NULL, SW_SHOWNORMAL );

	SetWindowTextA( g_runB, "Run" );
	EnableWindow( g_runB, TRUE );
	g_running = 0;
}

// ---- window ------------------------------------------------------------------------

static HWND mk( HWND parent, const char *cls, const char *text, DWORD style,
                int x, int y, int w, int h, int id )
{
	HWND c = CreateWindowExA( 0, cls, text, WS_CHILD | WS_VISIBLE | style,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL );
	SendMessageA( c, WM_SETFONT, (WPARAM)g_font, TRUE );
	return c;
}

static void CreateControls( HWND hwnd )
{
	const int M = 12, RW = 560;
	int y = M;

	// input row
	mk( hwnd, "STATIC", "Demo:", WS_VISIBLE, M, y + 4, 44, 20, 0 );
	g_inputE = mk( hwnd, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, M + 48, y, RW - 48 - 90, 24, IDC_INPUT );
	mk( hwnd, "BUTTON", "Browse...", WS_VISIBLE | BS_PUSHBUTTON, M + RW - 84, y, 84, 24, IDC_BROWSE );
	y += 34;

	// (drop hint)
	mk( hwnd, "STATIC", "...or drag a .dm_1 file onto this window.", WS_VISIBLE, M + 48, y, RW - 48, 18, 0 );
	y += 26;

	// operation row
	mk( hwnd, "STATIC", "Do:", WS_VISIBLE, M, y + 4, 44, 20, 0 );
	g_opC = mk( hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, M + 48, y, RW - 48, 200, IDC_OP );
	for ( int i = 0; i < OP_COUNT; i++ )
		SendMessageA( g_opC, CB_ADDSTRING, 0, (LPARAM)kOpNames[ i ] );
	SendMessageA( g_opC, CB_SETCURSEL, OP_INFO, 0 );
	y += 36;

	// per-op extras (all created here, shown/hidden by UpdateOpPanel)
	g_kcLbl  = mk( hwnd, "STATIC", "Killcam:", 0, M, y + 2, 60, 20, IDC_LBL_KC );
	g_keepKc = mk( hwnd, "BUTTON", "keep the kill replays (only cut the dead-stare)", BS_AUTOCHECKBOX, M + 64, y, RW - 64, 22, IDC_KEEPKC );

	g_cutLbl = mk( hwnd, "STATIC", "Range:", 0, M, y + 2, 60, 20, IDC_LBL_CUT );
	g_startE = mk( hwnd, "EDIT", "start", WS_BORDER, M + 64, y, 90, 22, IDC_START );
	mk( hwnd, "STATIC", "to", 0, M + 160, y + 2, 18, 20, 0 );
	g_endE   = mk( hwnd, "EDIT", "end", WS_BORDER, M + 182, y, 90, 22, IDC_END );

	g_cleanLbl = mk( hwnd, "STATIC", "Strip:", 0, M, y + 2, 60, 20, IDC_LBL_CLEAN );
	g_chat   = mk( hwnd, "BUTTON", "chat", BS_AUTOCHECKBOX, M + 64,  y, 70, 22, IDC_CHAT );
	g_center = mk( hwnd, "BUTTON", "center prints", BS_AUTOCHECKBOX, M + 140, y, 120, 22, IDC_CENTER );
	g_white  = mk( hwnd, "BUTTON", "console prints", BS_AUTOCHECKBOX, M + 266, y, 130, 22, IDC_WHITE );
	SendMessageA( g_chat,   BM_SETCHECK, BST_CHECKED, 0 );
	SendMessageA( g_center, BM_SETCHECK, BST_CHECKED, 0 );
	SendMessageA( g_white,  BM_SETCHECK, BST_CHECKED, 0 );
	y += 34;

	// output row
	g_outLbl   = mk( hwnd, "STATIC", "Save as:", 0, M, y + 4, 60, 20, IDC_LBL_OUT );
	g_outputE  = mk( hwnd, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, M + 64, y, RW - 64 - 90, 24, IDC_OUTPUT );
	g_outBrowse = mk( hwnd, "BUTTON", "...", WS_VISIBLE | BS_PUSHBUTTON, M + RW - 84, y, 84, 24, IDC_OUTBROWSE );
	y += 36;

	// run
	g_runB = mk( hwnd, "BUTTON", "Run", WS_VISIBLE | BS_DEFPUSHBUTTON, M, y, RW, 30, IDC_RUN );
	y += 40;

	// log
	g_logE = mk( hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
		M, y, RW, 180, IDC_LOG );

	UpdateOpPanel();
}

static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	switch ( msg )
	{
	case WM_CREATE:
		g_font = (HFONT)GetStockObject( DEFAULT_GUI_FONT );
		CreateControls( hwnd );
		DragAcceptFiles( hwnd, TRUE );
		return 0;

	case WM_DROPFILES:
	{
		HDROP drop = (HDROP)wp;
		char path[ 1024 ] = { 0 };
		if ( DragQueryFileA( drop, 0, path, sizeof( path ) ) )
			SetInputPath( path );
		DragFinish( drop );
		return 0;
	}

	case WM_COMMAND:
	{
		int id = LOWORD( wp ), code = HIWORD( wp );
		if ( id == IDC_BROWSE && code == BN_CLICKED )      BrowseInput( hwnd );
		else if ( id == IDC_OUTBROWSE && code == BN_CLICKED ) BrowseOutput( hwnd );
		else if ( id == IDC_RUN && code == BN_CLICKED )    RunSelected( hwnd );
		else if ( id == IDC_OP && code == CBN_SELCHANGE )  { UpdateOpPanel(); RecomputeOutput(); }
		return 0;
	}

	case WM_CLOSE:
		DestroyWindow( hwnd );
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

	WNDCLASSA wc;
	memset( &wc, 0, sizeof( wc ) );
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor( NULL, IDC_ARROW );
	wc.hbrBackground = (HBRUSH)( COLOR_BTNFACE + 1 );
	wc.lpszClassName = "CoD2DemoToolWnd";
	wc.hIcon         = LoadIcon( hInst, MAKEINTRESOURCE( 1 ) );   // version.rc icon if present
	RegisterClassA( &wc );

	int W = 600, H = 480;
	HWND hwnd = CreateWindowExA( WS_EX_ACCEPTFILES, wc.lpszClassName, "CoD2 Demo Tool",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hInst, NULL );
	ShowWindow( hwnd, SW_SHOWNORMAL );

	// If a demo was dropped onto the exe (argv), preload it.
	if ( lpCmdLine && lpCmdLine[ 0 ] )
	{
		char p[ 1024 ];
		Q_strncpyz( p, lpCmdLine, sizeof( p ) );
		// strip surrounding quotes Windows adds around paths with spaces
		char *q = p;
		if ( q[ 0 ] == '"' ) { q++; char *e = strrchr( q, '"' ); if ( e ) *e = 0; }
		SetInputPath( q );
	}

	MSG m;
	while ( GetMessage( &m, NULL, 0, 0 ) > 0 )
	{
		if ( !IsDialogMessage( hwnd, &m ) )
		{
			TranslateMessage( &m );
			DispatchMessage( &m );
		}
	}
	return 0;
}

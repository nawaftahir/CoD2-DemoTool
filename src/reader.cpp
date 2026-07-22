#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "declarations.hpp"

#define Com_DPrintf Com_Printf
#define Com_Memset memset
#define Q_vsnprintf vsnprintf
#define Q_strncpyz I_strncpyz
#define Com_Memcpy memcpy
#define Com_Memset memset


static char logFileName[1024];




#define ERR_FATAL 0
#define ERR_DROP 1
void Com_Error(int err, char* fmt,...)
{
	char buf[MAX_STRING_CHARS];

	va_list		argptr;

	va_start (argptr,fmt);
	Q_vsnprintf(buf, sizeof(buf), fmt, argptr);
	va_end (argptr);

	fputs(buf, stdout);
	//exit(1);
}

int g_quietLog = 0;   // when set, Com_Printf is a no-op (--copy avoids per-call fopen of the log)

// Record-and-replay of playerstate delta decisions (byte-1:1 re-encode). The decoder
// stashes the last playerstate's lc + per-field changed bits here; CL_ParseSnapshot
// copies them into the snapshot. The encoder replays them when g_encReplay is set and
// it is re-encoding against the original delta base (copy/verify/convert), else recomputes.
int g_recPsLc = 0;
byte g_recPsChanged[128];
int g_encReplay = 0;                 // 1 = replay original encoding decisions verbatim
const clSnapshot_t *g_encSnap = 0;   // the snapshot being encoded (source of replay decisions)

// Same record-and-replay for entity/client delta-structs. The decoder captures each
// struct's lc + per-field changed bits (MSG_ReadDeltaField reports its change bit via
// g_fieldChangedBit); CL_DeltaEntity/Client store them in a ring parallel to
// cl.parseEntities/parseClients; the encoder replays via g_encStructEnc.
typedef struct { int lc; byte changed[128]; byte valid; } deltaEnc_t;
int g_recStructLc = 0;
byte g_recStructChanged[128];
int g_fieldChangedBit = 0;
static deltaEnc_t g_entEnc[MAX_PARSE_ENTITIES];
static deltaEnc_t g_cliEnc[MAX_PARSE_CLIENTS];
const deltaEnc_t *g_encStructEnc = 0;   // record for the entity/client currently being encoded
int g_forceFieldChanged = -1;           // -1 = recompute changed-bit; 0/1 = replay this bit
int g_dumpCommands = 0;   // when set, CL_ParseCommandString prints each server command (for --commands)

// Server-command filters (Caball-style). Applied during transcode: a dropped
// command is simply not re-emitted; its sequence number is left as a harmless gap
// (the client tracks serverCommandSequence as a high-water mark — CL_ParseCommandString
// dedupes only seq<=current and never waits for a missing intermediate seq).
int g_removeChat       = 0;   // drop verbs h (public) + i (team)
int g_removeCenterText = 0;   // drop verb c (Announcement / center print)
int g_removeWhiteText  = 0;   // drop verbs e/f/g (allClientsPrint / iprintln / iprintlnbold)

// Returns 1 if this server-command string should be dropped by the active filters.
// s is "<verb> <payload>"; every real text verb has s[1]==' '. Control/state/score
// verbs (b scoreboard, v cvar, I health, G/H score, o/p/q fades, ...) are NEVER dropped.
static int CmdShouldDrop( const char *s )
{
	if ( !s[ 0 ] || s[ 1 ] != ' ' )
		return 0;
	char v = s[ 0 ];
	if ( g_removeChat       && ( v == 'h' || v == 'i' ) )                 return 1;
	if ( g_removeCenterText && v == 'c' )                                return 1;
	if ( g_removeWhiteText  && ( v == 'e' || v == 'f' || v == 'g' ) )     return 1;
	return 0;
}

// HUD / score-popup mutators (Caball-style). These edit cl.snap.ps in place right
// after CL_ParseSnapshot, BEFORE the snapshot is stored as the next frame's delta
// base — so the stored base and the emitted frame agree (else the delta desyncs).
const char *CL_ConfigString( int index );      // defined below; needed by HudElemKept
#define CS_SHADERS 1566                         // CoD2rev g_shared.h:1255 — material configstring base
#define SNAPFLAG_SERVERCOUNT 4                  // CoD2rev q_shared.h:180 — toggled on every map_restart

// Protocol conversion: rewrite the protocol/shortversion in configstring[0] on emit.
// For CoD2 the wire format is identical across versions, so this is a metadata swap.
void Info_SetValueForKey( char *s, int dstsize, const char *key, const char *value );  // defined below
int  g_convertProtocol = 0;                     // 0 = off, else target protocol (115/117/118/119/120)
const char *ProtocolShortVersion( int proto )
{
	switch ( proto )
	{
	case 115: return "1.0";
	case 117: return "1.2";
	case 118: return "1.3";
	case 119: case 120: return "1.4";
	default:  return "";
	}
}
int    g_removeHud  = 0;       // strip all hud elements (subject to the keep-list below)
int    g_scaleScore = 0;       // scale HE_TYPE_VALUE score popups
float  g_scoreMult  = 1.0f;
const char *g_keepShader = NULL;   // --remove-hud keep <shader-substring>

// Keep a hudelem if a keep-rule matches: its shader name (materialIndex -> CS_SHADERS)
// contains g_keepShader. Default = drop.
static int HudElemKept( const hudelem_t *h )
{
	if ( g_keepShader && h->materialIndex )
	{
		const char *shader = CL_ConfigString( CS_SHADERS + h->materialIndex );
		if ( shader[ 0 ] && strstr( shader, g_keepShader ) )
			return 1;
	}
	return 0;
}

// Remove (and COMPACT) hud elements in one array. MSG_WriteDeltaHudElems stops at the
// first HE_TYPE_FREE, so survivors must be packed from index 0 with a zeroed tail —
// exactly how the engine fills these arrays.
static void HudArrayRemove( hudelem_t *arr, int count )
{
	int w = 0;
	for ( int r = 0; r < count; r++ )
	{
		if ( arr[ r ].type == HE_TYPE_FREE )
			break;                                  // already the packed tail
		if ( HudElemKept( &arr[ r ] ) )
		{
			if ( w != r ) arr[ w ] = arr[ r ];
			w++;
		}
	}
	for ( ; w < count; w++ )
		memset( &arr[ w ], 0, sizeof( arr[ w ] ) );   // zero the tail (HE_TYPE_FREE)
}

// Scale a score-popup hudelem in one array: any HE_TYPE_VALUE element's value is
// multiplied (rounded to nearest, sign preserved). Ammo/timer use other types.
static void HudArrayScale( hudelem_t *arr, int count, float mult )
{
	for ( int i = 0; i < count; i++ )
	{
		if ( arr[ i ].type == HE_TYPE_FREE )
			break;
		if ( arr[ i ].type == HE_TYPE_VALUE )
		{
			float v = arr[ i ].value * mult;
			arr[ i ].value = (float)(int)( v + ( v >= 0 ? 0.5f : -0.5f ) );
		}
	}
}

// Apply the active hud/score edits to a playerstate (both archival + current arrays).
static void EditPlayerstateHud( playerState_t *ps )
{
	if ( g_removeHud )
	{
		HudArrayRemove( ps->hud.archival, MAX_HUDELEMS_ARCHIVAL );
		HudArrayRemove( ps->hud.current,  MAX_HUDELEMS_CURRENT );
	}
	if ( g_scaleScore )
	{
		HudArrayScale( ps->hud.archival, MAX_HUDELEMS_ARCHIVAL, g_scoreMult );
		HudArrayScale( ps->hud.current,  MAX_HUDELEMS_CURRENT,  g_scoreMult );
	}
}

// Chat / announcement / score events collected during decode, drained per frame
// by --overview so they interleave chronologically with the kills.
#define OV_MAX_EVENTS 64
#define OV_CHAT       0
#define OV_TEAMCHAT   1
#define OV_ANNOUNCE   2
#define OV_SCORE_ALLIES 3
#define OV_SCORE_AXIS   4
typedef struct { int kind; int value; char verb; char text[ 256 ]; } ovEvent_t;
static ovEvent_t g_ovEvents[ OV_MAX_EVENTS ];
static int g_ovNumEvents = 0;
int g_collectEvents = 0;

#ifdef GUI_BUILD
// The GUI calls the Cmd_* functions repeatedly in one process. The CLI relies on the
// OS clearing these per-process; the GUI must zero them before every operation or a
// previous run's filters leak into the next. Decoder state (cl/clc/demo) is already
// reset per file by CL_ResetState inside FS_FOpenFileRead — only these stay sticky.
// This is the single owner of the full filter-flag set; keep it next to the flags.
static void ResetFilters( void )
{
	g_quietLog         = 0;
	g_dumpCommands     = 0;
	g_removeChat       = 0;
	g_removeCenterText = 0;
	g_removeWhiteText  = 0;
	g_removeHud        = 0;
	g_scaleScore       = 0;
	g_scoreMult        = 1.0f;
	g_keepShader       = NULL;
	g_convertProtocol  = 0;
	g_collectEvents    = 0;
}
#endif // GUI_BUILD

void Com_Printf( const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( g_quietLog )
		return;

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	msg[MAXPRINTMSG - 1] = 0;
	
	//printf("%s", msg);

	// Save to file
	FILE *fp = fopen(logFileName, "a");
	if (fp)
	{
		fprintf(fp, "%s", msg);
		fclose(fp);
	}
	else
	{
		printf("Failed to open output.log for writing.\n");
	}
}

void I_strncpyz( char *dest, const char *src, int destsize )
{
	strncpy( dest, src, destsize-1 );
	dest[destsize-1] = 0;
}


static huffman_t msgHuff;
static qboolean	msgInit = qfalse;

int LittleLong( int l )
{
	return l;
}

short LittleShort( short l )
{
	return l;
}

// BEGIN huffman.c from Enemy Territory
static int bloc = 0;

//bani - optimized version
//clears data along the way so we dont have to memset() it ahead of time
void    Huff_putBit( int bit, byte *fout, int *offset )
{
	int x, y;
	bloc = *offset;
	x = bloc >> 3;
	y = bloc & 7;
	if ( !y )
	{
		fout[ x ] = 0;
	}
	fout[ x ] |= bit << y;
	bloc++;
	*offset = bloc;
}

//bani - optimized version
//optimization works on gcc 3.x, but not 2.95 ? most curious.
int Huff_getBit( byte *fin, int *offset )
{
	int t;
	bloc = *offset;
	t = fin[ bloc >> 3 ] >> ( bloc & 7 ) & 0x1;
	bloc++;
	*offset = bloc;
	return t;
}

//bani - optimized version
//clears data along the way so we dont have to memset() it ahead of time
static void add_bit( char bit, byte *fout )
{
	int x, y;

	y = bloc >> 3;
	x = bloc++ & 7;
	if ( !x )
	{
		fout[ y ] = 0;
	}
	fout[ y ] |= bit << x;
}

//bani - optimized version
//optimization works on gcc 3.x, but not 2.95 ? most curious.
static int get_bit( byte *fin )
{
	int t;
	t = fin[ bloc >> 3 ] >> ( bloc & 7 ) & 0x1;
	bloc++;
	return t;
}

static node_t **get_ppnode( huff_t* huff )
{
	node_t **tppnode;
	if ( !huff->freelist )
	{
		return &( huff->nodePtrs[huff->blocPtrs++] );
	}
	else
	{
		tppnode = huff->freelist;
		huff->freelist = (node_t **)*tppnode;
		return tppnode;
	}
}

static void free_ppnode( huff_t* huff, node_t **ppnode )
{
	*ppnode = (node_t *)huff->freelist;
	huff->freelist = ppnode;
}

/* Swap the location of these two nodes in the tree */
static void swap( huff_t* huff, node_t *node1, node_t *node2 )
{
	node_t *par1, *par2;

	par1 = node1->parent;
	par2 = node2->parent;

	if ( par1 )
	{
		if ( par1->left == node1 )
		{
			par1->left = node2;
		}
		else
		{
			par1->right = node2;
		}
	}
	else
	{
		huff->tree = node2;
	}

	if ( par2 )
	{
		if ( par2->left == node2 )
		{
			par2->left = node1;
		}
		else
		{
			par2->right = node1;
		}
	}
	else
	{
		huff->tree = node1;
	}

	node1->parent = par2;
	node2->parent = par1;
}

/* Swap these two nodes in the linked list (update ranks) */
static void swaplist( node_t *node1, node_t *node2 )
{
	node_t *par1;

	par1 = node1->next;
	node1->next = node2->next;
	node2->next = par1;

	par1 = node1->prev;
	node1->prev = node2->prev;
	node2->prev = par1;

	if ( node1->next == node1 )
	{
		node1->next = node2;
	}
	if ( node2->next == node2 )
	{
		node2->next = node1;
	}
	if ( node1->next )
	{
		node1->next->prev = node1;
	}
	if ( node2->next )
	{
		node2->next->prev = node2;
	}
	if ( node1->prev )
	{
		node1->prev->next = node1;
	}
	if ( node2->prev )
	{
		node2->prev->next = node2;
	}
}

/* Do the increments */
static void increment( huff_t* huff, node_t *node )
{
	node_t *lnode;

	if ( !node )
	{
		return;
	}

	if ( node->next != NULL && node->next->weight == node->weight )
	{
		lnode = *node->head;
		if ( lnode != node->parent )
		{
			swap( huff, lnode, node );
		}
		swaplist( lnode, node );
	}
	if ( node->prev && node->prev->weight == node->weight )
	{
		*node->head = node->prev;
	}
	else
	{
		*node->head = NULL;
		free_ppnode( huff, node->head );
	}
	node->weight++;
	if ( node->next && node->next->weight == node->weight )
	{
		node->head = node->next->head;
	}
	else
	{
		node->head = get_ppnode( huff );
		*node->head = node;
	}
	if ( node->parent )
	{
		increment( huff, node->parent );
		if ( node->prev == node->parent )
		{
			swaplist( node, node->parent );
			if ( *node->head == node )
			{
				*node->head = node->parent;
			}
		}
	}
}

void Huff_addRef( huff_t* huff, byte ch )
{
	node_t *tnode, *tnode2;
	if ( huff->loc[ch] == NULL )   /* if this is the first transmission of this node */
	{
		tnode = &( huff->nodeList[huff->blocNode++] );
		tnode2 = &( huff->nodeList[huff->blocNode++] );

		tnode2->symbol = INTERNAL_NODE;
		tnode2->weight = 1;
		tnode2->next = huff->lhead->next;
		if ( huff->lhead->next )
		{
			huff->lhead->next->prev = tnode2;
			if ( huff->lhead->next->weight == 1 )
			{
				tnode2->head = huff->lhead->next->head;
			}
			else
			{
				tnode2->head = get_ppnode( huff );
				*tnode2->head = tnode2;
			}
		}
		else
		{
			tnode2->head = get_ppnode( huff );
			*tnode2->head = tnode2;
		}
		huff->lhead->next = tnode2;
		tnode2->prev = huff->lhead;

		tnode->symbol = ch;
		tnode->weight = 1;
		tnode->next = huff->lhead->next;
		if ( huff->lhead->next )
		{
			huff->lhead->next->prev = tnode;
			if ( huff->lhead->next->weight == 1 )
			{
				tnode->head = huff->lhead->next->head;
			}
			else
			{
				/* this should never happen */
				tnode->head = get_ppnode( huff );
				*tnode->head = tnode2;
			}
		}
		else
		{
			/* this should never happen */
			tnode->head = get_ppnode( huff );
			*tnode->head = tnode;
		}
		huff->lhead->next = tnode;
		tnode->prev = huff->lhead;
		tnode->left = tnode->right = NULL;

		if ( huff->lhead->parent )
		{
			if ( huff->lhead->parent->left == huff->lhead )   /* lhead is guaranteed to by the NYT */
			{
				huff->lhead->parent->left = tnode2;
			}
			else
			{
				huff->lhead->parent->right = tnode2;
			}
		}
		else
		{
			huff->tree = tnode2;
		}

		tnode2->right = tnode;
		tnode2->left = huff->lhead;

		tnode2->parent = huff->lhead->parent;
		huff->lhead->parent = tnode->parent = tnode2;

		huff->loc[ch] = tnode;

		increment( huff, tnode2->parent );
	}
	else
	{
		increment( huff, huff->loc[ch] );
	}
}

/* Get a symbol */
int Huff_Receive( node_t *node, int *ch, byte *fin )
{
	while ( node && node->symbol == INTERNAL_NODE )
	{
		if ( get_bit( fin ) )
		{
			node = node->right;
		}
		else
		{
			node = node->left;
		}
	}
	if ( !node )
	{
		return 0;
//		Com_Error(ERR_DROP, "Illegal tree!\n");
	}
	return ( *ch = node->symbol );
}

/* Get a symbol */
void Huff_offsetReceive( node_t *node, int *ch, byte *fin, int *offset )
{
	bloc = *offset;
	while ( node && node->symbol == INTERNAL_NODE )
	{
		if ( get_bit( fin ) )
		{
			node = node->right;
		}
		else
		{
			node = node->left;
		}
	}
	if ( !node )
	{
		*ch = 0;
		return;
//		Com_Error(ERR_DROP, "Illegal tree!\n");
	}
	*ch = node->symbol;
	*offset = bloc;
}

/* Send the prefix code for this node */
static void send( node_t *node, node_t *child, byte *fout )
{
	if ( node->parent )
	{
		send( node->parent, node, fout );
	}
	if ( child )
	{
		if ( node->right == child )
		{
			add_bit( 1, fout );
		}
		else
		{
			add_bit( 0, fout );
		}
	}
}

/* Send a symbol */
void Huff_transmit( huff_t *huff, int ch, byte *fout )
{
	int i;
	if ( huff->loc[ch] == NULL )
	{
		/* node_t hasn't been transmitted, send a NYT, then the symbol */
		Huff_transmit( huff, NYT, fout );
		for ( i = 7; i >= 0; i-- )
		{
			add_bit( (char)( ( ch >> i ) & 0x1 ), fout );
		}
	}
	else
	{
		send( huff->loc[ch], NULL, fout );
	}
}

void Huff_offsetTransmit( huff_t *huff, int ch, byte *fout, int *offset )
{
	bloc = *offset;
	send( huff->loc[ch], NULL, fout );
	*offset = bloc;
}

void Huff_Decompress( msg_t *mbuf, int offset )
{
	int ch, cch, i, j, size;
	byte seq[65536];
	byte*       buffer;
	huff_t huff;

	size = mbuf->cursize - offset;
	buffer = mbuf->data + offset;

	if ( size <= 0 )
	{
		return;
	}

	Com_Memset( &huff, 0, sizeof( huff_t ) );
	// Initialize the tree & list with the NYT node
	huff.tree = huff.lhead = huff.ltail = huff.loc[NYT] = &( huff.nodeList[huff.blocNode++] );
	huff.tree->symbol = NYT;
	huff.tree->weight = 0;
	huff.lhead->next = huff.lhead->prev = NULL;
	huff.tree->parent = huff.tree->left = huff.tree->right = NULL;

	cch = buffer[0] * 256 + buffer[1];
	// don't overflow with bad messages
	if ( cch > mbuf->maxsize - offset )
	{
		cch = mbuf->maxsize - offset;
	}
	bloc = 16;

	for ( j = 0; j < cch; j++ )
	{
		ch = 0;
		// don't overflow reading from the messages
		// FIXME: would it be better to have a overflow check in get_bit ?
		if ( ( bloc >> 3 ) > size )
		{
			seq[j] = 0;
			break;
		}
		Huff_Receive( huff.tree, &ch, buffer );               /* Get a character */
		if ( ch == NYT )                                /* We got a NYT, get the symbol associated with it */
		{
			ch = 0;
			for ( i = 0; i < 8; i++ )
			{
				ch = ( ch << 1 ) + get_bit( buffer );
			}
		}

		seq[j] = ch;                                    /* Write symbol */

		Huff_addRef( &huff, (byte)ch );                               /* Increment node */
	}
	mbuf->cursize = cch + offset;
	Com_Memcpy( mbuf->data + offset, seq, cch );
}

extern int oldsize;

void Huff_Compress( msg_t *mbuf, int offset )
{
	int i, ch, size;
	byte seq[65536];
	byte*       buffer;
	huff_t huff;

	size = mbuf->cursize - offset;
	buffer = mbuf->data + + offset;

	if ( size <= 0 )
	{
		return;
	}

	Com_Memset( &huff, 0, sizeof( huff_t ) );
	// Add the NYT (not yet transmitted) node into the tree/list */
	huff.tree = huff.lhead = huff.loc[NYT] =  &( huff.nodeList[huff.blocNode++] );
	huff.tree->symbol = NYT;
	huff.tree->weight = 0;
	huff.lhead->next = huff.lhead->prev = NULL;
	huff.tree->parent = huff.tree->left = huff.tree->right = NULL;
	huff.loc[NYT] = huff.tree;

	seq[0] = ( size >> 8 );
	seq[1] = size & 0xff;

	bloc = 16;

	for ( i = 0; i < size; i++ )
	{
		ch = buffer[i];
		Huff_transmit( &huff, ch, seq );                      /* Transmit symbol */
		Huff_addRef( &huff, (byte)ch );                               /* Do update */
	}

	bloc += 8;                                              // next byte

	mbuf->cursize = ( bloc >> 3 ) + offset;
	Com_Memcpy( mbuf->data + offset, seq, ( bloc >> 3 ) );
}

void Huff_Init( huffman_t *huff )
{

	Com_Memset( &huff->compressor, 0, sizeof( huff_t ) );
	Com_Memset( &huff->decompressor, 0, sizeof( huff_t ) );

	// Initialize the tree & list with the NYT node
	huff->decompressor.tree = huff->decompressor.lhead = huff->decompressor.ltail = huff->decompressor.loc[NYT] = &( huff->decompressor.nodeList[huff->decompressor.blocNode++] );
	huff->decompressor.tree->symbol = NYT;
	huff->decompressor.tree->weight = 0;
	huff->decompressor.lhead->next = huff->decompressor.lhead->prev = NULL;
	huff->decompressor.tree->parent = huff->decompressor.tree->left = huff->decompressor.tree->right = NULL;

	// Add the NYT (not yet transmitted) node into the tree/list */
	huff->compressor.tree = huff->compressor.lhead = huff->compressor.loc[NYT] =  &( huff->compressor.nodeList[huff->compressor.blocNode++] );
	huff->compressor.tree->symbol = NYT;
	huff->compressor.tree->weight = 0;
	huff->compressor.lhead->next = huff->compressor.lhead->prev = NULL;
	huff->compressor.tree->parent = huff->compressor.tree->left = huff->compressor.tree->right = NULL;
	huff->compressor.loc[NYT] = huff->compressor.tree;
}
// END huffman.c from Enemy Territory

void MSG_initHuffman()
{
	int i,j;

	msgInit = qtrue;
	Huff_Init(&msgHuff);
	for(i=0; i<256; i++)
	{
		for (j=0; j<msg_hData[i]; j++)
		{
			Huff_addRef(&msgHuff.compressor,	(byte)i);			// Do update
			Huff_addRef(&msgHuff.decompressor,	(byte)i);			// Do update
		}
	}
}

void MSG_Init( msg_t *buf, byte *data, int length )
{
	if (!msgInit)
	{
		MSG_initHuffman();
	}
	buf->data = data;
	buf->maxsize = length;
	buf->overflowed = qfalse;
	buf->cursize = 0;
	buf->readcount = 0;
	buf->bit = 0;
}

int MSG_ReadBitsCompress(const byte* input, int readsize, byte* outputBuf, int outputBufSize)
{
	readsize = readsize * 8;
	byte *outptr = outputBuf;

	int get;
	int offset;
	int i;

	if(readsize <= 0)
	{
		return 0;
	}

	for(offset = 0, i = 0; offset < readsize && i < outputBufSize; i++)
	{
		Huff_offsetReceive( msgHuff.decompressor.tree, &get, (byte*)input, &offset);
		*outptr = (byte)get;
		outptr++;
	}
	return i;
}

int MSG_WriteBitsCompress( const byte *datasrc, byte *buffdest, int bytecount)
{

	int offset;
	int i;

	if(bytecount <= 0)
	{
		return 0;
	}

	for(offset = 0, i = 0; i < bytecount; i++)
	{
		Huff_offsetTransmit( &msgHuff.compressor, (int)datasrc[i], buffdest, &offset );
	}
	return (offset + 7) / 8;
}

int MSG_GetByte(msg_t *msg, int where)
{
	return msg->data[where];
}

int MSG_GetShort(msg_t *msg, int where)
{
	return *(int16_t*)&msg->data[where];
}

int MSG_GetLong(msg_t *msg, int where)
{
	return *(int32_t*)&msg->data[where];
}

int MSG_ReadByte( msg_t *msg )
{
	byte	c;

	if ( msg->readcount+sizeof(byte) > msg->cursize )
	{
		msg->overflowed = 1;
		return -1;
	}
	c = MSG_GetByte(msg, msg->readcount);
	msg->readcount += sizeof(byte);
	return c;
}

int MSG_ReadShort( msg_t *msg )
{
	int16_t	c;

	if ( msg->readcount+sizeof(short) > msg->cursize )
	{
		msg->overflowed = 1;
		return -1;
	}
	c = MSG_GetShort(msg, msg->readcount);
	msg->readcount += sizeof(short);
	return c;
}

int32_t MSG_ReadLong( msg_t *msg )
{
	int32_t	c;

	if ( msg->readcount+sizeof(int32_t) > msg->cursize )
	{
		msg->overflowed = 1;
		return -1;
	}
	c = MSG_GetLong(msg, msg->readcount);
	msg->readcount += sizeof(int32_t);
	return c;
}

#define BIG_INFO_STRING 8192
char *MSG_ReadBigString( msg_t *msg )
{
	static char string[BIG_INFO_STRING];
	int l,c;

	l = 0;
	do
	{
		c = MSG_ReadByte( msg );      // use ReadByte so -1 is out of bounds
		if ( c == -1 || c == 0 )
		{
			break;
		}
		// translate all fmt spec to avoid crash bugs
		if ( c == '%' )
		{
			c = '.';
		}

		string[l] = c;
		l++;
	}
	while ( l < sizeof( string ) - 1 );

	string[l] = 0;

	return string;
}

int MSG_ReadBits(msg_t* msg, int bits)
{
	int i;
	int bit;
	int value;

	value = 0;

	for (i = 0; i < bits; ++i)
	{
		bit = msg->bit & 7;

		if (!bit)
		{
			if (msg->readcount >= msg->cursize)
			{
				msg->overflowed = qtrue;
				return -1;
			}

			msg->bit = 8 * msg->readcount++;
		}

		value |= ((msg->data[msg->bit++ >> 3] >> bit) & 1) << i;
	}

	return value;
}

int MSG_ReadBit(msg_t* msg)
{
	int bit;

	bit = msg->bit & 7;

	if (!bit)
	{
		if (msg->readcount >= msg->cursize)
		{
			msg->overflowed = qtrue;
			return -1;
		}

		msg->bit = 8 * msg->readcount++;
	}

	return (msg->data[msg->bit++ >> 3] >> bit) & 1;
}

void MSG_ReadData(msg_t *msg, void *data, int len)
{
	int size = msg->readcount + len;

	if ( size > msg->cursize )
	{
		msg->overflowed = qtrue;
		Com_Memset(data, -1, len);
	}
	else
	{
		Com_Memcpy(data, &msg->data[msg->readcount], len);
		msg->readcount = size;
	}
}



void SHOWNET( msg_t *msg, char *s ) {
	Com_Printf( "%3i:%s\n", msg->readcount - 1, s );
}



qboolean    CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t    *clSnap;
	int i, count;

	if ( snapshotNumber > cl.snap.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.snapshot.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}


	snapshot->numEntities = count;
	for ( i = 0 ; i < count ; i++ ) {
		snapshot->entities[i] =
			cl.parseEntities[ ( clSnap->parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 ) ];
	}

	count = clSnap->numClients;
	if ( count > MAX_CLIENTS_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i clients to %i\n", count, MAX_CLIENTS_IN_SNAPSHOT );
		count = MAX_CLIENTS_IN_SNAPSHOT;
	}
	snapshot->numClients = count;
	for ( i = 0 ; i < count ; i++ ) {
		snapshot->clients[i] =
			cl.parseClients[ ( clSnap->parseClientsNum + i ) & ( MAX_PARSE_CLIENTS - 1 ) ];
	}

	// FIXME: configstring changes and server commands!!!

	return qtrue;
}







#if 0  // --- live-client snapshot interpolation / timing: unused by the offline parser; references symbols (cls, cl_timedemo, Sys_Milliseconds, CG_*) that don't exist outside a running client ---
/*
==================
CL_SetCGameTime
==================
*/
void CL_SetCGameTime( void ) {
	// getting a valid frame message ends the connection process
	/*if ( cls.state != CA_ACTIVE ) {
		if ( cls.state != CA_PRIMED ) {
			return;
		}
		if ( clc.demoplaying ) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if ( !clc.firstDemoFrameSkipped ) {
				clc.firstDemoFrameSkipped = qtrue;
				return;
			}
			CL_ReadDemoMessage();
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}
		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}	*/

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if ( !cl.snap.valid ) {
		Com_Error( ERR_DROP, "CL_SetCGameTime: !cl.snap.valid" );
	}

	// allow pause in single player
	/*if ( sv_paused->integer && cl_paused->integer && com_sv_running->integer ) {
		// paused
		return;
	}*/

	if ( cl.snap.serverTime < cl.oldFrameServerTime ) {
		Com_Error( ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.snap.serverTime;


	// get our current view of time

	/*if ( clc.demoplaying && cl_freezeDemo->integer ) {
		// cl_freezeDemo is used to lock a demo in place for single frame advances

	} else*/ {
		// cl_timeNudge is a user adjustable cvar that allows more
		// or less latency to be added in the interest of better 
		// smoothness or better responsiveness.
		int tn;
		
		tn = 0;//cl_timeNudge->integer;
		if (tn<-30) {
			tn = -30;
		} else if (tn>30) {
			tn = 30;
		}

		cl.serverTime = cls.realtime + cl.serverTimeDelta - tn;

		// guarantee that time will never flow backwards, even if
		// serverTimeDelta made an adjustment or cl_timeNudge was changed
		if ( cl.serverTime < cl.oldServerTime ) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		if ( cls.realtime + cl.serverTimeDelta >= cl.snap.serverTime - 5 ) {
			cl.extrapolatedSnapshot = qtrue;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}

	if ( !clc.demoplaying ) {
		return;
	}

	// if we are playing a demo back, we can just keep reading
	// messages from the demo file until the cgame definately
	// has valid snapshots to interpolate between

	// a timedemo will always use a deterministic set of time samples
	// no matter what speed machine it is run on,
	// while a normal demo may have different time samples
	// each time it is played back
	if ( cl_timedemo->integer ) {
		if (!clc.timeDemoStart) {
			clc.timeDemoStart = Sys_Milliseconds();
		}
		clc.timeDemoFrames++;
		cl.serverTime = clc.timeDemoBaseTime + clc.timeDemoFrames * 50;
	}

	while ( cl.serverTime >= cl.snap.serverTime ) {
		// feed another messag, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if ( cls.state != CA_ACTIVE ) {
			return;		// end of demo
		}
	}

}

#define CG_Printf Com_Printf
#define CG_Error(err) Com_Error(ERR_DROP, err)

#define trap_GetCurrentSnapshotNumber	CL_GetCurrentSnapshotNumber
#define trap_GetSnapshot	CL_GetSnapshot


void	CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = cl.snap.messageNum;
	*serverTime = cl.snap.serverTime;
}

static snapshot_t *CG_ReadNextSnapshot( void ) {
	qboolean	r;
	snapshot_t	*dest;

	if ( cg.latestSnapshotNum > cgs.processedSnapshotNum + 1000 ) {
		CG_Printf( "WARNING: CG_ReadNextSnapshot: way out of range, %i > %i", 
			cg.latestSnapshotNum, cgs.processedSnapshotNum );
	}

	while ( cgs.processedSnapshotNum < cg.latestSnapshotNum ) {
		// decide which of the two slots to load it into
		if ( cg.snap == &cg.activeSnapshots[0] ) {
			dest = &cg.activeSnapshots[1];
		} else {
			dest = &cg.activeSnapshots[0];
		}

		// try to read the snapshot from the client system
		cgs.processedSnapshotNum++;
		r = trap_GetSnapshot( cgs.processedSnapshotNum, dest );

		// FIXME: why would trap_GetSnapshot return a snapshot with the same server time
		if ( cg.snap && r && dest->serverTime == cg.snap->serverTime ) {
			//continue;
		}

		// if it succeeded, return
		if ( r ) {
			//CG_AddLagometerSnapshotInfo( dest );
			return dest;
		}

		// a GetSnapshot will return failure if the snapshot
		// never arrived, or  is so old that its entities
		// have been shoved off the end of the circular
		// buffer in the client system.

		// record as a dropped packet
		//CG_AddLagometerSnapshotInfo( NULL );

		// If there are additional snapshots, continue trying to
		// read them.
	}

	// nothing left to read
	return NULL;
}

void CG_ProcessSnapshots( void ) {
	snapshot_t		*snap;
	int				n;

	// see what the latest snapshot the client system has is
	trap_GetCurrentSnapshotNumber( &n, &cg.latestSnapshotTime );
	if ( n != cg.latestSnapshotNum ) {
		if ( n < cg.latestSnapshotNum ) {
			// this should never happen
			CG_Error( "CG_ProcessSnapshots: n < cg.latestSnapshotNum" );
		}
		cg.latestSnapshotNum = n;
	}

	// If we have yet to receive a snapshot, check for it.
	// Once we have gotten the first snapshot, cg.snap will
	// always have valid data for the rest of the game
	while ( !cg.snap ) {
		snap = CG_ReadNextSnapshot();
		if ( !snap ) {
			// we can't continue until we get a snapshot
			return;
		}

		// set our weapon selection to what
		// the playerstate is currently using
		if ( !( snap->snapFlags & SNAPFLAG_NOT_ACTIVE ) ) {
			CG_SetInitialSnapshot( snap );
		}
	}

	// loop until we either have a valid nextSnap with a serverTime
	// greater than cg.time to interpolate towards, or we run
	// out of available snapshots
	do {
		// if we don't have a nextframe, try and read a new one in
		if ( !cg.nextSnap ) {
			snap = CG_ReadNextSnapshot();

			// if we still don't have a nextframe, we will just have to
			// extrapolate
			if ( !snap ) {
				break;
			}

			CG_SetNextSnap( snap );


			// if time went backwards, we have a level restart
			if ( cg.nextSnap->serverTime < cg.snap->serverTime ) {
				CG_Error( "CG_ProcessSnapshots: Server time went backwards" );
			}
		}

		// if our time is < nextFrame's, we have a nice interpolating state
		if ( cg.time >= cg.snap->serverTime && cg.time < cg.nextSnap->serverTime ) {
			break;
		}

		// we have passed the transition from nextFrame to frame
		CG_TransitionSnapshot();
	} while ( 1 );

	// assert our valid conditions upon exiting
	if ( cg.snap == NULL ) {
		CG_Error( "CG_ProcessSnapshots: cg.snap == NULL" );
	}
	if ( cg.time < cg.snap->serverTime ) {
		// this can happen right after a vid_restart
		cg.time = cg.snap->serverTime;
	}
	if ( cg.nextSnap != NULL && cg.nextSnap->serverTime <= cg.time ) {
		CG_Error( "CG_ProcessSnapshots: cg.nextSnap->serverTime <= cg.time" );
	}

}
#endif  // --- end unused live-client interpolation block ---










float MSG_ReadAngle16(msg_t* msg)
{
	return SHORT2ANGLE((float)MSG_ReadShort(msg));
}

void MSG_ReadDeltaField(msg_t* msg, const void* from, const void* to, netField_t* field, qboolean print)
{
	int* fromF, * toF;
	int32_t readbits;
	int32_t readbyte;
	uint32_t unsignedbits;
	int i;

	fromF = (int32_t*)((byte*)from + field->offset);
	toF = (int32_t*)((byte*)to + field->offset);

	if (!MSG_ReadBit(msg))
	{
		g_fieldChangedBit = 0;
		*toF = *fromF;
		return;
	}
	g_fieldChangedBit = 1;

	switch (field->bits)
	{
	case 0:
		if (MSG_ReadBit(msg))
		{
			if (MSG_ReadBit(msg))
			{
				*toF = MSG_ReadLong(msg);
				if (print)
					Com_Printf("%s:%f ", field->name, *(float*)toF);
			}
			else
			{
				readbits = MSG_ReadBits(msg, 5);
				readbyte = 32 * MSG_ReadByte(msg) + readbits - 4096;
				*(float*)toF = (float)readbyte;
				if (print)
					Com_Printf("%s:%i ", field->name, readbyte);
			}
		}
		else
			*toF = 0;
		return;

	case -99:
		if (MSG_ReadBit(msg))
		{
			if (MSG_ReadBit(msg))
			{
				*toF = MSG_ReadLong(msg);
				if (print)
					Com_Printf("%s:%f ", field->name, *(float*)toF);
			}
			else
			{
				readbits = MSG_ReadBits(msg, 2);
				readbyte = 4 * MSG_ReadByte(msg) + readbits - 512;
				*(float*)toF = (float)readbyte;
				if (print)
					Com_Printf("%s:%i ", field->name, readbyte);
			}
		}
		else
			*toF = 0;
		return;

	case -100:
		if (MSG_ReadBit(msg))
		{
			*(float*)toF = MSG_ReadAngle16(msg);
			// present-bit=1 but the angle quantized to 0: recompute (which keys off *toF)
			// would drop the present bit, so flag this field for present-forced replay.
			if (*toF == 0)
				g_fieldChangedBit = 2;
		}
		else
			*toF = 0;
		return;

	default:
		if (MSG_ReadBit(msg))
		{
			unsignedbits = (unsigned int)field->bits >> 31;
			readbits = abs(field->bits);
			if ((readbits & 7) != 0)
				readbyte = MSG_ReadBits(msg, readbits & 7);
			else
				readbyte = 0;
			for (i = readbits & 7; i < readbits; i += 8)
				readbyte |= MSG_ReadByte(msg) << i;
			if (unsignedbits && ((readbyte >> (readbits - 1)) & 1) != 0)
				readbyte |= ~((1 << readbits) - 1);
			*toF = readbyte;
			if (print)
				Com_Printf("%s:%i ", field->name, *toF);
			// present-bit=1 but the transmitted low bits are 0 (e.g. a mod-256 counter
			// whose real value is a multiple of 256): recompute keys off *toF and would
			// drop the present bit, so flag this field for present-forced replay.
			if (*toF == 0)
				g_fieldChangedBit = 2;
		}
		else
			*toF = 0;
		return;
	}
}

void MSG_ReadDeltaStruct(msg_t* msg, const void* from, void* to, unsigned int number, int numFields, int indexBits, netField_t* stateFields)
{
	netField_t* field;
	int lc;
	int i;

	g_recStructLc = -1;   // -1 until the field-delta branch fills it (remove/no-delta = recompute)

	// check for a remove
	if (MSG_ReadBit(msg) == 1) {
		*(uint32_t*)to = (1 << indexBits) - 1;   // mark removed (1023 ents / 63 clients) so the caller drops it
		return;
	}

	// check for no delta
	if (MSG_ReadBit(msg) == 0)
	{
		//Com_Memcpy(to, from, 4 * numFields + 4);
		Com_Memcpy(to, from, (numFields + sizeof(intptr_t)) * sizeof(from));	
		return;
	}

	lc = MSG_ReadByte(msg);

	if (lc > numFields)
	{
		msg->overflowed = qtrue;
		return;
	}

	Com_Printf("%3i: #%-3i ", msg->readcount, *(uint32_t*)to);

	*(uint32_t*)to = number;

	// Record the original per-field encoding decisions for byte-1:1 replay.
	g_recStructLc = lc;
	Com_Memset( g_recStructChanged, 0, sizeof( g_recStructChanged ) );

	for (i = 0, field = stateFields; i < lc; i++, field++)
	{
		MSG_ReadDeltaField(msg, from, to, field, 1);
		if ( i < 128 )
			g_recStructChanged[ i ] = (byte)g_fieldChangedBit;
	}

	for (i = lc, field = &stateFields[lc]; i < numFields; i++, field++)
	{
		*(uint32_t*)((byte*)to + field->offset) = *(uint32_t*)((byte*)from + field->offset);
	}

	Com_Printf("\n");
}

void MSG_ReadDeltaEntity(msg_t* msg, entityState_t* from, entityState_t* to, int number)
{
	MSG_ReadDeltaStruct(msg, from, to, number, COUNT_OF(entityStateFields), 10, entityStateFields);
}

void MSG_ReadDeltaClient(msg_t *msg, clientState_t *from, clientState_t *to, int number)
{
	clientState_t nullstate;

	if ( !from )
	{
		from = &nullstate;
		Com_Memset(&nullstate, 0, sizeof(nullstate));
	}

	MSG_ReadDeltaStruct(msg, from, to, number, COUNT_OF(clientStateFields), 6, clientStateFields);
}



void MSG_ReadDeltaHudElems(msg_t *msg, hudelem_t *from, hudelem_t *to, int count)
{
	int inuse, lc, i, j, k;

	inuse = MSG_ReadBits(msg, 5);

	for ( i = 0; i < inuse; ++i )
	{
		lc = MSG_ReadBits(msg, 5);

		for ( j = 0; j <= lc; ++j )
		{
			MSG_ReadDeltaField(msg, &from[i], &to[i], &hudElemFields[j], 1);
		}

		for ( k = j; j < COUNT_OF(hudElemFields); ++j, ++k )
		{
			*(int32_t *)((byte *)(from + i) + hudElemFields[k].offset) = *(int32_t *)((byte *)(to + i) + hudElemFields[k].offset);
		}
	}

	while ( inuse < count && to[inuse].type )
	{
		memset(&to[inuse], 0, sizeof(hudelem_t));
		//assert(to[inuse].type == HE_TYPE_FREE);
		++inuse;
	}
}

void MSG_ReadDeltaObjective(msg_t *msg, objective_t *from, objective_t *to, int numFields, netField_t *objFields)
{
	int i = 0;

	if ( MSG_ReadBit(msg) )
	{
		for ( i = 0; i < numFields; i++ )
			MSG_ReadDeltaField(msg, from, to, &objFields[i], 1);
	}
	else
	{
		for ( i = 0; i < numFields; i++ )
		{
			*(int32_t *)((byte *)to + objFields[i].offset) = *(int32_t *)((byte *)from + objFields[i].offset);
		}
	}
}

void MSG_ReadDeltaPlayerstate(msg_t *msg, playerState_t *from, playerState_t *to)
{
	int *fromF, *toF;
	netField_t *field;
	playerState_t nullstate;
	int readbits;
	int readbyte;
	int bitstemp;
	uint32_t unsignedbits;
	int i, j, k, lc;

	if ( !from )
	{
		from = &nullstate;
		Com_Memset(&nullstate, 0, sizeof(nullstate));
	}

	Com_Memcpy(to, from, sizeof(playerState_t));

	lc = 0;

	qboolean print = qtrue; // only present in client aka cl_shownet

	Com_Printf("%3i: playerstate ", msg->readcount);


	lc = MSG_ReadByte( msg );

	// Record the original encoding decisions for byte-1:1 replay.
	g_recPsLc = lc;
	Com_Memset( g_recPsChanged, 0, sizeof( g_recPsChanged ) );

	for ( i = 0, field = playerStateFields ; i < lc ; i++, field++ )
	{
		fromF = ( int32_t * )( (byte *)from + field->offset );
		toF = ( int32_t * )( (byte *)to + field->offset );

		if ( !MSG_ReadBit(msg) )
		{
			*toF = *fromF;
			continue;
		}
		if ( i < (int)sizeof( g_recPsChanged ) )
			g_recPsChanged[ i ] = 1;   // original wrote this field as changed

		switch (field->bits)
		{
		case 0:
			if ( MSG_ReadBit(msg) )
			{
				*toF = MSG_ReadLong(msg);
				if ( print )
					Com_Printf("{0}%s:%f ", field->name, *(float *)toF);
			}
			else
			{
				readbits = MSG_ReadBits(msg, 5);
				readbyte = 32 * MSG_ReadByte(msg) + readbits - 4096;
				*(float *)toF = (float)readbyte;
				if ( print )
					Com_Printf("{1}%s:%i ", field->name, readbyte);
			}
			break;

		case -100:
			if ( MSG_ReadBit(msg) )
				*(float *)toF = MSG_ReadAngle16(msg);
			else
				*toF = 0;
			break;

		default:
			unsignedbits = (unsigned int)field->bits >> 31;
			readbits = abs(field->bits);
			if ( (readbits & 7) != 0 )
				readbyte = MSG_ReadBits(msg, readbits & 7);
			else
				readbyte = 0;
			for ( k = readbits & 7; k < readbits; k += 8 )
				readbyte |= MSG_ReadByte(msg) << k;
			if ( unsignedbits && ((readbyte >> (readbits - 1)) & 1) != 0 )
				readbyte |= ~((1 << readbits) - 1);
			*toF = readbyte;
			if ( print )
				Com_Printf("{2}%s:%i ", field->name, *toF);
			break;
		}
	}

	for ( i = lc, field = &playerStateFields[lc]; i < COUNT_OF(playerStateFields); i++, field++ )
	{
		fromF = ( int32_t * )( (byte *)from + field->offset );
		toF = ( int32_t * )( (byte *)to + field->offset );

		*toF = *fromF;
	}

	// stats
	int statsbits = 0;



	if ( MSG_ReadBits( msg, 1 ) ) {  // one general bit tells if any of this infrequently changing stuff has changed
		statsbits = MSG_ReadBits(msg, 6);

		
		Com_Printf( "%s ", "PS_STATS" );

		if ( (statsbits & 1) != 0 )
			to->stats[STAT_HEALTH] = MSG_ReadShort(msg);

		if ( (statsbits & 2) != 0 )
			to->stats[STAT_DEAD_YAW] = MSG_ReadShort(msg);

		if ( (statsbits & 4) != 0 )
			to->stats[STAT_MAX_HEALTH] = MSG_ReadShort(msg);

		if ( (statsbits & 8) != 0 )
			to->stats[STAT_IDENT_CLIENT_NUM] = MSG_ReadBits(msg, 6);

		if ( (statsbits & 0x10) != 0 )
			to->stats[STAT_IDENT_CLIENT_HEALTH] = MSG_ReadShort(msg);

		if ( (statsbits & 0x20) != 0 )
			to->stats[STAT_SPAWN_COUNT] = MSG_ReadByte(msg);
	}



	// ammo stored
	int ammobits = 0;
	if ( MSG_ReadBit(msg) )
	{
		for ( i = 0; i < 4; ++i )
		{
			if ( MSG_ReadBit(msg) )
			{
				Com_Printf( "%s ", "PS_AMMO" );
				ammobits = MSG_ReadShort(msg);

				for ( j = 0; j < 16; ++j )
				{
					if ( ((ammobits >> j) & 1) != 0 )
					{
						to->ammo[j + 16 * i] = MSG_ReadShort(msg);
					}
				}
			}
		}
	}
/*
	// ammo stored
	if ( MSG_ReadBits( msg, 1 ) ) {     // check for any ammo change (0-63)
		for ( j = 0; j < 4; j++ ) {
			if ( MSG_ReadBits( msg, 1 ) ) {
				Com_Printf( "PS_AMMO" );
				bits = MSG_ReadShort( msg );
				for ( i = 0 ; i < 16 ; i++ ) {
					if ( bits & ( 1 << i ) ) {
						to->ammo[i + ( j * 16 )] = MSG_ReadShort( msg );
					}
				}
			}
		}
	}
*/


	// ammo in clip
	int clipbits = 0;

	for ( i = 0; i < 4; ++i )
	{
		if ( MSG_ReadBit(msg) )
		{
			Com_Printf( "%s ", "PS_AMMOCLIP" );
			clipbits = MSG_ReadShort(msg);

			for ( j = 0; j < 16; ++j )
			{
				if ( ((clipbits >> j) & 1) != 0 )
				{
					to->ammoclip[j + 16 * i] = MSG_ReadShort(msg);
				}
			}
		}
	}
/*
	// ammo in clip
	for ( j = 0; j < 4; j++ ) {
		if ( MSG_ReadBits( msg, 1 ) ) {
			Com_Printf( "PS_AMMOCLIP" );
			bits = MSG_ReadShort( msg );
			for ( i = 0 ; i < 16 ; i++ ) {
				if ( bits & ( 1 << i ) ) {
					to->ammoclip[i + ( j * 16 )] = MSG_ReadShort( msg );
				}
			}
		}
	}
*/

	// Objectives
	if ( MSG_ReadBit(msg) )
	{
		for ( i = 0; i < MAX_OBJECTIVES; ++i )
		{
			to->objective[i].state = MSG_ReadBits(msg, 3);
			MSG_ReadDeltaObjective(msg, &from->objective[i], &to->objective[i], COUNT_OF(objectiveFields), objectiveFields);
		}
	}

	// Hud-elements
	if ( MSG_ReadBit(msg) )
	{
		MSG_ReadDeltaHudElems(msg, from->hud.archival, to->hud.archival, MAX_HUDELEMS_ARCHIVAL);
		MSG_ReadDeltaHudElems(msg, from->hud.current, to->hud.current, MAX_HUDELEMS_CURRENT);
	}

	Com_Printf("\n");
}













void CL_DeltaEntity( msg_t *msg, clSnapshot_t *frame, int newnum, entityState_t *old,
					 qboolean unchanged ) {
	
	entityState_t   *state = NULL;

	// save the parsed entity state into the big circular buffer so
	// it can be used as the source for a later delta
	int slot = cl.parseEntitiesNum & ( MAX_PARSE_ENTITIES - 1 );
	state = &cl.parseEntities[slot];


	if ( unchanged ) {
		*state = *old;
		g_entEnc[slot].valid = 0;
	} else {
		MSG_ReadDeltaEntity( msg, old, state, newnum );
		// Record the original encoding decisions alongside the decoded entity.
		g_entEnc[slot].valid = ( g_recStructLc >= 0 );
		if ( g_entEnc[slot].valid )
		{
			g_entEnc[slot].lc = g_recStructLc;
			Com_Memcpy( g_entEnc[slot].changed, g_recStructChanged, sizeof( g_entEnc[slot].changed ) );
		}
	}

	if ( state->number == ( MAX_GENTITIES - 1 ) ) {
		return;     // entity was delta removed
	}

	cl.parseEntitiesNum++;
	frame->numEntities++;
}



void CL_DeltaClient( msg_t *msg, clSnapshot_t *frame, int newnum, clientState_t *old,
					 qboolean unchanged ) {
	
	clientState_t   *state = NULL;

	// save the parsed entity state into the big circular buffer so
	// it can be used as the source for a later delta
	int slot = cl.parseClientsNum & ( MAX_PARSE_CLIENTS - 1 );
	state = &cl.parseClients[slot];


	if ( unchanged ) {
		*state = *old;
		g_cliEnc[slot].valid = 0;
	} else {
		MSG_ReadDeltaClient( msg, old, state, newnum );
		g_cliEnc[slot].valid = ( g_recStructLc >= 0 );
		if ( g_cliEnc[slot].valid )
		{
			g_cliEnc[slot].lc = g_recStructLc;
			Com_Memcpy( g_cliEnc[slot].changed, g_recStructChanged, sizeof( g_cliEnc[slot].changed ) );
		}
	}

	// asi neni
	if ( state->number == ( MAX_GCLIENTS - 1 ) ) {
		return;     // entity was delta removed
	}

	cl.parseClientsNum++;
	frame->numClients++;
}



void CL_ParsePacketEntities( msg_t *msg, clSnapshot_t *oldframe, clSnapshot_t *newframe ) {
	int newnum;
	entityState_t   *oldstate;
	int oldindex, oldnum;

	newframe->parseEntitiesNum = cl.parseEntitiesNum;
	newframe->numEntities = 0;

	// delta from the entities present in oldframe
	oldindex = 0;
	oldstate = NULL;
	if ( !oldframe ) {
		oldnum = 99999;
	} else {
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseEntities[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}

	while ( 1 ) {
		// read the entity index number
		newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );

		if ( newnum == ( MAX_GENTITIES - 1 ) ) {
			break;
		}

		if ( msg->readcount > msg->cursize ) {
			Com_Error( ERR_DROP,"CL_ParsePacketEntities: end of message" );
		}

		while ( oldnum < newnum ) {
			// one or more entities from the old packet are unchanged
			//if ( cl_shownet->integer == 3 ) {
				//Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
			//}
			CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseEntities[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
		}
		if ( oldnum == newnum ) {
			// delta from previous state
			//if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  delta: %i\n", msg->readcount, newnum );
			//}
			CL_DeltaEntity( msg, newframe, newnum, oldstate, qfalse );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseEntities[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
			continue;
		}

		if ( oldnum > newnum ) {
			// delta from baseline
			//if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  baseline: %i\n", msg->readcount, newnum );
			//}
			CL_DeltaEntity( msg, newframe, newnum, &cl.entityBaselines[newnum], qfalse );
			continue;
		}

	}

	// any remaining entities in the old frame are copied over
	while ( oldnum != 99999 ) {
		// one or more entities from the old packet are unchanged
		//if ( cl_shownet->integer == 3 ) {
			//Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
		//}
		CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

		oldindex++;

		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseEntities[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}

	//if ( cl_shownuments->integer ) {
		Com_Printf( "Entities in packet: %i\n", newframe->numEntities );
	//}
}

// Unknown func
void CL_ParsePacketClients( msg_t *msg, clSnapshot_t *oldframe, clSnapshot_t *newframe )
{
	int newnum, clientAvaible;
	clientState_t   *oldstate;
	int oldindex, oldnum;

	newframe->parseClientsNum = cl.parseClientsNum;
	newframe->numClients = 0;

	// delta from the Clients present in oldframe
	oldindex = 0;
	oldstate = NULL;
	if ( !oldframe ) {
		oldnum = 99999;
	} else {
		if ( oldindex >= oldframe->numClients ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseClients[
				( oldframe->parseClientsNum + oldindex ) & ( MAX_PARSE_CLIENTS - 1 )];
			oldnum = oldstate->number;
		}
	}

	while ( 1 ) {

		// Is next client avaivle
		clientAvaible = MSG_ReadBit( msg );
      	if (clientAvaible == 0) {
			break;
		}

		// read the entity index number
		newnum = MSG_ReadBits( msg, GCLIENTNUM_BITS );

		// toto tam neni
		/*if ( newnum == ( MAX_GCLIENTS - 1 ) ) {
			break;
		}*/

		if ( msg->readcount > msg->cursize ) {
			Com_Error( ERR_DROP,"CL_ParsePacketClients: end of message" );
		}

		while ( oldnum < newnum ) {
			// one or more entities from the old packet are unchanged
			//if ( cl_shownet->integer == 3 ) {
				//Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
			//}
			CL_DeltaClient( msg, newframe, oldnum, oldstate, qtrue );

			oldindex++;

			if ( oldindex >= oldframe->numClients ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseClients[
					( oldframe->parseClientsNum + oldindex ) & ( MAX_PARSE_CLIENTS - 1 )];
				oldnum = oldstate->number;
			}
		}
		if ( oldnum == newnum ) {
			// delta from previous state
			//if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  delta: %i\n", msg->readcount, newnum );
			//}

			// [SOMETHING MISSING HRER]
			/*
				puVar6 = local_d0;
				if (local_d0 == (uint *)0x0) {
					puVar6 = local_c8;
					puVar7 = puVar6;
					for (iVar4 = 0x17; iVar4 != 0; iVar4 = iVar4 + -1) {
					*puVar7 = 0;
					puVar7 = puVar7 + 1;
					}
				}
			*/


			CL_DeltaClient( msg, newframe, newnum, oldstate, qfalse );

			oldindex++;

			if ( oldindex >= oldframe->numClients ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseClients[
					( oldframe->parseClientsNum + oldindex ) & ( MAX_PARSE_CLIENTS - 1 )];
				oldnum = oldstate->number;
			}
			continue;
		}

		if ( oldnum > newnum ) {
			// delta from baseline
			//if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  baseline: %i\n", msg->readcount, newnum );
			//}

			// [SOMETHING MISSING HRER]
			/*
				puVar8 = local_68;
				for (iVar4 = 0x17; iVar4 != 0; iVar4 = iVar4 + -1) {
					*puVar8 = 0;
					puVar8 = puVar8 + 1;
				}
			*/


			CL_DeltaClient( msg, newframe, newnum, &cl.clientBaselines[newnum], qfalse );
			continue;
		}

	}

	// any remaining entities in the old frame are copied over
	while ( oldnum != 99999 ) {
		// one or more entities from the old packet are unchanged
		//if ( cl_shownet->integer == 3 ) {
			//Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
		//}
		CL_DeltaClient( msg, newframe, oldnum, oldstate, qtrue );

		oldindex++;

		if ( oldindex >= oldframe->numClients ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseClients[
				( oldframe->parseClientsNum + oldindex ) & ( MAX_PARSE_CLIENTS - 1 )];
			oldnum = oldstate->number;
		}
	}

	//if ( cl_shownuments->integer ) {
		Com_Printf( "Clients in packet: %i\n", newframe->numClients );
	//}
}




void CL_ParseSnapshot( msg_t *msg ) {
	int len;
	clSnapshot_t    *old;
	clSnapshot_t newSnap;
	int deltaNum;
	int oldMessageNum;
	int i, packetNum;


	// get the reliable sequence acknowledge number
	// NOTE: now sent with all server to client messages
	//clc.reliableAcknowledge = MSG_ReadLong( msg );

	// read in the new snapshot to a temporary buffer
	// we will only copy to cl.snap if it is valid
	memset( &newSnap, 0, sizeof( newSnap ) );

	// we will have read any new server commands in this
	// message before we got to svc_snapshot
	newSnap.serverCommandNum = clc.serverCommandSequence;

	newSnap.serverTime = MSG_ReadLong( msg );

	newSnap.messageNum = clc.serverMessageSequence;

	deltaNum = MSG_ReadByte( msg );
	if ( !deltaNum ) {
		newSnap.deltaNum = -1;
	} else {
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	newSnap.snapFlags = MSG_ReadByte( msg );

	// If the frame is delta compressed from data that we
	// no longer have available, we must suck up the rest of
	// the frame, but not use it, then ask for a non-compressed
	// message
	if ( newSnap.deltaNum <= 0 ) {
		newSnap.valid = qtrue;      // uncompressed frame
		old = NULL;
	} else {
		
		old = &cl.snapshots[newSnap.deltaNum & PACKET_MASK];
		if ( !old->valid ) {
			// should never happen
			Com_Printf( "Delta from invalid frame (not supposed to happen!).\n" );
		} else if ( old->messageNum != newSnap.deltaNum ) {
			// The frame that the server did the delta from
			// is too old, so we can't reconstruct it properly.
			Com_DPrintf( "Delta frame too old.\n" );
		} else if ( cl.parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES - 128 ) {
			Com_DPrintf( "Delta parseEntitiesNum too old.\n" );
		} else {
			newSnap.valid = qtrue;  // valid delta parse
		}
	}

	// read areamask
	//P len = MSG_ReadByte( msg );

	//P
	//if ( len > sizeof( newSnap.areamask ) ) {
	//	Com_Error( ERR_DROP,"CL_ParseSnapshot: Invalid size %d for areamask.", len );
	//	return;
	//}

	//P MSG_ReadData( msg, &newSnap.areamask, len );

	// read playerinfo
	SHOWNET( msg, "playerstate" );
	if ( old ) {
		MSG_ReadDeltaPlayerstate( msg, &old->ps, &newSnap.ps );
	} else {
		MSG_ReadDeltaPlayerstate( msg, NULL, &newSnap.ps );
	}
	// Capture the original playerstate encoding decisions for byte-1:1 replay.
	newSnap.psLc = g_recPsLc;
	Com_Memcpy( newSnap.psFieldChanged, g_recPsChanged, sizeof( newSnap.psFieldChanged ) );

	// read packet entities
	SHOWNET( msg, "packet entities" );
	CL_ParsePacketEntities( msg, old, &newSnap );



	// New by eyza
	SHOWNET( msg, "packet clients" );

  	CL_ParsePacketClients(msg, old, &newSnap);


	// if not valid, dump the entire thing now that it has
	// been properly read
	if ( !newSnap.valid ) {
		return;
	}


	// clear the valid flags of any snapshots between the last
	// received and this one, so if there was a dropped packet
	// it won't look like something valid to delta from next
	// time we wrap around in the buffer
	oldMessageNum = cl.snap.messageNum + 1;

	
	if ( newSnap.messageNum - oldMessageNum >= PACKET_BACKUP ) {
		oldMessageNum = newSnap.messageNum - ( PACKET_BACKUP - 1 );
	}
	for ( ; oldMessageNum < newSnap.messageNum ; oldMessageNum++ ) {
		cl.snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
	}


	// copy to the current good spot
	cl.snap = newSnap;
	cl.snap.ping = 999;

	// HUD / score-popup edits must happen here, before cl.snap is stored as the next
	// frame's delta base below — so the base and the emitted frame stay in sync.
	if ( g_removeHud || g_scaleScore )
		EditPlayerstateHud( &cl.snap.ps );
	// calculate ping time
	/*Pfor ( i = 0 ; i < PACKET_BACKUP ; i++ ) {
		packetNum = ( clc.netchan.outgoingSequence - 1 - i ) & PACKET_MASK;
		if ( cl.snap.ps.commandTime >= cl.outPackets[ packetNum ].p_serverTime ) {
			cl.snap.ping = cls.realtime - cl.outPackets[ packetNum ].p_realtime;
			break;
		}
	}*/
	// save the frame off in the backup array for later delta comparisons
	cl.snapshots[cl.snap.messageNum & PACKET_MASK] = cl.snap;

	//if ( cl_shownet->integer == 3 ) {
		Com_Printf( "   snapshot:%i  delta:%i  ping:%i\n", cl.snap.messageNum,
					cl.snap.deltaNum, cl.snap.ping );
	//}

	cl.newSnapshots = qtrue;
}















void CL_ParseGamestate( msg_t *msg )
{
	int i;
	entityState_t   *es;
	int newnum;
	entityState_t nullstate;
	int cmd;
	char            *s;

	// a gamestate always marks a server command sequence
	clc.serverCommandSequence = MSG_ReadLong( msg );

	// parse all the configstrings and baselines
	cl.gameState.dataCount = 1; // leave a 0 at the beginning for uninitialized configstrings
	while ( 1 )
	{
		cmd = MSG_ReadByte( msg );

		if ( cmd == svc_EOF )
		{
			break;
		}

		if ( cmd == svc_configstring )
		{
			int len;

			i = MSG_ReadShort( msg );
			if ( i < 0 || i >= MAX_CONFIGSTRINGS )
			{
				Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );
			}
			s = MSG_ReadBigString( msg );

			len = strlen( s );

			Com_Printf("    configstring[%i]: %s\n", i, s);
			//Com_Printf("%s\n", s);

			if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS )
			{
				Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
			}

			// append it to the gameState string buffer
			cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
			memcpy( cl.gameState.stringData + cl.gameState.dataCount, s, len + 1 );
			cl.gameState.dataCount += len + 1;

		}
		else if ( cmd == svc_baseline )
		{
			newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
			if ( newnum < 0 || newnum >= MAX_GENTITIES )
			{
				Com_Error( ERR_DROP, "Baseline number out of range: %i", newnum );
			}
			memset( &nullstate, 0, sizeof( nullstate ) );
			es = &cl.entityBaselines[ newnum ];
			MSG_ReadDeltaEntity( msg, &nullstate, es, newnum );
		}
		else
		{
			Com_Error( ERR_DROP, "CL_ParseGamestate: bad command byte" );
		}
	}

	clc.clientNum = MSG_ReadLong( msg );
	// read the checksum feed
	clc.checksumFeed = MSG_ReadLong( msg );
}



/*
=====================
CL_ParseCommandString

Command strings are just saved off until cgame asks for them
when it transitions a snapshot
=====================
*/
void CL_ParseCommandString( msg_t *msg ) {
	char	*s;
	int		seq;
	int		index;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadBigString( msg );

	// see if we have already executed stored it off
	if ( clc.serverCommandSequence >= seq ) {
		return;
	}
	clc.serverCommandSequence = seq;

	index = seq & (MAX_RELIABLE_COMMANDS-1);
	Q_strncpyz( clc.serverCommands[ index ], s, sizeof( clc.serverCommands[ index ] ) );

	// Engine verbs (CoD2rev SV_GameSendServerCommand call sites):
	//   h = public chat, i = team chat   ("\x15<name>^<color><message>")
	//   e = allClientsPrint, f = iprintln, g = iprintlnbold  (announcements;
	//       payload starts with \x15 for literal text, bare = localization key)
	//   H = allies team score, G = axis team score   ("H <int>")
	//   v = set client cvar, cs handled elsewhere.
	int isChat     = ( s[ 0 ] == 'h' || s[ 0 ] == 'i' ) && s[ 1 ] == ' ';
	int isAnnounce = ( s[ 0 ] == 'e' || s[ 0 ] == 'f' || s[ 0 ] == 'g' ) && s[ 1 ] == ' ';
	int isScore    = ( s[ 0 ] == 'H' || s[ 0 ] == 'G' ) && s[ 1 ] == ' ';

	if ( ( isChat || isAnnounce ) && ( g_dumpCommands || g_collectEvents ) )
	{
		char clean[ 1024 ]; int j = 0;
		for ( const char *p = s + 2; *p && j < (int)sizeof( clean ) - 1; p++ )
		{
			if ( *p == '"' || *p == 0x15 ) continue;       // quotes + literal-text marker
			if ( *p == 0x14 )                              // localized-key marker:
			{
				if ( isAnnounce && j ) clean[ j++ ] = ' '; // separate key from preceding text
				continue;                                  // in chat it sits inside (GAME_DEAD)
			}
			clean[ j++ ] = *p;
		}
		clean[ j ] = 0;

		if ( g_dumpCommands && isChat )
			printf( "[%8d] CHAT%s %s\n", cl.snap.serverTime, s[ 0 ] == 'i' ? "(team)" : "      ", clean );
		else if ( g_dumpCommands )
			printf( "[%8d] #%d  %s\n", cl.snap.serverTime, seq, s );

		if ( g_collectEvents && g_ovNumEvents < OV_MAX_EVENTS )
		{
			if ( isChat ) g_ovEvents[ g_ovNumEvents ].kind = ( s[ 0 ] == 'i' ) ? OV_TEAMCHAT : OV_CHAT;
			else          g_ovEvents[ g_ovNumEvents ].kind = OV_ANNOUNCE;
			g_ovEvents[ g_ovNumEvents ].verb = s[ 0 ];   // e/f/g for announces; h/i for chat
			Q_strncpyz( g_ovEvents[ g_ovNumEvents ].text, clean, sizeof( g_ovEvents[ 0 ].text ) );
			g_ovNumEvents++;
		}
	}
	else if ( isScore && g_collectEvents )
	{
		if ( g_ovNumEvents < OV_MAX_EVENTS )
		{
			g_ovEvents[ g_ovNumEvents ].kind  = ( s[ 0 ] == 'H' ) ? OV_SCORE_ALLIES : OV_SCORE_AXIS;
			g_ovEvents[ g_ovNumEvents ].value = atoi( s + 2 );
			g_ovEvents[ g_ovNumEvents ].text[ 0 ] = 0;
			g_ovNumEvents++;
		}
		if ( g_dumpCommands )
			printf( "[%8d] #%d  %s\n", cl.snap.serverTime, seq, s );
	}
	else if ( g_dumpCommands )
	{
		printf( "[%8d] #%d  %s\n", cl.snap.serverTime, seq, s );
	}
}


int CL_ParseServerMessage( msg_t *msg )
{
	int cmd;
	byte buffer[MAX_MSGLEN];
	msg_t decompressMsg;

	Com_Printf("------------------\n");

	// get the reliable sequence acknowledge number
	clc.reliableAcknowledge = MSG_ReadLong( msg );

	MSG_Init(&decompressMsg, buffer, sizeof(buffer));
	decompressMsg.cursize = MSG_ReadBitsCompress(msg->data + msg->readcount, msg->cursize - msg->readcount, decompressMsg.data, decompressMsg.maxsize);

	//
	// parse the message
	//
	while ( 1 )
	{
		if ( decompressMsg.readcount > decompressMsg.cursize )
		{
			Com_Error( ERR_DROP,"CL_ParseServerMessage: read past end of server message" );
			return 0;
		}

		cmd = MSG_ReadByte( &decompressMsg );

		if ( cmd == svc_EOF )
		{
			Com_Printf( "%3i:%s\n", msg->readcount - 1, "END OF MESSAGE\n" );
			return 1;
		}

		if ( !svc_strings[cmd] )
		{
			Com_Printf( "%3i:BAD CMD %i\n", decompressMsg.readcount - 1, cmd );
		}
		else
		{
			Com_Printf( "%3i:%s\n", decompressMsg.readcount - 1, svc_strings[cmd] );
		}

		// other commands
		switch ( cmd )
		{
		default:
			Com_Error( ERR_DROP,"CL_ParseServerMessage: Illegible server message %d\n", cmd );
			return 0;

		case svc_nop:
			Com_Printf("svc_nop\n");
			break;
		case svc_serverCommand:
			Com_Printf("svc_serverCommand\n");
			CL_ParseCommandString( &decompressMsg );
			break;
		case svc_gamestate:
			//Com_Printf("svc_gamestate\n");
			CL_ParseGamestate( &decompressMsg );
			break;
		case svc_snapshot:
			Com_Printf("svc_snapshot\n");
			CL_ParseSnapshot( &decompressMsg );
			break;
		case svc_download:
			Com_Printf("svc_download\n");
			break;
		}
	}

	return 0;
}

int FS_Read( void *buffer, int len, FILE *f )
{
	return fread( buffer, 1, len, f );
}

int CL_ReadDemoMessage( void )
{
	int r;
	msg_t buf;
	byte bufData[ MAX_MSGLEN ];
	int s;

	if ( !demo.demofile )
	{
		return 0;
	}

	// get the sequence number
	r = FS_Read( &s, 4, demo.demofile );
	if ( r != 4 )
	{
		return 0;
	}

	clc.serverMessageSequence = LittleLong( s );

	// init the message
	MSG_Init( &buf, bufData, sizeof( bufData ) );

	// get the length
	r = FS_Read( &buf.cursize, 4, demo.demofile );

	if ( r != 4 )
	{
		return 0;
	}
	buf.cursize = LittleLong( buf.cursize );
	if ( buf.cursize == -1 )
	{
		return 0;
	}
	if ( buf.cursize > buf.maxsize )
	{
		Com_Error( ERR_DROP, "CL_ReadDemoMessage: demoMsglen > MAX_MSGLEN" );
	}
	r = FS_Read( buf.data, buf.cursize, demo.demofile );
	if ( r != buf.cursize )
	{
		Com_Printf( "Demo file was truncated.\n" );
		return 0;
	}

	return CL_ParseServerMessage( &buf );
}

// Zero the decoder's global state so a fresh demo doesn't inherit the previous
// one's parse rings / snapshots / command sequence (needed for batch runs, where
// several demos are processed in one process).
void CL_ResetState( void )
{
	Com_Memset( &cl, 0, sizeof( cl ) );
	Com_Memset( &clc, 0, sizeof( clc ) );
}

int FS_FOpenFileRead( const char *filename, FILE **file, qboolean uniqueFILE )
{
	CL_ResetState();
	*file = fopen( filename, "rb" );
	return ( *file != NULL );
}

// --verify section attribution: byte offsets in the re-encoded snapshot where each
// section begins (set by SV_WriteSnapshot). Lets --verify name which section a
// divergence falls in (playerstate / entities / clients) instead of a raw offset.
static int g_secPlayerstate = -1, g_secEntities = -1, g_secClients = -1;

// --verify field attribution: when g_attrOn, the encoders record a marker (byte offset,
// label, entity/field) at each entity boundary and each written field, so --verify can
// re-transcode the one divergent frame and map the divergent byte to the exact entity+field.
#define ATTR_MAX 8192
typedef struct { int off; int byteoff; const char *label; int num; } attrMark_t;
static attrMark_t g_attr[ ATTR_MAX ];
static int g_attrN = 0, g_attrOn = 0;
static int g_attrCursize = 0;   // set by encoders to the current cursize when AttrMark fires
static inline void AttrMark( int off, const char *label, int num )
{
	if ( g_attrOn && g_attrN < ATTR_MAX ) { g_attr[ g_attrN ].off = off; g_attr[ g_attrN ].byteoff = g_attrCursize; g_attr[ g_attrN ].label = label; g_attr[ g_attrN ].num = num; g_attrN++; }
}

// The encoder (inverse of the decoder above). Pulled in here so it sees the
// field tables, msg_t and the MSG_Read* helpers defined earlier in this TU.
#include "writer.h"

// Killfeed icons (base64 PNG) + the CoD2 weapon-name -> icon map, for the --overview
// HTML page. Auto-generated by scripts/gen_icons_header.py from assets/icons/.
#include "icons_embed.h"

// ======================================================================
//  CoD-DemoTool — CLI front end
//  Everything above this line is the seeded, proven CoD2-DemoParser decoder.
//  Below is our tool's command surface.
// ======================================================================

// Return a parsed configstring by index, or "" if it was never set.
const char *CL_ConfigString( int index )
{
	if ( index < 0 || index >= MAX_CONFIGSTRINGS )
		return "";
	if ( cl.gameState.stringOffsets[ index ] == 0 )
		return "";
	return cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
}

// Extract the value for a key from a "\key\value\key\value" info string.
// CoD2 serverinfo (protocol, mapname, ...) lives in configstring[0].
const char *Info_ValueForKey( const char *s, const char *key )
{
	// Rotate through several buffers so callers can hold multiple results
	// live at once (e.g. version + map + gametype in one printf) without aliasing.
	static char value[ 8 ][ MAX_STRING_CHARS ];
	static int valueIndex = 0;
	char pkey[ MAX_STRING_CHARS ];
	char *out, *o;

	if ( !s || !key )
		return "";

	valueIndex = ( valueIndex + 1 ) & 7;
	out = value[ valueIndex ];
	out[ 0 ] = 0;

	if ( *s == '\\' )
		s++;

	while ( 1 )
	{
		o = pkey;
		while ( *s != '\\' )
		{
			if ( !*s )
				return "";
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = out;
		while ( *s != '\\' && *s )
			*o++ = *s++;
		*o = 0;

		if ( !strcasecmp( key, pkey ) )
			return out;

		if ( !*s )
			return "";
		s++;
	}
}

// Set/replace a key's value in a "\k\v\k\v" info string, in place (dstsize bounded).
// Used by --convert to rewrite protocol/shortversion in configstring[0] on emit.
void Info_SetValueForKey( char *s, int dstsize, const char *key, const char *value )
{
	char out[ MAX_STRING_CHARS * 2 ]; int j = 0;
	const char *p = s;
	int wrote = 0;
	if ( *p == '\\' ) p++;
	while ( *p )
	{
		char k[ MAX_STRING_CHARS ]; int ki = 0;
		while ( *p && *p != '\\' && ki < (int)sizeof( k ) - 1 ) k[ ki++ ] = *p++;
		k[ ki ] = 0;
		if ( *p == '\\' ) p++;
		char v[ MAX_STRING_CHARS ]; int vi = 0;
		while ( *p && *p != '\\' && vi < (int)sizeof( v ) - 1 ) v[ vi++ ] = *p++;
		v[ vi ] = 0;
		if ( *p == '\\' ) p++;
		const char *useV = v;
		if ( !strcasecmp( k, key ) ) { useV = value; wrote = 1; }
		j += snprintf( out + j, sizeof( out ) - j, "\\%s\\%s", k, useV );
	}
	if ( !wrote )
		j += snprintf( out + j, sizeof( out ) - j, "\\%s\\%s", key, value );
	Q_strncpyz( s, out, dstsize );
}

// Open a demo and run the decode loop. `dump` controls whether the verbose
// per-frame state is written to <demo>.log. Out-params summarise the demo.
// Returns 0 on success.
static int DecodeDemo( const char *path, bool dump,
                       int *outFrames, int *outFirstTime, int *outLastTime,
                       bool seenClients[ MAX_CLIENTS ] )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.log", path );
	FILE *fp = fopen( logFileName, "w" );   // truncate/clear the log
	if ( fp ) fclose( fp );

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	int frames = 0, firstTime = -1, lastTime = 0;

	while ( CL_ReadDemoMessage() )
	{
		snapshot_t snapshot;
		if ( !CL_GetSnapshot( cl.snap.messageNum, &snapshot ) )
			continue;

		frames++;
		if ( firstTime < 0 )
			firstTime = cl.snap.serverTime;
		if ( cl.snap.serverTime > lastTime )      // max, not last: serverTime can reset
			lastTime = cl.snap.serverTime;        // mid-demo (map restart) and go backwards

		for ( int i = 0; i < snapshot.numClients; i++ )
		{
			int n = snapshot.clients[ i ].number;
			if ( n >= 0 && n < MAX_CLIENTS )
				seenClients[ n ] = true;
		}

		if ( dump )
		{
			Com_Printf( "-------------------------\n" );
			Com_Printf( "serverTime: %i  cmdTime: %i  deltaTime: %i  Clients: %i\n",
				cl.snap.serverTime, cl.snap.ps.commandTime, cl.snap.ps.deltaTime, snapshot.numClients );
			for ( int i = 0; i < snapshot.numClients; i++ )
			{
				clientState_t client = snapshot.clients[ i ];
				Com_Printf( "number: %i, name: %s, team: %i\n", client.number, client.name, client.team );
			}
			playerState_t ps = snapshot.ps;
			Com_Printf( "Playerstate: clientNum: %3i, weapon: %2i, origin: %f %f %f\n",
				ps.clientNum, ps.weapon, ps.origin[ 0 ], ps.origin[ 1 ], ps.origin[ 2 ] );
			for ( int e = 0; e < cl.snap.numEntities; e++ )
			{
				entityState_t *es = &cl.parseEntities[ ( cl.snap.parseEntitiesNum + e ) & ( MAX_PARSE_ENTITIES - 1 ) ];
				Com_Printf( "ENT st=%i ne=%i num=%i eType=%i loop=%i time=%i time2=%i trTime=%i\n",
					cl.snap.serverTime, cl.snap.numEntities, es->number, es->eType, es->loopSound,
					es->time, es->time2, es->pos.trTime );
			}
		}
	}

	fclose( demo.demofile );
	demo.demofile = NULL;

	if ( outFrames )    *outFrames = frames;
	if ( outFirstTime ) *outFirstTime = firstTime;
	if ( outLastTime )  *outLastTime = lastTime;
	return 0;
}

// --info : print a friendly one-glance summary of what a demo is.
static int Cmd_Info( const char *path )
{
	int frames = 0, firstTime = -1, lastTime = 0;
	bool seen[ MAX_CLIENTS ];
	memset( seen, 0, sizeof( seen ) );

	// --info is just a summary — suppress the verbose per-frame log (otherwise a single
	// --info writes a ~28 MB <demo>.log and takes ~14 s instead of a fraction of a second;
	// this is what made a double-clicked .exe "do nothing" for ages). --dump is the
	// command for the full log.
	g_quietLog = 1;
	if ( DecodeDemo( path, false, &frames, &firstTime, &lastTime, seen ) != 0 )
	{
		g_quietLog = 0;
		return 1;
	}
	g_quietLog = 0;

	const char *si    = CL_ConfigString( 0 );          // serverinfo
	const char *sver  = Info_ValueForKey( si, "shortversion" );
	const char *proto = Info_ValueForKey( si, "protocol" );
	const char *map   = Info_ValueForKey( si, "mapname" );
	const char *gt    = Info_ValueForKey( si, "g_gametype" );
	const char *host  = Info_ValueForKey( si, "sv_hostname" );

	int players = 0;
	for ( int i = 0; i < MAX_CLIENTS; i++ )
		if ( seen[ i ] ) players++;

	int ms   = ( firstTime < 0 ) ? 0 : ( lastTime - firstTime );
	int secs = ms / 1000;

	printf( "\n" );
	printf( "  %s\n", path );
	printf( "  CoD2 %s (protocol %s)\n", *sver ? sver : "?", *proto ? proto : "?" );
	printf( "  map %s   gametype %s\n", *map ? map : "?", *gt ? gt : "?" );
	printf( "  length %d:%02d   frames %d   players %d\n", secs / 60, secs % 60, frames, players );
	if ( *host )
		printf( "  host %s\n", host );
	printf( "\n" );
	return 0;
}

// --dump : write the full per-frame decode to <demo>.log (debugging aid).
static int Cmd_Dump( const char *path )
{
	int frames = 0, first = -1, last = 0;
	bool seen[ MAX_CLIENTS ];
	memset( seen, 0, sizeof( seen ) );

	if ( DecodeDemo( path, true, &frames, &first, &last, seen ) != 0 )
		return 1;

	printf( "wrote %d frames to %s.log\n", frames, path );
	return 0;
}

// pm_type values (engine: PM_NORMAL=0 .. PM_INTERMISSION=5, PM_DEAD=6, PM_DEAD_LINKED=7).
#define PM_NORMAL          0
#define PM_NORMAL_LINKED   1
#define PM_SPECTATOR       4     // free-float spectate (own slot, not following)
#define PM_INTERMISSION    5
#define PM_DEAD            6
#define PM_DEAD_LINKED     7

// pm_flags bits we care about. CoD2 1.3 (bg_public.h): the engine forces the
// spectating player's playerstate to a COPY of the player being watched. During a
// killcam the player follows their killer, so PMF_FOLLOW is set and ps.clientNum
// becomes the killer's slot. (Verified empirically on a real CTF demo: post-death
// frames carry flags 0x00400142, cnum != owner for the whole killcam.)
#define PMF_FOLLOW         0x400000     // following another player (killcam / follow-spectate)
#define PMF_SPECTATOR_FREE 0x1000000    // free-float spectate (this decoder's old "PMF_SPECTATING")

// How the local (recording) player is faring on a given snapshot. The whole point
// of skip-dead is to keep only ST_ALIVE (the recorder in their own body, alive) and
// cut everything else.
typedef enum
{
	ST_ALIVE,            // own body, pm 0/1, health > 0 — real action
	ST_DEAD_OR_KILLCAM,  // own body dead (pm 6/7) OR following the killer (killcam)
	ST_FREE_SPECTATE     // free-float spectate (pm 4, own slot)
} frameState_t;

static const char *FrameStateName( frameState_t st )
{
	if ( st == ST_ALIVE )           return "(alive)";
	if ( st == ST_FREE_SPECTATE )   return "(spectate)";
	return "(dead/killcam)";
}

// Classify one snapshot's playerstate. ownNum is the recording client's own slot
// (clc.clientNum). Order matters: the follow/foreign-slot test runs FIRST so a
// killcam — which presents as pm 0, health 100 of the *killer* — is never mistaken
// for the recorder being alive.
static frameState_t ClassifyFrame( const playerState_t *ps, int ownNum )
{
	int pm       = ps->pm_type;
	int hp       = ps->stats[ STAT_HEALTH ];
	int following = ( ps->pm_flags & PMF_FOLLOW ) != 0;
	int foreignPS = ( ps->clientNum != ownNum );

	// Killcam / following another player: the camera is on someone else.
	if ( following || foreignPS )
		return ST_DEAD_OR_KILLCAM;

	// Own body, dead and waiting (pm 6/7).
	if ( pm >= PM_DEAD )
		return ST_DEAD_OR_KILLCAM;

	// Free-float spectate (own slot, pm 4).
	if ( pm == PM_SPECTATOR )
		return ST_FREE_SPECTATE;

	// Own body, alive.
	if ( pm <= PM_NORMAL_LINKED && hp > 0 )
		return ST_ALIVE;

	// pm 0/1 but health 0 — a death/respawn transition frame; not real action.
	return ST_DEAD_OR_KILLCAM;
}

// Edit modes for the transcoder's drop/re-time decision.
#define EDIT_SKIPDEAD 0
#define EDIT_CUT      1
#define EDIT_MERGE    2

// Edit running state, threaded through the transcoder (NULL = plain copy).
typedef struct
{
	int mode;                  // EDIT_SKIPDEAD / EDIT_CUT / EDIT_MERGE
	int timeOffset;            // ms removed before the current frame (the re-time amount)
	int dropped;               // frames dropped
	int forceFulls;            // cut/rebase frames emitted (delta base is the previous kept frame)
	// skip-dead
	int inDeadSpan;            // currently inside a death->respawn span
	int deathTime;             // serverTime the current dead span began (start of a dropped run)
	int keepKillcam;           // 1 = keep killcam / follow-another-player frames
	int killcamKept;           // diagnostic: killcam/follow frames kept
	int keepSpectate;          // 1 = keep free-float spectate frames (pm 4)
	int spectateKept;          // diagnostic: free-spectate frames kept
	// cut
	int firstTime;             // serverTime of the first snapshot (captured once)
	int haveFirst;
	int cutStartMs;            // cut: keep [cutStartMs, cutEndMs] relative to firstTime
	int cutEndMs;
	int pastEnd;               // set once we pass cutEndMs so the caller can stop reading
	// merge: fixed re-time so B's snapshots continue A's timeline (timeOffset = mergeShift)
	int mergeShift;            // = firstB_serverTime - (lastA_serverTime + step); shifts B forward
} skipState_t;

// Read the next demo frame's raw bytes ([seq][len][len bytes]); 0 at EOF.
static int Demo_ReadRawFrame( FILE *f, int *outSeq, byte *buf, int *outLen )
{
	int s, len;
	if ( fread( &s, 4, 1, f ) != 1 ) return 0;
	s = LittleLong( s );
	if ( fread( &len, 4, 1, f ) != 1 ) return 0;
	len = LittleLong( len );
	if ( len == -1 || s == -1 ) return 0;            // EOF marker
	if ( len <= 0 || len > MAX_MSGLEN ) return 0;
	if ( (int)fread( buf, 1, len, f ) != len ) return 0;
	*outSeq = s;
	*outLen = len;
	return 1;
}

// Write one re-encoded frame: [seq][len][reliableAck + Huffman(payload)].
static void Demo_WriteFrame( FILE *f, int seq, int reliableAck, const byte *payload, int payloadLen )
{
	static byte compressed[ MAX_MSGLEN ];
	int compSize = MSG_WriteBitsCompress( payload, compressed, payloadLen );
	int len = 4 + compSize;
	int v;
	v = LittleLong( seq );         fwrite( &v, 4, 1, f );
	v = LittleLong( len );         fwrite( &v, 4, 1, f );
	v = LittleLong( reliableAck ); fwrite( &v, 4, 1, f );
	fwrite( compressed, 1, compSize, f );
}

// Skip-dead keeps the last two materialised output frames (ping-pong) so each kept
// frame can delta from the previous KEPT one. Reset by Cmd_SkipDead.
static storedFrame_t g_sf[ 2 ];
static int           g_sfCur   = 0;
static qboolean      g_sfValid = qfalse;

// --verify capture. When g_verifyCap is set, Demo_TranscodeFrame stashes the ORIGINAL
// decompressed payload (dmsg) and the RE-ENCODED payload (omsg) here so the caller can
// byte-compare the two UNCOMPRESSED bitstreams — that's where any divergence originates
// and localizes cleanly (Huffman is a fixed table, so if the uncompressed streams match,
// the on-disk compressed bytes match too). Off by default (NULL).
typedef struct {
	byte orig[ MAX_MSGLEN ];  int origLen;
	byte reenc[ MAX_MSGLEN ]; int reencLen;
} verifyCapture_t;
static verifyCapture_t *g_verifyCap = NULL;

// Transcode one frame: decode each svc command (updating decoder state) and emit
// the equivalent into a fresh uncompressed message, then frame it to `out`.
// `skip` NULL = exact copy; non-NULL = skip-dead (drop dead frames, re-time).
// Returns 1 = written, 0 = dropped (skip-dead), -1 = error.
static int Demo_TranscodeFrame( const byte *frame, int frameLen, FILE *out, int seq, skipState_t *skip )
{
	msg_t in;
	MSG_Init( &in, (byte *)frame, frameLen );
	in.cursize = frameLen;

	int reliableAck = MSG_ReadLong( &in );

	static byte dbuf[ MAX_MSGLEN ];
	msg_t dmsg;
	MSG_Init( &dmsg, dbuf, sizeof( dbuf ) );
	dmsg.cursize = MSG_ReadBitsCompress( in.data + in.readcount, in.cursize - in.readcount, dmsg.data, dmsg.maxsize );

	static byte obuf[ MAX_MSGLEN ];
	msg_t omsg;
	MSG_Init( &omsg, obuf, sizeof( obuf ) );

	// Replay the original playerstate encoding decisions only on the pure re-encode path
	// (--copy/--verify/--convert). Editing ops re-delta against different bases, so they
	// must recompute the change-bits from values.
	g_encReplay = ( skip == NULL );

	int dropFrame = 0;

	while ( 1 )
	{
		if ( dmsg.readcount > dmsg.cursize )
			return -1;
		int cmd = MSG_ReadByte( &dmsg );

		if ( cmd == svc_EOF )
		{
			MSG_WriteByte( &omsg, svc_EOF );
			break;
		}

		switch ( cmd )
		{
		case svc_nop:
			MSG_WriteByte( &omsg, svc_nop );
			break;

		case svc_serverCommand:
		{
			int cseq = MSG_ReadLong( &dmsg );
			char *s = MSG_ReadBigString( &dmsg );
			if ( cseq > clc.serverCommandSequence )      // advance the high-water mark either way
				clc.serverCommandSequence = cseq;
			if ( CmdShouldDrop( s ) )                    // filtered out — leave a harmless seq gap
				break;
			MSG_WriteByte( &omsg, svc_serverCommand );
			MSG_WriteLong( &omsg, cseq );                // keep cseq verbatim — never renumber
			MSG_WriteBigStringRaw( &omsg, s );
			break;
		}

		case svc_gamestate:
			CL_ParseGamestate( &dmsg );
			SV_WriteGameState( &omsg );
			break;

		case svc_snapshot:
		{
			CL_ParseSnapshot( &dmsg );

			if ( !skip )
			{
				SV_WriteSnapshot( &cl.snap, &omsg );   // --copy
				break;
			}

			int t = cl.snap.serverTime;
			qboolean isCut = qfalse;
			if ( !skip->haveFirst ) { skip->firstTime = t; skip->haveFirst = 1; }

			if ( skip->mode == EDIT_CUT )
			{
				int rel = t - skip->firstTime;
				if ( rel < skip->cutStartMs || rel > skip->cutEndMs ) dropFrame = 1;
				if ( rel > skip->cutEndMs ) skip->pastEnd = 1;   // nothing left to keep
				skip->timeOffset = skip->cutStartMs;             // drop the leading slice, re-time by it
			}
			else if ( skip->mode == EDIT_MERGE )
			{
				skip->timeOffset = skip->mergeShift;             // shift B forward onto A's timeline
			}
			else // EDIT_SKIPDEAD
			{
				frameState_t fs = ClassifyFrame( &cl.snap.ps, clc.clientNum );

				// Keep the recorder alive in their own body, plus whatever the keep
				// flags ask for. keep-killcam keeps killcam / following-another-player
				// frames (PMF_FOLLOW); keep-spectate keeps free-float spectate (pm 4).
				// The own-body dead-stare (pm 6/7) is always dropped. With both flags on,
				// everything except the dead-stare is kept.
				int keep = 0;
				if ( fs == ST_ALIVE )
					keep = 1;
				else if ( skip->keepKillcam && fs == ST_DEAD_OR_KILLCAM
				          && ( cl.snap.ps.pm_flags & PMF_FOLLOW ) )
				{
					keep = 1;
					skip->killcamKept++;
				}
				else if ( skip->keepSpectate && fs == ST_FREE_SPECTATE )
				{
					keep = 1;
					skip->spectateKept++;
				}

				// Re-time by the total dropped duration so far. Accumulating the gap
				// since the previous snapshot is correct whether the whole dead span
				// is dropped (default) or only part of it (keep-killcam).
				if ( !keep )
				{
					if ( skip->inDeadSpan ) skip->timeOffset += t - skip->deathTime;
					skip->deathTime = t;       // advance the running drop anchor
					skip->inDeadSpan = 1;
					dropFrame = 1;
				}
				else
				{
					if ( skip->inDeadSpan )    // first kept frame after a dropped run
					{
						skip->timeOffset += t - skip->deathTime;
						skip->inDeadSpan = 0;
						isCut = qtrue;
					}
				}
			}

			if ( !dropFrame )
			{
				storedFrame_t *prev = g_sfValid ? &g_sf[ g_sfCur ^ 1 ] : NULL;
				storedFrame_t *cur  = &g_sf[ g_sfCur ];
				if ( !prev ) isCut = qtrue;
				SkipExtractFrame( cur, skip->timeOffset, isCut, prev );
				SV_WriteSkipSnapshot( prev, cur, &omsg );
				g_sfCur ^= 1;
				g_sfValid = qtrue;
				if ( isCut ) skip->forceFulls++;
			}
			break;
		}

		default:
			return -1;     // svc_download / unknown — not expected in a playable demo
		}
	}

	if ( dropFrame )
	{
		if ( skip ) skip->dropped++;
		return 0;
	}
	if ( omsg.overflowed )
		return -1;

	if ( g_verifyCap )
	{
		g_verifyCap->origLen  = dmsg.cursize;
		g_verifyCap->reencLen = omsg.cursize;
		memcpy( g_verifyCap->orig,  dmsg.data, dmsg.cursize );
		memcpy( g_verifyCap->reenc, omsg.data, omsg.cursize );
	}
	if ( out )                     // --verify passes out=NULL (compare only, no write)
		Demo_WriteFrame( out, seq, reliableAck, omsg.data, omsg.cursize );
	return 1;
}

// --verify : the byte-1:1 gate. For each frame, decode -> re-encode -> RE-COMPRESS and
// compare the result to the ORIGINAL on-disk compressed payload. That on-disk compare is
// the real definition of "1:1" (the output file must equal the input file). Comparing the
// UNCOMPRESSED payloads is NOT valid: the decompressor over-reads the trailing padding bits
// of the byte-aligned stream and emits spurious garbage bytes past the message's svc_EOF,
// which the game ignores but which would false-flag a perfect re-encode. So: on-disk bytes
// gate it, and the uncompressed omsg-vs-dmsg content diff only LOCALIZES where it went wrong.
// Exit: 0 = every frame 1:1, 2 = divergence found, 1 = open error.
static int Cmd_Verify( const char *path )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.verify.log", path );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	static verifyCapture_t cap;
	g_verifyCap = &cap;

	static byte frame[ MAX_MSGLEN ], comp[ MAX_MSGLEN ];
	int seq, len, frames = 0, diverged = 0, errors = 0, gsDiv = 0, snapDiv = 0;
	int firstFrame = -1, firstByte = -1, firstBit = -1, firstSvc = -1;
	int firstOrigComp = 0, firstReComp = 0, firstCompDiffByte = -1;
	int fSecPS = -1, fSecEnt = -1, fSecCli = -1;   // section offsets of the first divergent snapshot
	static byte winOrig[ 48 ], winReenc[ 48 ]; int winStart = 0, winN = 0;
	static byte firstFrameBuf[ MAX_MSGLEN ]; int firstFrameLen = 0, firstFrameSeq = 0;

	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		cap.origLen = cap.reencLen = 0;
		// Mirror the real demo playback: the frame's sequence number drives
		// clc.serverMessageSequence, which snapshot delta references depend on.
		// (Every other command loop sets this; --verify must too, or delta
		// frames decode as non-delta and the re-encode falsely diverges.)
		clc.serverMessageSequence = seq;
		int r = Demo_TranscodeFrame( frame, len, NULL, seq, NULL );
		frames++;
		if ( r != 1 ) { errors++; continue; }         // e.g. svc_download / overflow

		// THE GATE: re-compress the re-encoded message, compare to the original on-disk
		// compressed payload (frame = [reliableAck(4)][compressed]; compare compressed part).
		int compLen  = MSG_WriteBitsCompress( cap.reenc, comp, cap.reencLen );
		int origComp = len - 4;
		int cn = ( compLen < origComp ) ? compLen : origComp;
		int compDiff = -1;
		for ( int i = 0; i < cn; i++ )
			if ( comp[ i ] != frame[ 4 + i ] ) { compDiff = i; break; }
		if ( compDiff < 0 && compLen == origComp )
			continue;                                 // on-disk byte-identical — 1:1 for this frame

		diverged++;
		int svc = cap.reenc[ 0 ];
		if ( svc == svc_gamestate ) gsDiv++; else snapDiv++;

		if ( firstFrame < 0 )
		{
			firstFrame = frames - 1; firstSvc = svc;
			firstOrigComp = origComp; firstReComp = compLen; firstCompDiffByte = compDiff;
			fSecPS = g_secPlayerstate; fSecEnt = g_secEntities; fSecCli = g_secClients;
			memcpy( firstFrameBuf, frame, len ); firstFrameLen = len; firstFrameSeq = seq;
			// Localize in the UNCOMPRESSED domain: first byte where the re-encoded message
			// (omsg = cap.reenc) differs from the original decompressed (dmsg = cap.orig),
			// within the re-encoded length (garbage lives beyond it, so it can't false-hit).
			int un = ( cap.reencLen < cap.origLen ) ? cap.reencLen : cap.origLen;
			for ( int i = 0; i < un; i++ )
				if ( cap.reenc[ i ] != cap.orig[ i ] ) { firstByte = i; break; }
			if ( firstByte >= 0 )
			{
				byte x = cap.orig[ firstByte ] ^ cap.reenc[ firstByte ];
				int b = 0; while ( b < 8 && !( x & ( 1 << b ) ) ) b++;
				firstBit = firstByte * 8 + b;
				winStart = firstByte - 8; if ( winStart < 0 ) winStart = 0;
			}
			winN = 32;
			if ( winStart + winN > cap.origLen )  winN = cap.origLen  - winStart;
			if ( winStart + winN > cap.reencLen ) winN = cap.reencLen - winStart;
			if ( winN < 0 ) winN = 0;
			memcpy( winOrig,  cap.orig  + winStart, winN );
			memcpy( winReenc, cap.reenc + winStart, winN );
		}
	}

	fclose( demo.demofile ); demo.demofile = NULL;
	g_verifyCap = NULL; g_quietLog = 0;

	printf( "\n  verify %s\n", path );
	printf( "  %d frames: %d byte-identical (on disk), %d diverged, %d decode-errors\n",
		frames, frames - diverged - errors, diverged, errors );
	if ( !diverged && !errors )
	{
		printf( "  -> BYTE-IDENTICAL round-trip. 1:1 for this demo.\n" );
		return 0;
	}
	if ( diverged )
		printf( "  diverged: %d gamestate, %d snapshot\n", gsDiv, snapDiv );
	if ( firstFrame >= 0 )
	{
		printf( "  first divergence: frame %d (svc 0x%02x)  compressed %d B -> %d B\n",
			firstFrame, firstSvc, firstOrigComp, firstReComp );
		if ( firstByte >= 0 )
		{
			const char *sec = "header";
			if ( fSecCli >= 0 && firstByte >= fSecCli )       sec = "clients";
			else if ( fSecEnt >= 0 && firstByte >= fSecEnt )  sec = "entities";
			else if ( fSecPS >= 0 && firstByte >= fSecPS )    sec = "playerstate";
			printf( "    re-encode first differs from original at uncompressed byte %d (bit %d) -> %s section\n",
				firstByte, firstBit, sec );
			printf( "    (snapshot sections start at: playerstate=%d entities=%d clients=%d)\n",
				fSecPS, fSecEnt, fSecCli );

			// Second pass: re-transcode just this frame with field attribution on, then map
			// firstByte to the entity + field whose marker precedes it.
			g_attrOn = 1; g_attrN = 0;
			cap.origLen = cap.reencLen = 0;
			Demo_TranscodeFrame( firstFrameBuf, firstFrameLen, NULL, firstFrameSeq, NULL );
			g_attrOn = 0;
			int mi = -1, curEnt = -1; const char *curField = "?";
			// Attribute by BYTE offset (cursize) — reliable, since MSG_WriteByte/Short/Long
			// advance cursize but NOT msg->bit (so bit markers go stale after byte writes).
			for ( int k = 0; k < g_attrN; k++ )
			{
				if ( g_attr[ k ].byteoff > firstByte ) break;
				mi = k;
			}
			if ( mi >= 0 )
			{
				// walk back to the enclosing entity for context
				for ( int k = mi; k >= 0; k-- )
					if ( strstr( g_attr[ k ].label, "entity" ) ) { curEnt = g_attr[ k ].num; break; }
				curField = g_attr[ mi ].label;
				int fStart = g_attr[ mi ].byteoff, fEnd = ( mi + 1 < g_attrN ) ? g_attr[ mi + 1 ].byteoff : -1;
				printf( "    -> at field '%s' (bits=%d)", curField, g_attr[ mi ].num );
				if ( curEnt >= 0 ) printf( "  (entity %d)", curEnt );
				printf( "   [field bytes %d..%d, firstByte %d]\n", fStart, fEnd, firstByte );
				// show a few surrounding markers for context
				for ( int k = ( mi > 2 ? mi - 2 : 0 ); k < g_attrN && k <= mi + 2; k++ )
					printf( "         marker[%d] byte %d bit %d  %s (num=%d)\n",
						k, g_attr[ k ].byteoff, g_attr[ k ].off, g_attr[ k ].label, g_attr[ k ].num );
			}
		}
		else
			printf( "    uncompressed content matches; divergence is length/encoding (comp byte %d)\n",
				firstCompDiffByte );
		printf( "    window from byte %d (original / re-encoded uncompressed):\n    ", winStart );
		for ( int i = 0; i < winN; i++ ) printf( "%02x ", winOrig[ i ] );
		printf( "\n    " );
		for ( int i = 0; i < winN; i++ ) printf( "%02x ", winReenc[ i ] );
		printf( "\n" );
	}
	printf( "  -> NOT 1:1 yet (expected in development; chase divergences to 0).\n" );
	return 2;
}

// --copy : round-trip a demo through decode->encode. Proves the writer.
static int Cmd_Copy( const char *inPath, const char *outPath )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.copy.log", inPath );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;   // silence the decoder's per-command logging

	if ( !FS_FOpenFileRead( inPath, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", inPath );
		return 1;
	}
	FILE *out = fopen( outPath, "wb" );
	if ( !out )
	{
		printf( "error: cannot write '%s'\n", outPath );
		fclose( demo.demofile );
		return 1;
	}

	static byte frame[ MAX_MSGLEN ];
	int seq, len, frames = 0, bad = 0;

	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;
		int r = Demo_TranscodeFrame( frame, len, out, seq, NULL );
		if ( r != 1 ) { bad++; if ( bad <= 3 ) { g_quietLog = 0; printf( "  frame %d (seq %d) failed to transcode\n", frames, seq ); g_quietLog = 1; } }
		frames++;
	}

	// EOF marker
	int eof = -1;
	fwrite( &eof, 4, 1, out );
	fwrite( &eof, 4, 1, out );

	fclose( out );
	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	printf( "copied %d frames", frames );
	if ( bad ) printf( "  (%d failed)", bad );
	printf( "  ->  %s\n", outPath );
	return bad ? 1 : 0;
}

// --skip-dead : drop every death->respawn span and re-time, into a new playable demo.
// keepKillcam:  keep killcam / following-another-player frames (cut only the dead-stare).
// keepSpectate: keep free-float spectate frames too. Both on => keep everything but the
// own-body dead-stare.
static int Cmd_SkipDead( const char *inPath, const char *outPath, int keepKillcam, int keepSpectate )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.skip.log", inPath );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( inPath, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", inPath );
		return 1;
	}
	FILE *out = fopen( outPath, "wb" );
	if ( !out )
	{
		printf( "error: cannot write '%s'\n", outPath );
		fclose( demo.demofile );
		return 1;
	}

	skipState_t skip;
	memset( &skip, 0, sizeof( skip ) );
	skip.keepKillcam  = keepKillcam;
	skip.keepSpectate = keepSpectate;
	g_sfValid = qfalse;
	g_sfCur   = 0;

	static byte frame[ MAX_MSGLEN ];
	int seq, len, kept = 0, err = 0, outSeq = 0;

	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;
		int r = Demo_TranscodeFrame( frame, len, out, outSeq, &skip );
		if ( r == 1 ) { kept++; outSeq++; }
		else if ( r < 0 ) err++;
	}

	int eof = -1;
	fwrite( &eof, 4, 1, out );
	fwrite( &eof, 4, 1, out );

	fclose( out );
	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	int sec = skip.timeOffset / 1000;
	printf( "skip-dead: kept %d frames, dropped %d (%d.%03ds removed), %d full/cut frames",
		kept, skip.dropped, sec, skip.timeOffset % 1000, skip.forceFulls );
	if ( err ) printf( "  [%d errors]", err );
	printf( "  ->  %s\n", outPath );
	if ( keepKillcam || keepSpectate )
	{
		if ( keepKillcam )  printf( "  kept %d killcam/follow frames (keep-killcam)\n", skip.killcamKept );
		if ( keepSpectate ) printf( "  kept %d free-spectate frames (keep-spectate)\n", skip.spectateKept );
		printf( "  only the own-body dead-stare was removed\n" );
	}
	else
		printf( "  killcam and spectating were cut too (default). Add keep-killcam and/or\n"
		        "  keep-spectate to keep those.\n" );
	// If the demo ended while the player was still dead (never respawned), every
	// remaining frame was dropped — warn so a truncated tail isn't a surprise.
	if ( skip.inDeadSpan )
		printf( "  note: demo ended during a dead span — the tail (no respawn) was removed\n" );
	return 0;
}

// Decode a raw frame just far enough to classify it: is it a gamestate? a snapshot?
// what is the snapshot's snapFlags? Updates the decoder state (cl) as a side effect,
// exactly like a normal decode, so cl.gameState is current for the splitter.
// Returns: 1=gamestate, 2=snapshot, 0=other/empty, -1=error. *outSnapFlags valid for 2.
static int Demo_ClassifyFrame( const byte *frame, int frameLen, int *outSnapFlags )
{
	msg_t in;
	MSG_Init( &in, (byte *)frame, frameLen );
	in.cursize = frameLen;
	MSG_ReadLong( &in );   // reliableAck

	static byte dbuf[ MAX_MSGLEN ];
	msg_t dmsg;
	MSG_Init( &dmsg, dbuf, sizeof( dbuf ) );
	dmsg.cursize = MSG_ReadBitsCompress( in.data + in.readcount, in.cursize - in.readcount, dmsg.data, dmsg.maxsize );

	int kind = 0;
	while ( 1 )
	{
		if ( dmsg.readcount > dmsg.cursize ) return -1;
		int cmd = MSG_ReadByte( &dmsg );
		if ( cmd == svc_EOF ) break;
		switch ( cmd )
		{
		case svc_nop: break;
		case svc_serverCommand: { MSG_ReadLong( &dmsg ); MSG_ReadBigString( &dmsg ); break; }
		case svc_gamestate: CL_ParseGamestate( &dmsg ); kind = 1; break;
		case svc_snapshot:  CL_ParseSnapshot( &dmsg );  if ( outSnapFlags ) *outSnapFlags = cl.snap.snapFlags; kind = 2; break;
		default: return -1;
		}
	}
	return kind;
}

// --split-map / --split-match : break one demo into several. Each output segment is a
// self-contained demo: a fresh gamestate (from the live cl.gameState) followed by a
// --copy of that segment's frames.
//   split-map   : new segment at each mid-stream svc_gamestate (the server changed map).
//   split-match : new segment at each fast_restart (SNAPFLAG_SERVERCOUNT flips in snapFlags).
static int Cmd_Split( const char *inPath, int byMatch )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.split.log", inPath );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( inPath, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", inPath );
		return 1;
	}

	// derive an output base: strip a trailing .dm_1
	char base[ 1024 ];
	Q_strncpyz( base, inPath, sizeof( base ) );
	char *dot = strrchr( base, '.' );
	char *sl  = strrchr( base, '/' ); char *bs = strrchr( base, '\\' ); if ( bs > sl ) sl = bs;
	if ( dot && dot > sl ) *dot = 0;

	static byte frame[ MAX_MSGLEN ];
	int seq, len, segment = 0, outSeq = 0, segFrames = 0, prevServerCount = -1, frameNo = 0;
	FILE *out = NULL;
	char outName[ 1100 ];

	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;
		// peek (decodes into cl); keep a copy because transcode re-reads the same bytes
		static byte peekCopy[ MAX_MSGLEN ];
		memcpy( peekCopy, frame, len );
		int snapFlags = 0;
		int kind = Demo_ClassifyFrame( peekCopy, len, &snapFlags );

		int boundary = 0;
		if ( frameNo > 0 )   // never split before the very first gamestate
		{
			if ( !byMatch && kind == 1 )                    // split-map: a 2nd+ gamestate
				boundary = 1;
			else if ( byMatch && kind == 2 )                // split-match: snapFlags server-count flip
			{
				int sc = snapFlags & SNAPFLAG_SERVERCOUNT;
				if ( prevServerCount >= 0 && sc != prevServerCount )
					boundary = 1;
				prevServerCount = sc;
			}
		}
		else if ( byMatch && kind == 2 )
			prevServerCount = snapFlags & SNAPFLAG_SERVERCOUNT;

		// open a new segment file at the very start or at a boundary
		if ( !out || boundary )
		{
			if ( out )
			{
				int eof = -1; fwrite( &eof, 4, 1, out ); fwrite( &eof, 4, 1, out );
				fclose( out );
				if ( !g_quietLog ) ; // (segment summary printed below)
				g_quietLog = 0; printf( "  segment %d: %d frames -> %s\n", segment, segFrames, outName ); g_quietLog = 1;
			}
			segment++;
			snprintf( outName, sizeof( outName ), "%s_%s%d.dm_1", base, byMatch ? "match" : "map", segment );
			out = fopen( outName, "wb" );
			if ( !out ) { printf( "error: cannot write '%s'\n", outName ); fclose( demo.demofile ); return 1; }
			outSeq = 0; segFrames = 0;

			// segments after the first need their own leading gamestate (the original
			// gamestate only loads at the very start of the file). For split-map the
			// boundary frame IS a gamestate, so it self-serves; for split-match we must
			// synthesize one from the current cl.gameState.
			if ( boundary && byMatch )
			{
				static byte gbuf[ MAX_MSGLEN ];
				msg_t gmsg; MSG_Init( &gmsg, gbuf, sizeof( gbuf ) );
				SV_WriteGameState( &gmsg );                 // full gamestate from the live cl.gameState
				Demo_WriteFrame( out, outSeq++, 0, gmsg.data, gmsg.cursize );
				segFrames++;
			}
		}

		// write this frame to the current segment as an exact copy
		if ( Demo_TranscodeFrame( frame, len, out, outSeq, NULL ) == 1 ) { outSeq++; segFrames++; }
		frameNo++;
	}

	if ( out )
	{
		int eof = -1; fwrite( &eof, 4, 1, out ); fwrite( &eof, 4, 1, out );
		fclose( out );
		g_quietLog = 0; printf( "  segment %d: %d frames -> %s\n", segment, segFrames, outName ); g_quietLog = 1;
	}

	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;
	printf( "split into %d %s segment(s)\n", segment, byMatch ? "match" : "map" );
	return 0;
}

// Parse a demo timestamp: "M:SS", plain seconds, or the keywords "start" (=0) /
// "end" (=demo end). Returns milliseconds from the demo start.
static int parseTimeMs( const char *s )
{
	if ( !strcmp( s, "start" ) ) return 0;
	if ( !strcmp( s, "end" ) )   return 0x7FFFFFFF;
	const char *colon = strchr( s, ':' );
	if ( colon )
		return ( atoi( s ) * 60 + atoi( colon + 1 ) ) * 1000;
	return atoi( s ) * 1000;
}

// --cut : keep only [start, end] (mm:ss from the demo start) and re-time so the
// clip plays from the beginning, into a new playable demo. Reuses the skip-dead
// delta-from-previous-kept-frame path; the only difference is the drop decision.
static int Cmd_Cut( const char *inPath, const char *outPath, const char *startArg, const char *endArg )
{
	int startMs = parseTimeMs( startArg );
	int endMs   = parseTimeMs( endArg );
	if ( endMs <= startMs )
	{
		printf( "error: end (%s) must be after start (%s)\n", endArg, startArg );
		return 1;
	}

	snprintf( logFileName, sizeof( logFileName ), "%s.cut.log", inPath );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( inPath, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", inPath );
		return 1;
	}
	FILE *out = fopen( outPath, "wb" );
	if ( !out )
	{
		printf( "error: cannot write '%s'\n", outPath );
		fclose( demo.demofile );
		return 1;
	}

	skipState_t skip;
	memset( &skip, 0, sizeof( skip ) );
	skip.mode       = EDIT_CUT;
	skip.cutStartMs = startMs;
	skip.cutEndMs   = endMs;
	g_sfValid = qfalse;
	g_sfCur   = 0;

	static byte frame[ MAX_MSGLEN ];
	int seq, len, kept = 0, err = 0, outSeq = 0;

	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;
		int r = Demo_TranscodeFrame( frame, len, out, outSeq, &skip );
		if ( r == 1 ) { kept++; outSeq++; }
		else if ( r < 0 ) err++;
		if ( skip.pastEnd ) break;          // everything past the cut end is dropped — stop reading
	}

	int eof = -1;
	fwrite( &eof, 4, 1, out );
	fwrite( &eof, 4, 1, out );

	fclose( out );
	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	printf( "cut: kept %d frames", kept );
	if ( skip.cutEndMs != 0x7FFFFFFF )
	{
		int dur = skip.cutEndMs - skip.cutStartMs;
		printf( " (%d:%02d)", dur / 60000, ( dur / 1000 ) % 60 );
	}
	if ( err ) printf( "  [%d errors]", err );
	printf( "  ->  %s\n", outPath );
	return 0;
}

// --merge inA inB out : append demo B after demo A as one continuous demo (no map
// reload). EXPERIMENTAL (same as Caball's): works for same-map/mod demos; entities
// whose baseline differs in B can ghost, and the POV stays A's local player.
//
// Strategy: keep A's gamestate; copy A verbatim (recording its last serverTime + the
// running output sequence). Then decode B and re-emit its snapshots shifted forward
// onto A's timeline (EDIT_MERGE), first one non-delta, injecting B's differing
// configstrings as mid-stream svc_configstring updates so scores/start-time follow.
static int Cmd_Merge( const char *inA, const char *inB, const char *outPath )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.merge.log", inA );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	FILE *out = fopen( outPath, "wb" );
	if ( !out ) { printf( "error: cannot write '%s'\n", outPath ); return 1; }

	static byte frame[ MAX_MSGLEN ];
	int seq, len, outSeq = 0, framestep = 50;

	// --- pass 1: copy A verbatim, capture its tail timeline + configstrings ---
	if ( !FS_FOpenFileRead( inA, &demo.demofile, qtrue ) || !demo.demofile )
	{ printf( "error: cannot open '%s'\n", inA ); fclose( out ); return 1; }

	int lastA = 0, aFrames = 0, prevA = -1, stepSeen = 0;
	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;
		if ( Demo_TranscodeFrame( frame, len, out, outSeq, NULL ) == 1 ) outSeq++;
		if ( cl.snap.serverTime > lastA ) lastA = cl.snap.serverTime;
		if ( prevA >= 0 && cl.snap.serverTime > prevA && !stepSeen )
		{ framestep = cl.snap.serverTime - prevA; if ( framestep > 0 && framestep <= 200 ) stepSeen = 1; }
		prevA = cl.snap.serverTime;
		aFrames++;
	}
	int maxCmdSeqA = clc.serverCommandSequence;
	// snapshot A's serverinfo configstrings so we can diff B against them
	static char csA[ MAX_CONFIGSTRINGS ][ MAX_STRING_CHARS ];
	for ( int i = 0; i < MAX_CONFIGSTRINGS; i++ )
		Q_strncpyz( csA[ i ], CL_ConfigString( i ), sizeof( csA[ 0 ] ) );
	fclose( demo.demofile ); demo.demofile = NULL;

	// --- read B's first snapshot time (need its gamestate first) ---
	if ( !FS_FOpenFileRead( inB, &demo.demofile, qtrue ) || !demo.demofile )
	{ printf( "error: cannot open '%s'\n", inB ); fclose( out ); return 1; }

	skipState_t skip;
	memset( &skip, 0, sizeof( skip ) );
	skip.mode = EDIT_MERGE;
	g_sfValid = qfalse; g_sfCur = 0;        // first B snapshot will be non-delta (prev=NULL)

	int firstBTime = -1, bKept = 0, injected = 0, gotGamestate = 0;
	while ( Demo_ReadRawFrame( demo.demofile, &seq, frame, &len ) )
	{
		clc.serverMessageSequence = seq;

		// classify without emitting (decodes into cl) — B's gamestate is dropped, not re-sent
		static byte peek[ MAX_MSGLEN ];
		memcpy( peek, frame, len );
		int sf = 0, kind = Demo_ClassifyFrame( peek, len, &sf );

		if ( kind == 1 )            // B's gamestate: parsed into cl above; do NOT emit (would reload map)
		{
			gotGamestate = 1;
			// inject configstrings that differ from A (scores, start-time, motd, winner...)
			for ( int i = 0; i < MAX_CONFIGSTRINGS; i++ )
			{
				const char *bcs = CL_ConfigString( i );
				if ( i == 0 ) continue;                       // serverinfo: keep A's (same map/version)
				if ( !bcs[ 0 ] || !strcmp( bcs, csA[ i ] ) ) continue;
				// A mid-stream configstring change rides the reliable-command channel as
				// `cs <index> "<value>"` (a svc_serverCommand) — svc_configstring is only
				// valid inside a gamestate block, so we must NOT emit it standalone.
				static byte cbuf[ MAX_MSGLEN ]; msg_t cmsg; MSG_Init( &cmsg, cbuf, sizeof( cbuf ) );
				char cscmd[ MAX_STRING_CHARS + 16 ];
				snprintf( cscmd, sizeof( cscmd ), "cs %d \"%s\"", i, bcs );
				MSG_WriteByte( &cmsg, svc_serverCommand );
				MSG_WriteLong( &cmsg, ++maxCmdSeqA );        // continue A's command sequence
				MSG_WriteBigStringRaw( &cmsg, cscmd );
				MSG_WriteByte( &cmsg, svc_EOF );
				Demo_WriteFrame( out, outSeq++, 0, cmsg.data, cmsg.cursize );
				injected++;
			}
			continue;
		}

		if ( kind == 2 && firstBTime < 0 )
		{
			firstBTime = cl.snap.serverTime;
			skip.mergeShift = firstBTime - ( lastA + framestep );   // shift B forward onto A's tail
			// keep B's command sequence strictly above A's so its chat/score isn't deduped away
			if ( clc.serverCommandSequence <= maxCmdSeqA )
				clc.serverCommandSequence = maxCmdSeqA;
		}

		// re-emit B's frame (snapshots get EDIT_MERGE re-timing; first is non-delta)
		if ( Demo_TranscodeFrame( frame, len, out, outSeq, &skip ) == 1 ) { outSeq++; bKept++; }
	}

	int eof = -1; fwrite( &eof, 4, 1, out ); fwrite( &eof, 4, 1, out );
	fclose( out ); fclose( demo.demofile ); demo.demofile = NULL;
	g_quietLog = 0;

	printf( "merge: A=%d frames + B=%d frames (%d configstrings injected) -> %s\n",
		aFrames, bKept, injected, outPath );
	if ( !gotGamestate )
		printf( "  warning: demo B had no gamestate frame — merge may be incomplete\n" );
	printf( "  note: experimental — same-map/mod only; new B entities may ghost; POV stays demo A's player\n" );
	return 0;
}

// --deadscan : diagnostic — show where the local player is dead (pm_type >= PM_DEAD)
// so we can see the dead/respawn spans skip-dead will remove.
// Friendly name for a hudelem type (he_type_t).
static const char *HudTypeName( int t )
{
	switch ( t )
	{
	case HE_TYPE_TEXT:       return "text";
	case HE_TYPE_VALUE:      return "value";
	case HE_TYPE_PLAYERNAME: return "playername";
	case HE_TYPE_MAPNAME:    return "mapname";
	case HE_TYPE_GAMETYPE:   return "gametype";
	case HE_TYPE_MATERIAL:   return "material/icon";
	case HE_TYPE_TIMER_DOWN: return "timer";
	default:                 return "other";
	}
}

// --hudscan : list the distinct scripted HUD elements a demo carries (the things
// --remove-hud strips). Each line shows the element type, its on-screen position, and
// — for icons — the shader name, which is exactly what `--remove-hud keep <name>` matches.
// Built-in readouts (ammo, weapon name, grenade count, compass) are NOT here: the client
// draws those from the player's own state, they're not stored as HUD elements.
static int Cmd_HudScan( const char *path )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.hudscan.log", path );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	// Collect distinct elements by a simple key (type | materialIndex | text | label).
	// HUD elements live in the playerstate every frame; we only want each unique one once.
	struct { int type, mat, text, label; int seen; char shader[ 64 ]; } seen[ 256 ];
	int nSeen = 0;

	while ( CL_ReadDemoMessage() )
	{
		snapshot_t snapshot;
		if ( !CL_GetSnapshot( cl.snap.messageNum, &snapshot ) )
			continue;

		for ( int pass = 0; pass < 2; pass++ )
		{
			hudelem_t *arr = ( pass == 0 ) ? cl.snap.ps.hud.current : cl.snap.ps.hud.archival;
			int count = ( pass == 0 ) ? MAX_HUDELEMS_CURRENT : MAX_HUDELEMS_ARCHIVAL;
			for ( int i = 0; i < count; i++ )
			{
				hudelem_t *h = &arr[ i ];
				if ( h->type == HE_TYPE_FREE )
					break;                            // packed array — tail is empty
				int dup = 0;
				for ( int k = 0; k < nSeen; k++ )
					if ( seen[ k ].type == h->type && seen[ k ].mat == h->materialIndex &&
					     seen[ k ].text == h->text && seen[ k ].label == h->label )
					{ seen[ k ].seen++; dup = 1; break; }
				if ( dup || nSeen >= 256 ) continue;

				seen[ nSeen ].type  = h->type;
				seen[ nSeen ].mat   = h->materialIndex;
				seen[ nSeen ].text  = h->text;
				seen[ nSeen ].label = h->label;
				seen[ nSeen ].seen  = 1;
				seen[ nSeen ].shader[ 0 ] = 0;
				if ( h->materialIndex )
				{
					const char *s = CL_ConfigString( CS_SHADERS + h->materialIndex );
					Q_strncpyz( seen[ nSeen ].shader, s, sizeof( seen[ 0 ].shader ) );
				}
				nSeen++;
			}
		}
	}

	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	printf( "\n  %d distinct scripted HUD element(s) in this demo:\n\n", nSeen );
	printf( "  %-14s %-32s %s\n", "type", "shader (for keep <name>)", "frames" );
	printf( "  %-14s %-32s %s\n", "----", "------------------------", "------" );
	for ( int k = 0; k < nSeen; k++ )
		printf( "  %-14s %-32s %d\n",
			HudTypeName( seen[ k ].type ),
			seen[ k ].shader[ 0 ] ? seen[ k ].shader : "-",
			seen[ k ].seen );
	printf( "\n  --remove-hud strips all of these. Keep one with:  --remove-hud in out keep <shader>\n" );
	printf( "  The ammo / weapon name / grenade count / compass are NOT here — the game draws\n" );
	printf( "  those from the player's own state, so they aren't stored as removable elements.\n" );
	return 0;
}

static int Cmd_DeadScan( const char *path )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.scan.log", path );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	int firstTime = -1, prevTime = 0x7fffffff;
	frameState_t prevState = (frameState_t)-1;
	int deadStartMs = -1, spans = 0, deadMs = 0, total = 0;
	int ownAlive = 0, killcamMs = 0;   // diagnostics: killcam time + own-alive frame count

	while ( CL_ReadDemoMessage() )
	{
		snapshot_t snapshot;
		if ( !CL_GetSnapshot( cl.snap.messageNum, &snapshot ) )
			continue;
		total++;

		int t  = cl.snap.serverTime;
		if ( firstTime < 0 ) firstTime = t;
		int rel = t - firstTime;

		frameState_t st = ClassifyFrame( &cl.snap.ps, clc.clientNum );
		if ( st == ST_ALIVE ) ownAlive++;
		if ( st != prevState )
		{
			g_quietLog = 0;
			printf( "  t=%5d.%03ds  %-14s  pm_type=%d health=%d cnum=%d\n",
				rel / 1000, rel % 1000, FrameStateName( st ),
				cl.snap.ps.pm_type, cl.snap.ps.stats[ STAT_HEALTH ], cl.snap.ps.clientNum );
			g_quietLog = 1;
			prevState = st;
		}

		// A dead span runs from the moment the player stops being ALIVE (own body,
		// alive) until they are ALIVE again — so it spans the whole arc:
		// own-body dead (pm 6/7) -> killcam (following the killer) -> free-spectate
		// (pm 4) -> respawn. ST_ALIVE is the only state that closes a span.
		if ( deadStartMs < 0 && st != ST_ALIVE ) deadStartMs = t;
		if ( deadStartMs >= 0 && st == ST_ALIVE ) { deadMs += t - deadStartMs; spans++; deadStartMs = -1; }
		// Killcam portion = time spent following the killer; measured by the real gap
		// to the previous frame, not a fixed step (frame cadence varies by sv_fps).
		if ( prevTime != 0x7fffffff && ( cl.snap.ps.pm_flags & PMF_FOLLOW ) )
			killcamMs += t - prevTime;
		prevTime = t;
	}
	if ( deadStartMs >= 0 ) { deadMs += cl.snap.serverTime - deadStartMs; spans++; }

	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	int totalMs = ( firstTime < 0 ) ? 0 : ( cl.snap.serverTime - firstTime );
	printf( "\n  %d frames, %d:%02d total\n", total, totalMs / 60000, ( totalMs / 1000 ) % 60 );
	printf( "  dead spans: %d   dead time: %d.%03ds  (%d%% of demo)\n",
		spans, deadMs / 1000, deadMs % 1000, totalMs ? ( deadMs * 100 / totalMs ) : 0 );
	printf( "  of which killcam (following the killer): ~%d.%03ds\n",
		killcamMs / 1000, killcamMs % 1000 );

	// Caster/spectator-demo guard: if the recorder is almost never alive in their
	// own body, this is a spectator demo, not a player demo — skip-dead would gut it.
	if ( total > 0 && ownAlive * 100 / total < 10 )
		printf( "  WARNING: own-body-alive only %d%% of frames — looks like a spectator/caster\n"
		        "           demo; --skip-dead would remove almost everything. Use --cut instead.\n",
		        ownAlive * 100 / total );
	return 0;
}

// --commands : dump every server command in the demo with its serverTime. This is
// the gameplay/event channel (chat, killfeed, configstring updates) — the raw
// material for a killfeed/chat overview and the last un-decoded layer of the format.
static int Cmd_Commands( const char *path )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.cmd.log", path );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	printf( "[serverTime]  #seq  command\n" );
	g_dumpCommands = 1;
	while ( CL_ReadDemoMessage() )
		;                                  // decoding prints each new server command
	g_dumpCommands = 0;

	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;
	return 0;
}

// Strip CoD colour codes from a name. Standard codes are ^<digit>; some servers
// store them doubled (^^NN). Rule: after a run of N carets, drop up to N colour
// digits — handles both ^3 and ^^33 without eating real digits in a name.
static void StripColors( char *dst, const char *src, int dstsize )
{
	int j = 0;
	while ( *src && j < dstsize - 1 )
	{
		if ( *src == '^' )
		{
			int carets = 0;
			while ( *src == '^' ) { src++; carets++; }
			while ( carets-- > 0 && *src >= '0' && *src <= '9' ) src++;
			continue;
		}
		dst[ j++ ] = *src++;
	}
	dst[ j ] = 0;
}

// Special means-of-death (eventParm & 0x80) — CoD2rev MOD_ enum.
static const char *MeansOfDeathName( int mod )
{
	switch ( mod )
	{
	case 7:  return "melee";
	case 8:  return "headshot";
	case 9:  return "crush";
	case 11: return "falling";
	case 12: return "suicide";
	default: return "world";
	}
}

// Render bare localization keys readably: "MP_SCORE_LIMIT_REACHED" ->
// "score limit reached". Only rewrites ALL_CAPS_UNDERSCORE tokens; normal text
// passes through. Output never grows, so in-place via a scratch copy is safe.
static void PrettifyLocKeys( char *s, int size )
{
	char out[ 512 ]; int oj = 0;
	for ( const char *p = s; *p && oj < (int)sizeof( out ) - 1; )
	{
		if ( *p == ' ' ) { out[ oj++ ] = *p++; continue; }
		const char *start = p;
		while ( *p && *p != ' ' ) p++;
		int len = (int)( p - start );
		int caps = 1, under = 0;
		for ( int i = 0; i < len; i++ )
		{
			char c = start[ i ];
			if ( c == '_' ) { under = 1; continue; }
			if ( ( c < 'A' || c > 'Z' ) && ( c < '0' || c > '9' ) ) { caps = 0; break; }
		}
		if ( caps && under && len >= 5 )
		{
			int i = 0;
			if      ( !strncmp( start, "GAME_", 5 ) )     i = 5;
			else if ( !strncmp( start, "MP_", 3 ) )       i = 3;
			else if ( !strncmp( start, "PLATFORM_", 9 ) ) i = 9;
			for ( ; i < len && oj < (int)sizeof( out ) - 1; i++ )
			{
				char c = start[ i ];
				if ( c == '_' )                 out[ oj++ ] = ' ';
				else if ( c >= 'A' && c <= 'Z' ) out[ oj++ ] = c - 'A' + 'a';
				else                             out[ oj++ ] = c;
			}
		}
		else
		{
			for ( int i = 0; i < len && oj < (int)sizeof( out ) - 1; i++ )
				out[ oj++ ] = start[ i ];
		}
	}
	out[ oj ] = 0;
	Q_strncpyz( s, out, size );
}

// ---- HTML export (--overview <demo> <out.html>) -------------------------------
// Timeline rows are accumulated in memory so the page can lead with the summary.

static char *g_htmlRows = NULL;
static int   g_htmlLen  = 0, g_htmlCap = 0;

// Which embedded icons the page actually uses — collected during the timeline pass so
// the <head> only emits the base64 for icons that appear (keeps the file small). One
// flag per g_embeddedIcons entry.
static char g_iconUsed[ 64 ];

// Map a CoD2 weapon name (as it appears in CS_WEAPONS / the killfeed, e.g.
// "mosin_nagant_mp", "TT30_mp") to an embedded icon name, or NULL for text fallback.
// Strips the trailing _mp, lowercases, and collapses stance/bipod variants so all
// mg42_bipod_* etc. resolve to one icon.
static const char *IconForWeapon( const char *weap )
{
	char key[ 64 ];
	int j = 0;
	for ( int i = 0; weap[ i ] && j < (int)sizeof( key ) - 1; i++ )
		key[ j++ ] = tolower( (unsigned char)weap[ i ] );
	key[ j ] = 0;

	// drop a trailing _mp
	int n = (int)strlen( key );
	if ( n > 3 && !strcmp( key + n - 3, "_mp" ) ) key[ n - 3 ] = 0;

	// collapse stance/bipod variants (mg42_bipod_stand, 30cal_prone, ...)
	char *p;
	if ( ( p = strstr( key, "_bipod" ) ) ) *p = 0;
	if ( ( p = strstr( key, "_prone" ) ) ) *p = 0;
	if ( ( p = strstr( key, "_stand" ) ) ) *p = 0;
	if ( ( p = strstr( key, "_duck"  ) ) ) *p = 0;

	for ( int i = 0; i < g_numWeaponIcons; i++ )
		if ( !strcmp( key, g_weaponIcons[ i ].key ) )
			return g_weaponIcons[ i ].icon;
	return NULL;
}

// Map a means-of-death name ("headshot", "melee", "falling", ...) to a death icon, or
// NULL. Headshots keep the weapon icon but get the hs colour, so they are handled at
// the call site; this covers melee/falling/crush/suicide.
static const char *IconForDeath( const char *mod )
{
	for ( int i = 0; i < g_numDeathIcons; i++ )
		if ( !strcmp( mod, g_deathIcons[ i ].key ) )
			return g_deathIcons[ i ].icon;
	return NULL;
}

// Mark an icon name as used (so its base64 is emitted) and return it unchanged, or
// NULL if it isn't an embedded icon.
static const char *MarkIcon( const char *name )
{
	if ( !name ) return NULL;
	for ( int i = 0; i < g_numEmbeddedIcons; i++ )
		if ( !strcmp( name, g_embeddedIcons[ i ].name ) )
		{
			g_iconUsed[ i ] = 1;
			return name;
		}
	return NULL;
}

static void HtmlAppend( const char *s )
{
	int len = (int)strlen( s );
	if ( g_htmlLen + len + 1 > g_htmlCap )
	{
		g_htmlCap = ( g_htmlCap ? g_htmlCap * 2 : 0x10000 );
		if ( g_htmlCap < g_htmlLen + len + 1 ) g_htmlCap = g_htmlLen + len + 1;
		g_htmlRows = (char *)realloc( g_htmlRows, g_htmlCap );
	}
	memcpy( g_htmlRows + g_htmlLen, s, len + 1 );
	g_htmlLen += len;
}

static void HtmlEscape( char *dst, const char *src, int dstsize )
{
	int j = 0;
	for ( ; *src && j < dstsize - 8; src++ )
	{
		unsigned char b = (unsigned char)*src;
		if      ( b == '&' ) { memcpy( dst + j, "&amp;", 5 ); j += 5; }
		else if ( b == '<' ) { memcpy( dst + j, "&lt;",  4 ); j += 4; }
		else if ( b == '>' ) { memcpy( dst + j, "&gt;",  4 ); j += 4; }
		else if ( b >= 0x80 )   // CoD2 names are Latin-1-ish; re-encode as valid UTF-8
		{
			dst[ j++ ] = (char)( 0xC0 | ( b >> 6 ) );
			dst[ j++ ] = (char)( 0x80 | ( b & 0x3F ) );
		}
		else dst[ j++ ] = b;
	}
	dst[ j ] = 0;
}

// CoD colour-code palette (^0-^9), tuned for a dark page.
static const char *g_codColors[ 10 ] =
{
	"#7a7a7a", "#ff4d4d", "#4dff4d", "#ffe14d", "#5c8aff",
	"#4de1ff", "#ff5cf0", "#ececec", "#c8a064", "#9d9d9d"
};

// Render a raw (colour-coded) name as HTML spans. Handles doubled ^^NN codes.
static void HtmlColorName( char *dst, const char *src, int dstsize )
{
	int j = 0, open = 0;
	char esc[ 16 ];   // must exceed HtmlEscape's dstsize-8 guard for a 1-char input
	while ( *src && j < dstsize - 64 )
	{
		if ( *src == '^' )
		{
			int carets = 0;
			while ( *src == '^' ) { src++; carets++; }
			int color = -1;
			while ( carets-- > 0 && *src >= '0' && *src <= '9' )
			{
				if ( color < 0 ) color = *src - '0';
				src++;
			}
			if ( color >= 0 )
			{
				if ( open ) { memcpy( dst + j, "</span>", 7 ); j += 7; }
				j += snprintf( dst + j, dstsize - j, "<span style=\"color:%s\">", g_codColors[ color ] );
				open = 1;
			}
			continue;
		}
		{
			char one[ 2 ] = { *src++, 0 };
			HtmlEscape( esc, one, sizeof( esc ) );   // escapes &<> and re-encodes high bytes
			int el = (int)strlen( esc );
			memcpy( dst + j, esc, el ); j += el;
		}
	}
	if ( open ) { memcpy( dst + j, "</span>", 7 ); j += 7; }
	dst[ j ] = 0;
}

// --overview : the demo's killfeed. Kills are EV_OBITUARY temp entities
// (eType = ET_EVENTS+EV_OBITUARY = 208) carrying victim (otherEntityNum),
// attacker (attackerEntityNum) and eventParm (weapon index, or MOD|0x80).
// Player names come from the client states, weapon names from CS_WEAPONS.
static int Cmd_Overview( const char *path, const char *htmlPath )
{
	snprintf( logFileName, sizeof( logFileName ), "%s.ov.log", path );
	FILE *lf = fopen( logFileName, "w" ); if ( lf ) fclose( lf );
	g_quietLog = 1;

	if ( !FS_FOpenFileRead( path, &demo.demofile, qtrue ) || !demo.demofile )
	{
		printf( "error: cannot open '%s'\n", path );
		return 1;
	}

	static char names[ MAX_CLIENTS ][ 32 ];
	static char rawnames[ MAX_CLIENTS ][ 32 ];         // colour codes kept, for the HTML page
	static char weapons[ 256 ][ 40 ];
	static int  stKills[ MAX_CLIENTS ], stDeaths[ MAX_CLIENTS ], stHS[ MAX_CLIENTS ];
	char mapname[ 64 ] = "", gametype[ 24 ] = "";
	int  nWeapons = 0, haveWeapons = 0;
	int  prevObit[ 64 ], nPrev = 0;
	int  firstTime = -1, lastTime = 0, kills = 0, chats = 0, notes = 0;
	int  scoreAllies = -999999, scoreAxis = -999999;   // sentinel = not yet seen
	int  objective = 1;                                // score lines only when score = objectives
	int  html = ( htmlPath != NULL );
	char row[ 1400 ], h1[ 320 ], h2[ 320 ];
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		names[ i ][ 0 ] = 0; rawnames[ i ][ 0 ] = 0;
		stKills[ i ] = 0; stDeaths[ i ] = 0; stHS[ i ] = 0;
	}
	g_htmlLen = 0;
	g_collectEvents = 1;
	g_ovNumEvents = 0;

	while ( CL_ReadDemoMessage() )
	{
		snapshot_t snap;
		if ( !CL_GetSnapshot( cl.snap.messageNum, &snap ) )
			continue;
		int t = snap.serverTime;

		if ( firstTime < 0 )
		{
			firstTime = t;
			const char *si = CL_ConfigString( 0 );
			Q_strncpyz( mapname,  Info_ValueForKey( si, "mapname" ),    sizeof( mapname ) );
			Q_strncpyz( gametype, Info_ValueForKey( si, "g_gametype" ), sizeof( gametype ) );
			// in tdm/dm every kill moves the score — the killfeed already shows that,
			// so interleaved score lines are only printed for objective gametypes
			objective = strcmp( gametype, "tdm" ) != 0 && strcmp( gametype, "dm" ) != 0;
			if ( !html )
				printf( "=== %s  -  %s ===\n\n", mapname, gametype );
		}
		int rel = t - firstTime;
		lastTime = rel;

		// weapon-name list (CS_WEAPONS = 7) is a space-separated, index-ordered list
		if ( !haveWeapons && CL_ConfigString( 7 )[ 0 ] )
		{
			static char wbuf[ 2048 ];
			Q_strncpyz( wbuf, CL_ConfigString( 7 ), sizeof( wbuf ) );
			char *tok = strtok( wbuf, " " );
			while ( tok && nWeapons < 256 ) { Q_strncpyz( weapons[ nWeapons++ ], tok, 40 ); tok = strtok( NULL, " " ); }
			haveWeapons = 1;
		}

		for ( int i = 0; i < snap.numClients; i++ )
		{
			clientState_t *c = &snap.clients[ i ];
			if ( c->number >= 0 && c->number < MAX_CLIENTS )
			{
				StripColors( names[ c->number ], c->name, 32 );
				Q_strncpyz( rawnames[ c->number ], c->name, 32 );
			}
		}

		// obituary entities persist several frames — emit each kill only when its
		// entity first appears (not present in the previous frame).
		int curObit[ 64 ], nCur = 0;
		for ( int i = 0; i < snap.numEntities; i++ )
		{
			entityState_t *es = &snap.entities[ i ];
			if ( es->eType != 208 )
				continue;
			int num = es->number;
			if ( nCur < 64 ) curObit[ nCur++ ] = num;
			int isNew = 1;
			for ( int k = 0; k < nPrev; k++ ) if ( prevObit[ k ] == num ) { isNew = 0; break; }
			if ( !isNew )
				continue;

			int victim = es->otherEntityNum, attacker = es->attackerEntityNum, parm = es->eventParm;
			const char *vn = ( victim >= 0 && victim < MAX_CLIENTS && names[ victim ][ 0 ] ) ? names[ victim ] : "?";
			const char *an;
			if ( attacker >= MAX_CLIENTS )                an = "world";
			else if ( attacker == victim )                an = "(self)";
			else an = ( attacker >= 0 && attacker < MAX_CLIENTS && names[ attacker ][ 0 ] ) ? names[ attacker ] : "?";

			char how[ 48 ];
			int  headshot = ( parm & 0x80 ) && ( parm & 0x7f ) == 8;
			// The weapon index is 1-based: CS_WEAPONS is built from weapon index 1
			// (SaveRegisteredWeapons in CoD2rev), so token[0] is weapon 1 and weapon
			// `parm` lives at token[parm-1]. Using weapons[parm] shifted every weapon
			// by one (frag kills showed as smoke, shotgun showed as binoculars, etc.).
			if ( parm & 0x80 )                            Q_strncpyz( how, MeansOfDeathName( parm & 0x7f ), sizeof( how ) );
			else if ( parm >= 1 && parm - 1 < nWeapons )  Q_strncpyz( how, weapons[ parm - 1 ], sizeof( how ) );
			else                                          snprintf( how, sizeof( how ), "weapon %d", parm );

			if ( victim >= 0 && victim < MAX_CLIENTS ) stDeaths[ victim ]++;
			if ( attacker >= 0 && attacker < MAX_CLIENTS && attacker != victim )
			{
				stKills[ attacker ]++;
				if ( headshot ) stHS[ attacker ]++;
			}

			if ( html )
			{
				if ( attacker >= 0 && attacker < MAX_CLIENTS && rawnames[ attacker ][ 0 ] && attacker != victim )
					HtmlColorName( h1, rawnames[ attacker ], sizeof( h1 ) );
				else
					HtmlEscape( h1, an, sizeof( h1 ) );
				if ( victim >= 0 && victim < MAX_CLIENTS && rawnames[ victim ][ 0 ] )
					HtmlColorName( h2, rawnames[ victim ], sizeof( h2 ) );
				else
					HtmlEscape( h2, vn, sizeof( h2 ) );
				// Game-style middle cell: killer [icon] victim. A headshot/melee/etc.
				// obituary carries the death TYPE, not the gun, so it shows the death
				// icon; a normal kill shows the weapon icon. No-icon weapons fall back
				// to the bracketed name. (We never know the gun on a headshot, so the
				// headshot icon stands alone.)
				const char *icon;
				if ( parm & 0x80 )
					icon = MarkIcon( IconForDeath( how ) );   // headshot/melee/falling/crush/suicide
				else
					icon = MarkIcon( IconForWeapon( how ) );  // a normal weapon kill

				char mid[ 256 ];
				if ( icon )
					snprintf( mid, sizeof( mid ),
						"<span class=\"ic ic-%s\" title=\"%s\"></span>", icon, how );
				else
					snprintf( mid, sizeof( mid ), "<span class=\"w%s\">%s</span>",
						headshot ? " hs" : "", how );

				// killer (icon) victim, like the in-game killfeed (no arrow — the icon
				// is the separator). The killer cell is right-aligned so every icon and
				// victim line up in two columns down the feed.
				snprintf( row, sizeof( row ),
					"<tr class=\"k\"><td>%d:%02d</td><td class=\"kf\">"
					"<span class=\"kr\">%s</span>%s<span class=\"vc\">%s</span></td></tr>\n",
					rel / 60000, ( rel / 1000 ) % 60, h1, mid, h2 );
				HtmlAppend( row );
			}
			else
			{
				printf( "  %d:%02d  %-18s >> %-18s  [%s]\n", rel / 60000, ( rel / 1000 ) % 60, an, vn, how );
			}
			kills++;
		}
		nPrev = nCur;
		for ( int k = 0; k < nPrev; k++ ) prevObit[ k ] = curObit[ k ];

		// events collected while decoding this message — interleave at this frame's time
		for ( int k = 0; k < g_ovNumEvents; k++ )
		{
			ovEvent_t *ev = &g_ovEvents[ k ];

			if ( ev->kind == OV_SCORE_ALLIES || ev->kind == OV_SCORE_AXIS )
			{
				int *slot = ( ev->kind == OV_SCORE_ALLIES ) ? &scoreAllies : &scoreAxis;
				int known = ( *slot != -999999 );
				if ( *slot != ev->value )
				{
					*slot = ev->value;
					if ( objective && known )   // skip the initial sync, report real changes
					{
						if ( html )
						{
							snprintf( row, sizeof( row ),
								"<tr class=\"s\"><td>%d:%02d</td><td>score &mdash; allies %d : %d axis</td></tr>\n",
								rel / 60000, ( rel / 1000 ) % 60,
								scoreAllies == -999999 ? 0 : scoreAllies,
								scoreAxis   == -999999 ? 0 : scoreAxis );
							HtmlAppend( row );
						}
						else
							printf( "  %d:%02d  -- score  allies %d : %d axis --\n",
								rel / 60000, ( rel / 1000 ) % 60,
								scoreAllies == -999999 ? 0 : scoreAllies,
								scoreAxis   == -999999 ? 0 : scoreAxis );
					}
				}
				continue;
			}

			char clean[ 512 ];
			StripColors( clean, ev->text, sizeof( clean ) );
			PrettifyLocKeys( clean, sizeof( clean ) );

			if ( ev->kind == OV_ANNOUNCE )
			{
				// Tag the print type: f = iprintln (the bottom-left feed), g = iprintlnbold
				// (the bold centred print). e = allClientsPrint has no distinct tag.
				const char *tag = "";
				if ( ev->verb == 'f' )      tag = "(println) ";
				else if ( ev->verb == 'g' ) tag = "(printlnbold) ";
				if ( html )
				{
					HtmlEscape( h1, clean, sizeof( h1 ) );
					snprintf( row, sizeof( row ),
						"<tr class=\"a\"><td>%d:%02d</td><td><span class=\"pt\">%s</span>%s</td></tr>\n",
						rel / 60000, ( rel / 1000 ) % 60, tag, h1 );
					HtmlAppend( row );
				}
				else
					printf( "  %d:%02d  * %s%s\n", rel / 60000, ( rel / 1000 ) % 60, tag, clean );
				notes++;
			}
			else
			{
				const char *txt = clean;
				char buf[ 540 ];
				if ( !strncmp( txt, "(GAME_DEAD)", 11 ) )
				{
					snprintf( buf, sizeof( buf ), "(dead) %s", txt + 11 );
					txt = buf;
				}
				else if ( !strncmp( txt, "(GAME_ALLIES)", 13 ) ) txt += 13;   // "(team)" already shown
				else if ( !strncmp( txt, "(GAME_AXIS)", 11 ) )   txt += 11;
				if ( html )
				{
					HtmlEscape( h1, txt, sizeof( h1 ) );
					snprintf( row, sizeof( row ),
						"<tr class=\"c\"><td>%d:%02d</td><td>%s%s</td></tr>\n",
						rel / 60000, ( rel / 1000 ) % 60,
						ev->kind == OV_TEAMCHAT ? "<span class=\"tc\">(team)</span> " : "", h1 );
					HtmlAppend( row );
				}
				else
					printf( "  %d:%02d  %s%s\n", rel / 60000, ( rel / 1000 ) % 60,
						ev->kind == OV_TEAMCHAT ? "(team) " : "", txt );
				chats++;
			}
		}
		g_ovNumEvents = 0;
	}

	g_collectEvents = 0;
	fclose( demo.demofile );
	demo.demofile = NULL;
	g_quietLog = 0;

	// players sorted by kills for the summary table
	int order[ MAX_CLIENTS ], nPlayers = 0;
	for ( int i = 0; i < MAX_CLIENTS; i++ )
		if ( stKills[ i ] || stDeaths[ i ] )
			order[ nPlayers++ ] = i;
	for ( int i = 0; i < nPlayers; i++ )
		for ( int k = i + 1; k < nPlayers; k++ )
			if ( stKills[ order[ k ] ] > stKills[ order[ i ] ] )
			{
				int tmp = order[ i ]; order[ i ] = order[ k ]; order[ k ] = tmp;
			}

	if ( html )
	{
		FILE *hf = fopen( htmlPath, "w" );
		if ( !hf )
		{
			printf( "error: cannot write '%s'\n", htmlPath );
			return 1;
		}
		fprintf( hf,
			"<!doctype html>\n<html><head><meta charset=\"utf-8\">\n"
			"<title>%s - %s</title>\n<style>\n"
			"body{background:#15171c;color:#cfd3da;font:14px/1.5 'Segoe UI',sans-serif;margin:24px auto;max-width:880px;padding:0 16px}\n"
			"h1{font-size:20px;color:#fff;margin:0} .meta{color:#8a90a0;margin:4px 0 18px}\n"
			".score{font-size:16px;color:#fff;background:#22252d;border-radius:8px;padding:10px 16px;display:inline-block;margin-bottom:18px}\n"
			"table{border-collapse:collapse;width:100%%;margin-bottom:24px}\n"
			"th{text-align:left;color:#8a90a0;font-weight:600;border-bottom:1px solid #333845;padding:6px 10px}\n"
			"td{padding:4px 10px;border-bottom:1px solid #20232b;font-family:Consolas,monospace;font-size:13px}\n"
			"td:first-child{color:#6b7184;width:52px;white-space:nowrap}\n"
			"tr.c td{color:#9fd49f} tr.a td{color:#d4c98f} tr.s td{color:#7fd4d4;font-weight:600}\n"
			".w{color:#8a90a0;font-size:12px} .w:before{content:'['} .w:after{content:']'}\n"
			".hs{color:#ff6b6b} .tc{color:#6b7184}\n"
			".pt{color:#6b7184;font-size:11px}\n"   // iprintln/iprintlnbold type tag
			// game-style killfeed icon: a fixed box, the PNG fitted inside it
			".ic{display:inline-block;width:44px;height:22px;vertical-align:middle;margin:0 4px;"
			"background-size:contain;background-repeat:no-repeat;background-position:center}\n"
			// killfeed row: killer (right-aligned) | icon | victim, so columns line up
			".kf{display:flex;align-items:center;gap:6px}\n"
			".kr{flex:1;text-align:right} .vc{flex:1;text-align:left}\n",
			mapname, gametype );

		// Per-icon background-image rules, emitted only for icons that actually appear
		// (g_iconUsed set during the timeline pass) so the file carries no dead weight.
		for ( int i = 0; i < g_numEmbeddedIcons; i++ )
		{
			if ( !g_iconUsed[ i ] ) continue;
			fprintf( hf, ".ic-%s{background-image:url(data:image/png;base64,%s)}\n",
				g_embeddedIcons[ i ].name, g_embeddedIcons[ i ].b64 );
		}

		fprintf( hf,
			"</style></head><body>\n"
			"<h1>%s &mdash; %s</h1>\n"
			"<div class=\"meta\">%s &middot; length %d:%02d &middot; %d kills &middot; %d chat &middot; %d announcements</div>\n",
			mapname, gametype, path,
			lastTime / 60000, ( lastTime / 1000 ) % 60, kills, chats, notes );

		if ( scoreAllies != -999999 || scoreAxis != -999999 )
			fprintf( hf, "<div class=\"score\">final score &nbsp; allies %d : %d axis</div>\n",
				scoreAllies == -999999 ? 0 : scoreAllies,
				scoreAxis   == -999999 ? 0 : scoreAxis );

		fprintf( hf, "<table><tr><th>player</th><th>kills</th><th>deaths</th><th>headshots</th></tr>\n" );
		for ( int i = 0; i < nPlayers; i++ )
		{
			int c = order[ i ];
			if ( rawnames[ c ][ 0 ] ) HtmlColorName( h1, rawnames[ c ], sizeof( h1 ) );
			else                      snprintf( h1, sizeof( h1 ), "client %d", c );
			fprintf( hf, "<tr><td style=\"width:auto;color:#cfd3da\">%s</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
				h1, stKills[ c ], stDeaths[ c ], stHS[ c ] );
		}
		fprintf( hf, "</table>\n<table><tr><th>time</th><th>event</th></tr>\n" );
		if ( g_htmlRows ) fwrite( g_htmlRows, 1, g_htmlLen, hf );
		fprintf( hf, "</table>\n<div class=\"meta\">generated by CoD2-DemoTool</div>\n</body></html>\n" );
		fclose( hf );
		printf( "overview: %d kills, %d chat, %d announcements  ->  %s\n", kills, chats, notes, htmlPath );
		return 0;
	}

	if ( scoreAllies != -999999 || scoreAxis != -999999 )
		printf( "\n  final score  allies %d : %d axis\n",
			scoreAllies == -999999 ? 0 : scoreAllies,
			scoreAxis   == -999999 ? 0 : scoreAxis );

	if ( nPlayers )
	{
		printf( "\n  %-22s %6s %7s %10s\n", "player", "kills", "deaths", "headshots" );
		for ( int i = 0; i < nPlayers && i < 16; i++ )
		{
			int c = order[ i ];
			printf( "  %-22s %6d %7d %10d\n",
				names[ c ][ 0 ] ? names[ c ] : "?", stKills[ c ], stDeaths[ c ], stHS[ c ] );
		}
	}
	printf( "\n  %d kills, %d chat messages, %d announcements\n", kills, chats, notes );
	return 0;
}

static void Usage( void )
{
	printf( "CoD2-DemoTool - offline CoD2 .dm_1 demo editor (all versions)\n\n" );
	printf( "usage:\n" );
	printf( "  cod2-demotool --info  <demo.dm_1>            show what a demo is (version, map, length)\n" );
	printf( "  cod2-demotool --dump  <demo.dm_1>            write a verbose per-frame log (debug)\n" );
	printf( "  cod2-demotool --commands <demo.dm_1>         list the server commands (chat, events) by time\n" );
	printf( "  cod2-demotool --hudscan <demo.dm_1>          list the scripted HUD elements (what --remove-hud strips)\n" );
	printf( "  cod2-demotool --overview <demo.dm_1> [out.html]   match timeline: kills, chat, score (HTML optional)\n" );
	printf( "  cod2-demotool --copy       <in.dm_1> <out.dm_1>          re-encode unchanged (round-trip proof)\n" );
	printf( "  cod2-demotool --verify     <demo.dm_1>                   byte-compare round-trip (is the reverse 1:1?)\n" );
	printf( "  cod2-demotool --skip-dead  <in.dm_1> <out.dm_1> [keep-killcam] [keep-spectate]  remove dead-time\n" );
	printf( "  cod2-demotool --cut        <in.dm_1> <out.dm_1> <s> <e>  keep only the time range [s, e]\n" );
	printf( "  cod2-demotool --clean      <in.dm_1> <out.dm_1> <what>   strip chat / centertext / whitetext / all\n" );
	printf( "  cod2-demotool --remove-hud <in.dm_1> <out.dm_1> [keep <shader>]   strip server-set HUD elements\n" );
	printf( "  cod2-demotool --scale-score <in.dm_1> <out.dm_1> <mult> scale the +N score popups (e.g. 0.2)\n" );
	printf( "  cod2-demotool --split-map   <demo.dm_1>                split a multi-map demo into one file per map\n" );
	printf( "  cod2-demotool --split-match <demo.dm_1>                split into one file per match (fast_restart)\n" );
	printf( "  cod2-demotool --convert    <in.dm_1> <out.dm_1> <ver> re-tag version: 115|117|118|119|120\n" );
	printf( "  cod2-demotool --merge      <A.dm_1> <B.dm_1> <out.dm_1>  join two same-map demos (experimental)\n\n" );
	printf( "  times are mm:ss from the demo start (or plain seconds), or the words 'start' / 'end'\n" );
	printf( "  e.g.  cod2-demotool --cut game.dm_1 clip.dm_1 1:30 3:00\n\n" );
	printf( "  batch: drop several demos at once -> a summary for each; with --overview, a <demo>.html each\n" );
}

// Replace a path's extension (or append) with .html — "match.dm_1" -> "match.html".
static void HtmlPathFor( const char *demo, char *out, int outsize )
{
	Q_strncpyz( out, demo, outsize );
	char *dot = strrchr( out, '.' );
	char *slash = strrchr( out, '/' );
	char *bslash = strrchr( out, '\\' );
	if ( bslash > slash ) slash = bslash;
	if ( dot && dot > slash ) *dot = 0;
	int len = (int)strlen( out );
	snprintf( out + len, outsize - len, ".html" );
}

// The CLI entry point. The GUI build (gui_win32.cpp #defines GUI_BUILD and #includes
// this file) provides its own WinMain and calls the Cmd_* functions in-process, so the
// console main is compiled out there to avoid a duplicate entry point.
#ifndef GUI_BUILD
int main( int argc, char **argv )
{
	const char *mode = NULL;
	const char *p[ 256 ];
	int np = 0;

	for ( int i = 1; i < argc; i++ )
	{
		if ( argv[ i ][ 0 ] == '-' )
			mode = argv[ i ];
		else if ( np < 256 )
			p[ np++ ] = argv[ i ];
	}

	if ( np == 0 )
	{
		Usage();
		return 1;
	}

	// `--overview demo.dm_1 out.html` is the explicit single-demo HTML form — let it
	// fall through to Cmd_Overview below, don't treat out.html as a second demo. Only
	// inspect p[1] when there actually is one (np == 2); p[1] is NULL otherwise.
	int explicitHtml = ( mode && !strcmp( mode, "--overview" ) && np == 2 );
	if ( explicitHtml )
	{
		int n = (int)strlen( p[ 1 ] );
		if ( n < 5 || strcmp( p[ 1 ] + n - 5, ".html" ) != 0 ) explicitHtml = 0;
	}

	// Batch / drag-and-drop: several demos dropped on the tool with no explicit
	// mode -> --info each; with --overview -> write a <demo>.html beside each.
	if ( !explicitHtml && np > 1 && ( !mode || !strcmp( mode, "--info" ) || !strcmp( mode, "--overview" ) ) )
	{
		int rc = 0, doHtml = ( mode && !strcmp( mode, "--overview" ) );
		for ( int i = 0; i < np; i++ )
		{
			if ( doHtml )
			{
				char htmlOut[ 1024 ];
				HtmlPathFor( p[ i ], htmlOut, sizeof( htmlOut ) );
				rc |= Cmd_Overview( p[ i ], htmlOut );
			}
			else
			{
				if ( i ) printf( "\n" );
				rc |= Cmd_Info( p[ i ] );
			}
		}
		return rc ? 1 : 0;
	}

	if ( !mode ) mode = "--info";
	const char *path = p[ 0 ], *path2 = ( np > 1 ) ? p[ 1 ] : NULL;
	const char *path3 = ( np > 2 ) ? p[ 2 ] : NULL, *path4 = ( np > 3 ) ? p[ 3 ] : NULL;

	if ( !path )
	{
		Usage();
		return 1;
	}

	if ( !strcmp( mode, "--verify" ) )
		return Cmd_Verify( path );

	if ( !strcmp( mode, "--copy" ) )
	{
		if ( !path2 )
		{
			printf( "usage: cod2-demotool --copy <in.dm_1> <out.dm_1>\n" );
			return 1;
		}
		return Cmd_Copy( path, path2 );
	}

	if ( !strcmp( mode, "--skip-dead" ) )
	{
		if ( !path2 )
		{
			printf( "usage: cod2-demotool --skip-dead <in.dm_1> <out.dm_1> [keep-killcam] [keep-spectate]\n" );
			return 1;
		}
		int keepKillcam = 0, keepSpectate = 0;
		for ( int i = 2; i < np; i++ )         // scan trailing keywords in any order
		{
			if ( !strcmp( p[ i ], "keep-killcam" ) )  keepKillcam = 1;
			if ( !strcmp( p[ i ], "keep-spectate" ) ) keepSpectate = 1;
		}
		return Cmd_SkipDead( path, path2, keepKillcam, keepSpectate );
	}

	// --clean <in> <out> <what...> : strip server-command text (chat / centertext / whitetext).
	// Just a copy with the filters enabled, so it composes with the round-trip writer.
	if ( !strcmp( mode, "--clean" ) )
	{
		if ( !path2 || !path3 )
		{
			printf( "usage: cod2-demotool --clean <in.dm_1> <out.dm_1> <chat|centertext|whitetext|all> ...\n" );
			return 1;
		}
		for ( int i = 2; i < np; i++ )
		{
			if ( !strcmp( p[ i ], "chat" )       || !strcmp( p[ i ], "all" ) ) g_removeChat = 1;
			if ( !strcmp( p[ i ], "centertext" ) || !strcmp( p[ i ], "all" ) ) g_removeCenterText = 1;
			if ( !strcmp( p[ i ], "whitetext" )  || !strcmp( p[ i ], "all" ) ) g_removeWhiteText = 1;
		}
		if ( !g_removeChat && !g_removeCenterText && !g_removeWhiteText )
		{
			printf( "  nothing to clean — name at least one of: chat centertext whitetext all\n" );
			return 1;
		}
		return Cmd_Copy( path, path2 );
	}

	// --remove-hud <in> <out> [keep <shader-substring>] : strip server-set HUD elements.
	if ( !strcmp( mode, "--remove-hud" ) )
	{
		if ( !path2 )
		{
			printf( "usage: cod2-demotool --remove-hud <in.dm_1> <out.dm_1> [keep <shader-substring>]\n" );
			return 1;
		}
		g_removeHud = 1;
		for ( int i = 2; i + 1 < np; i++ )
			if ( !strcmp( p[ i ], "keep" ) ) g_keepShader = p[ i + 1 ];
		int rc = Cmd_Copy( path, path2 );
		printf( "  removed all scripted HUD elements (kill cards, server logos, score popups).\n"
		        "  the ammo/grenade readout (bottom-right) and the compass are NOT HUD elements —\n"
		        "  the client draws them from the player's own weapon/ammo state, so they aren't in\n"
		        "  the demo to remove. Hide the compass in-game with  cg_drawcompass 0.\n" );
		return rc;
	}

	// --scale-score <in> <out> <mult> : scale the +N score-popup hud elements.
	if ( !strcmp( mode, "--scale-score" ) )
	{
		if ( !path2 || !path3 )
		{
			printf( "usage: cod2-demotool --scale-score <in.dm_1> <out.dm_1> <multiplier>\n" );
			return 1;
		}
		g_scaleScore = 1;
		g_scoreMult  = (float)atof( path3 );
		return Cmd_Copy( path, path2 );
	}

	if ( !strcmp( mode, "--merge" ) )
	{
		if ( !path2 || !path3 )
		{
			printf( "usage: cod2-demotool --merge <A.dm_1> <B.dm_1> <out.dm_1>   (experimental, same map/mod)\n" );
			return 1;
		}
		return Cmd_Merge( path, path2, path3 );
	}

	if ( !strcmp( mode, "--split-map" ) )
		return Cmd_Split( path, 0 );

	if ( !strcmp( mode, "--split-match" ) )
		return Cmd_Split( path, 1 );

	// --convert <in> <out> <protocol> : re-tag the demo's version (115/117/118/119/120).
	// CoD2's wire format is identical across versions, so this only rewrites the
	// protocol/shortversion in the serverinfo configstring.
	if ( !strcmp( mode, "--convert" ) )
	{
		if ( !path2 || !path3 )
		{
			printf( "usage: cod2-demotool --convert <in.dm_1> <out.dm_1> <protocol: 115|117|118|119|120>\n" );
			return 1;
		}
		g_convertProtocol = atoi( path3 );
		if ( !ProtocolShortVersion( g_convertProtocol )[ 0 ] )
		{
			printf( "  unknown protocol %s — use 115 (1.0), 117 (1.2), 118 (1.3), 119/120 (1.4)\n", path3 );
			return 1;
		}
		return Cmd_Copy( path, path2 );
	}

	if ( !strcmp( mode, "--cut" ) )
	{
		if ( !path2 || !path3 || !path4 )
		{
			printf( "usage: cod2-demotool --cut <in.dm_1> <out.dm_1> <start> <end>\n" );
			printf( "       times are mm:ss from the demo start (or seconds), or 'start' / 'end'\n" );
			return 1;
		}
		return Cmd_Cut( path, path2, path3, path4 );
	}

	if ( !strcmp( mode, "--dump" ) )
		return Cmd_Dump( path );

	if ( !strcmp( mode, "--deadscan" ) )
		return Cmd_DeadScan( path );

	if ( !strcmp( mode, "--hudscan" ) )
		return Cmd_HudScan( path );

	if ( !strcmp( mode, "--commands" ) )
		return Cmd_Commands( path );

	if ( !strcmp( mode, "--overview" ) )
		return Cmd_Overview( path, path2 );      // optional second path = write an HTML page

	return Cmd_Info( path );
}
#endif // GUI_BUILD
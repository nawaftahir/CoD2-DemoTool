#ifndef _COD_DEMOTOOL_WRITER_H_
#define _COD_DEMOTOOL_WRITER_H_

// ======================================================================
//  CoD-DemoTool — snapshot/gamestate ENCODER
//
//  The exact inverse of the seeded decoder (reader.cpp). Ported from the
//  reverse-engineered CoD2 server (Refrences/CoD2rev_Server) and verified
//  field-for-field against this file's MSG_ReadDelta* functions so a
//  decode->encode round-trip is byte-identical.
//
//  Two field-encoding schemes (matching the reader):
//   - general  (MSG_WriteDeltaField): entity / client / hudelem / objective.
//                floats carry a zero-bit then a small-int/full-float bit;
//                integers carry a present-bit.
//   - inline   (inside MSG_WriteDeltaPlayerstate): floats are small-int/full
//                only (no zero-bit); integers are always written (no bit).
//
//  This header is #included once into reader.cpp (single translation unit),
//  AFTER the field tables, msg_t and the MSG_Read* helpers are defined.
// ======================================================================

// --- bit-width literals (use the exact values the reader consumes) ---
#define CLIENTNUM_BITS      6      // client index + STAT_IDENT_CLIENT_NUM
#define HUDELEM_BITS        5      // hud "inuse" and "last changed" counters
#define STATSBITS_COUNT     6      // playerstate stats change-mask width
#define OBJSTATE_BITS       3      // objective.state width
#define HUDELEM_COORD_BIAS  512    // bits == -99 small-int bias (2 bits + byte)

// ----------------------------------------------------------------------
//  A) MSG write primitives  (CoD2rev qcommon/msg_mp.cpp)
// ----------------------------------------------------------------------

void MSG_WriteBit0( msg_t *msg )
{
	int bit = msg->bit & 7;
	if ( msg->maxsize <= msg->cursize ) { msg->overflowed = qtrue; return; }
	if ( !bit )
	{
		msg->bit = msg->cursize * 8;
		msg->data[ msg->cursize ] = 0;
		msg->cursize++;
	}
	msg->bit++;
}

void MSG_WriteBit1( msg_t *msg )
{
	int bit = msg->bit & 7;
	if ( msg->maxsize <= msg->cursize ) { msg->overflowed = qtrue; return; }
	if ( !bit )
	{
		msg->bit = msg->cursize * 8;
		msg->data[ msg->cursize ] = 0;
		msg->cursize++;
	}
	msg->data[ msg->bit >> 3 ] |= 1 << bit;
	msg->bit++;
}

void MSG_WriteBits( msg_t *msg, int value, int bits )
{
	for ( int i = 0; i < bits; i++ )
	{
		if ( value & 1 )
			MSG_WriteBit1( msg );
		else
			MSG_WriteBit0( msg );
		value = ( value >> 1 );
	}
}

void MSG_WriteByte( msg_t *msg, int c )
{
	int newsize = msg->cursize + (int)sizeof( uint8_t );
	if ( newsize <= msg->maxsize )
	{
		*(uint8_t *)&msg->data[ msg->cursize ] = (uint8_t)c;
		msg->cursize = newsize;
		return;
	}
	msg->overflowed = qtrue;
}

void MSG_WriteShort( msg_t *msg, int c )
{
	int newsize = msg->cursize + (int)sizeof( uint16_t );
	if ( newsize <= msg->maxsize )
	{
		*(uint16_t *)&msg->data[ msg->cursize ] = (uint16_t)LittleShort( c );
		msg->cursize = newsize;
		return;
	}
	msg->overflowed = qtrue;
}

void MSG_WriteLong( msg_t *msg, int c )
{
	int newsize = msg->cursize + (int)sizeof( uint32_t );
	if ( newsize <= msg->maxsize )
	{
		*(uint32_t *)&msg->data[ msg->cursize ] = (uint32_t)LittleLong( c );
		msg->cursize = newsize;
		return;
	}
	msg->overflowed = qtrue;
}

void MSG_WriteData( msg_t *msg, const void *data, int length )
{
	int newsize = msg->cursize + length;
	if ( newsize <= msg->maxsize )
	{
		Com_Memcpy( &msg->data[ msg->cursize ], data, length );
		msg->cursize = newsize;
		return;
	}
	msg->overflowed = qtrue;
}

// Raw string writer (no char-cleaning) — preserves the exact bytes that the
// decoder read, which is what byte-identical round-trip requires.
void MSG_WriteBigStringRaw( msg_t *msg, const char *s )
{
	int l = (int)strlen( s );
	MSG_WriteData( msg, s, l + 1 );   // include the terminating NUL
}

void MSG_WriteAngle16( msg_t *msg, float f )
{
	MSG_WriteShort( msg, ANGLE2SHORT( f ) );
}

// Write `bits` bits of an integer, low part first then whole bytes. Inverse of
// the reader's default integer field read (partial bits + bytes, no xor).
void MSG_WriteValueNoXor( msg_t *msg, int value, int bits )
{
	int absbits = abs( bits );
	int partial = absbits & 7;

	if ( partial )
	{
		MSG_WriteBits( msg, value, partial );
		absbits -= partial;
		value >>= partial;
	}
	while ( absbits )
	{
		MSG_WriteByte( msg, value );
		value >>= 8;
		absbits -= 8;
	}
}

// ----------------------------------------------------------------------
//  B) Delta encoders
// ----------------------------------------------------------------------

// General per-field encoder (entity / client / hudelem / objective).
// Inverse of MSG_ReadDeltaField (reader.cpp).
void MSG_WriteDeltaField( msg_t *msg, const void *from, const void *to, const netField_t *field )
{
	int *fromF = ( int * )( (byte *)from + field->offset );
	int *toF   = ( int * )( (byte *)to   + field->offset );

	if ( *fromF == *toF )
	{
		MSG_WriteBit0( msg );      // unchanged
		return;
	}
	MSG_WriteBit1( msg );          // changed

	switch ( field->bits )
	{
	case 0: // float
	{
		float fullFloat = *(float *)toF;
		int trunc = (int)fullFloat;
		if ( fullFloat == 0.0f )
		{
			MSG_WriteBit0( msg );  // zero
			return;
		}
		MSG_WriteBit1( msg );      // non-zero
		if ( trunc == fullFloat && (unsigned)( trunc + FLOAT_INT_BIAS ) < ( 1 << FLOAT_INT_BITS ) )
		{
			MSG_WriteBit0( msg );  // small integer
			MSG_WriteBits( msg, trunc + FLOAT_INT_BIAS, 5 );
			MSG_WriteByte( msg, ( trunc + FLOAT_INT_BIAS ) >> 5 );
		}
		else
		{
			MSG_WriteBit1( msg );  // full float
			MSG_WriteLong( msg, *toF );
		}
		return;
	}
	case -99: // hudelem coordinate
	{
		float fullFloat = *(float *)toF;
		int trunc = (int)fullFloat;
		if ( fullFloat == 0.0f )
		{
			MSG_WriteBit0( msg );
			return;
		}
		MSG_WriteBit1( msg );
		if ( trunc == fullFloat && (unsigned)( trunc + HUDELEM_COORD_BIAS ) < ( 1 << 10 ) )
		{
			MSG_WriteBit0( msg );
			MSG_WriteBits( msg, trunc + HUDELEM_COORD_BIAS, 2 );
			MSG_WriteByte( msg, ( trunc + HUDELEM_COORD_BIAS ) >> 2 );
		}
		else
		{
			MSG_WriteBit1( msg );
			MSG_WriteLong( msg, *toF );
		}
		return;
	}
	case -100: // view angle (angle16)
		if ( *toF )
		{
			MSG_WriteBit1( msg );
			MSG_WriteAngle16( msg, *(float *)toF );
		}
		else
		{
			MSG_WriteBit0( msg );
		}
		return;
	default: // integer
		if ( *toF )
		{
			MSG_WriteBit1( msg );
			MSG_WriteValueNoXor( msg, *toF, field->bits );
		}
		else
		{
			MSG_WriteBit0( msg );
		}
		return;
	}
}

// Objective helper: "all fields" (bit1) or "no change" (bit0).
// Inverse of MSG_ReadDeltaObjective.
void MSG_WriteDeltaFields( msg_t *msg, const void *from, const void *to, int numFields, const netField_t *stateFields )
{
	for ( int i = 0; i < numFields; i++ )
	{
		int *fromF = ( int * )( (byte *)from + stateFields[ i ].offset );
		int *toF   = ( int * )( (byte *)to   + stateFields[ i ].offset );
		if ( *fromF != *toF )
		{
			MSG_WriteBit1( msg );
			for ( int k = 0; k < numFields; k++ )
				MSG_WriteDeltaField( msg, from, to, &stateFields[ k ] );
			return;
		}
	}
	MSG_WriteBit0( msg );
}

// entity/client struct encoder. Inverse of MSG_ReadDeltaStruct.
// The index (entity/client number) is written here; the caller writes nothing.
// `bChangeBit` (clients) prefixes a "present" bit before the index.
void MSG_WriteDeltaStruct( msg_t *msg, const void *from, const void *to, qboolean force,
                           int numFields, int indexBits, const netField_t *stateFields, qboolean bChangeBit )
{
	if ( to == NULL )
	{
		// removal: encode the (old) number, then the remove bit
		if ( bChangeBit ) MSG_WriteBit1( msg );
		MSG_WriteBits( msg, *(int *)from, indexBits );
		MSG_WriteBit1( msg );          // remove
		return;
	}

	int lc = 0;
	for ( int i = 0; i < numFields; i++ )
	{
		int *fromF = ( int * )( (byte *)from + stateFields[ i ].offset );
		int *toF   = ( int * )( (byte *)to   + stateFields[ i ].offset );
		if ( *fromF != *toF )
			lc = i + 1;
	}

	if ( lc )
	{
		if ( bChangeBit ) MSG_WriteBit1( msg );
		MSG_WriteBits( msg, *(int *)to, indexBits );
		MSG_WriteBit0( msg );          // not a remove
		MSG_WriteBit1( msg );          // has delta
		MSG_WriteByte( msg, lc );
		for ( int i = 0; i < lc; i++ )
			MSG_WriteDeltaField( msg, from, to, &stateFields[ i ] );
	}
	else
	{
		if ( !force )
			return;
		if ( bChangeBit ) MSG_WriteBit1( msg );
		MSG_WriteBits( msg, *(int *)to, indexBits );
		MSG_WriteBit0( msg );          // not a remove
		MSG_WriteBit0( msg );          // no delta
	}
}

void MSG_WriteDeltaEntity( msg_t *msg, entityState_t *from, entityState_t *to, qboolean force )
{
	MSG_WriteDeltaStruct( msg, (byte *)from, (byte *)to, force,
		COUNT_OF( entityStateFields ), GENTITYNUM_BITS, entityStateFields, qfalse );
}

void MSG_WriteDeltaClient( msg_t *msg, clientState_t *from, clientState_t *to, qboolean force )
{
	clientState_t dummy;
	if ( !from )
	{
		from = &dummy;
		Com_Memset( &dummy, 0, sizeof( dummy ) );
	}
	MSG_WriteDeltaStruct( msg, (byte *)from, (byte *)to, force,
		COUNT_OF( clientStateFields ), CLIENTNUM_BITS, clientStateFields, qtrue );
}

// Inverse of MSG_ReadDeltaHudElems.
void MSG_WriteDeltaHudElems( msg_t *msg, hudelem_t *from, hudelem_t *to, int count )
{
	int inuse;
	for ( inuse = 0; inuse < count; inuse++ )
		if ( to[ inuse ].type == HE_TYPE_FREE )
			break;

	MSG_WriteBits( msg, inuse, HUDELEM_BITS );

	for ( int i = 0; i < inuse; i++ )
	{
		int lc = 0;
		for ( int j = 0; j < (int)COUNT_OF( hudElemFields ); j++ )
		{
			int *fromF = ( int * )( (byte *)( from + i ) + hudElemFields[ j ].offset );
			int *toF   = ( int * )( (byte *)( to + i )   + hudElemFields[ j ].offset );
			if ( *fromF != *toF )
				lc = j;
		}
		MSG_WriteBits( msg, lc, HUDELEM_BITS );
		for ( int j = 0; j <= lc; j++ )
			MSG_WriteDeltaField( msg, (byte *)( from + i ), (byte *)( to + i ), &hudElemFields[ j ] );
	}
}

// Inverse of MSG_ReadDeltaPlayerstate. Non-CoD2x (protocol != 119) path.
void MSG_WriteDeltaPlayerstate( msg_t *msg, playerState_t *from, playerState_t *to )
{
	playerState_t dummy;
	if ( !from )
	{
		from = &dummy;
		Com_Memset( &dummy, 0, sizeof( dummy ) );
	}

	int lc = 0;
	for ( int i = 0; i < (int)COUNT_OF( playerStateFields ); i++ )
	{
		int *fromF = ( int * )( (byte *)from + playerStateFields[ i ].offset );
		int *toF   = ( int * )( (byte *)to   + playerStateFields[ i ].offset );
		if ( *fromF != *toF )
			lc = i + 1;
	}

	MSG_WriteByte( msg, lc );

	for ( int i = 0; i < lc; i++ )
	{
		const netField_t *field = &playerStateFields[ i ];
		int *fromF = ( int * )( (byte *)from + field->offset );
		int *toF   = ( int * )( (byte *)to   + field->offset );

		if ( *fromF == *toF )
		{
			MSG_WriteBit0( msg );      // unchanged
			continue;
		}
		MSG_WriteBit1( msg );          // changed

		if ( field->bits == 0 )
		{
			// float: small-int or full (no zero special-case here)
			float fullFloat = *(float *)toF;
			int trunc = (int)fullFloat;
			if ( trunc == fullFloat && (unsigned)( trunc + FLOAT_INT_BIAS ) < ( 1 << FLOAT_INT_BITS ) )
			{
				MSG_WriteBit0( msg );
				MSG_WriteBits( msg, trunc + FLOAT_INT_BIAS, 5 );
				MSG_WriteByte( msg, ( trunc + FLOAT_INT_BIAS ) >> 5 );
			}
			else
			{
				MSG_WriteBit1( msg );
				MSG_WriteLong( msg, *toF );
			}
		}
		else if ( field->bits == -100 )
		{
			if ( *toF )
			{
				MSG_WriteBit1( msg );
				MSG_WriteAngle16( msg, *(float *)toF );
			}
			else
			{
				MSG_WriteBit0( msg );
			}
		}
		else
		{
			// integer: always written, no present-bit
			MSG_WriteValueNoXor( msg, *toF, field->bits );
		}
	}

	// stats (6-bit change mask)
	int statsbits = 0;
	if ( to->stats[ STAT_HEALTH ]            != from->stats[ STAT_HEALTH ] )            statsbits |= 1;
	if ( to->stats[ STAT_DEAD_YAW ]          != from->stats[ STAT_DEAD_YAW ] )          statsbits |= 2;
	if ( to->stats[ STAT_MAX_HEALTH ]        != from->stats[ STAT_MAX_HEALTH ] )        statsbits |= 4;
	if ( to->stats[ STAT_IDENT_CLIENT_NUM ]  != from->stats[ STAT_IDENT_CLIENT_NUM ] )  statsbits |= 8;
	if ( to->stats[ STAT_IDENT_CLIENT_HEALTH]!= from->stats[ STAT_IDENT_CLIENT_HEALTH]) statsbits |= 0x10;
	if ( to->stats[ STAT_SPAWN_COUNT ]       != from->stats[ STAT_SPAWN_COUNT ] )       statsbits |= 0x20;

	if ( statsbits )
	{
		MSG_WriteBit1( msg );
		MSG_WriteBits( msg, statsbits, STATSBITS_COUNT );
		if ( statsbits & 1 )    MSG_WriteShort( msg, to->stats[ STAT_HEALTH ] );
		if ( statsbits & 2 )    MSG_WriteShort( msg, to->stats[ STAT_DEAD_YAW ] );
		if ( statsbits & 4 )    MSG_WriteShort( msg, to->stats[ STAT_MAX_HEALTH ] );
		if ( statsbits & 8 )    MSG_WriteBits( msg, to->stats[ STAT_IDENT_CLIENT_NUM ], CLIENTNUM_BITS );
		if ( statsbits & 0x10 ) MSG_WriteShort( msg, to->stats[ STAT_IDENT_CLIENT_HEALTH ] );
		if ( statsbits & 0x20 ) MSG_WriteByte( msg, to->stats[ STAT_SPAWN_COUNT ] );
	}
	else
	{
		MSG_WriteBit0( msg );
	}

	// ammo stored (outer present bit, then 4 groups of 16)
	int ammobits[ 4 ];
	for ( int j = 0; j < 4; j++ )
	{
		ammobits[ j ] = 0;
		for ( int i = 0; i < 16; i++ )
			if ( to->ammo[ i + j * 16 ] != from->ammo[ i + j * 16 ] )
				ammobits[ j ] |= 1 << i;
	}
	if ( ammobits[ 0 ] || ammobits[ 1 ] || ammobits[ 2 ] || ammobits[ 3 ] )
	{
		MSG_WriteBit1( msg );
		for ( int j = 0; j < 4; j++ )
		{
			if ( ammobits[ j ] )
			{
				MSG_WriteBit1( msg );
				MSG_WriteShort( msg, ammobits[ j ] );
				for ( int i = 0; i < 16; i++ )
					if ( ammobits[ j ] & ( 1 << i ) )
						MSG_WriteShort( msg, to->ammo[ i + j * 16 ] );
			}
			else
			{
				MSG_WriteBit0( msg );
			}
		}
	}
	else
	{
		MSG_WriteBit0( msg );
	}

	// ammo in clip (no outer bit; 4 groups each with its own present bit)
	for ( int j = 0; j < 4; j++ )
	{
		int clipbits = 0;
		for ( int i = 0; i < 16; i++ )
			if ( to->ammoclip[ i + j * 16 ] != from->ammoclip[ i + j * 16 ] )
				clipbits |= 1 << i;
		if ( clipbits )
		{
			MSG_WriteBit1( msg );
			MSG_WriteShort( msg, clipbits );
			for ( int i = 0; i < 16; i++ )
				if ( clipbits & ( 1 << i ) )
					MSG_WriteShort( msg, to->ammoclip[ i + j * 16 ] );
		}
		else
		{
			MSG_WriteBit0( msg );
		}
	}

	// objectives
	if ( memcmp( from->objective, to->objective, sizeof( from->objective ) ) )
	{
		MSG_WriteBit1( msg );
		for ( int i = 0; i < MAX_OBJECTIVES; i++ )
		{
			MSG_WriteBits( msg, to->objective[ i ].state, OBJSTATE_BITS );
			MSG_WriteDeltaFields( msg, &from->objective[ i ], &to->objective[ i ],
				COUNT_OF( objectiveFields ), objectiveFields );
		}
	}
	else
	{
		MSG_WriteBit0( msg );
	}

	// hud elements
	if ( memcmp( &from->hud, &to->hud, sizeof( from->hud ) ) )
	{
		MSG_WriteBit1( msg );
		MSG_WriteDeltaHudElems( msg, from->hud.archival, to->hud.archival, MAX_HUDELEMS_ARCHIVAL );
		MSG_WriteDeltaHudElems( msg, from->hud.current,  to->hud.current,  MAX_HUDELEMS_CURRENT );
	}
	else
	{
		MSG_WriteBit0( msg );
	}
}

#endif // _COD_DEMOTOOL_WRITER_H_

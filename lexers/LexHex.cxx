// Scintilla source code edit control
/** @file LexHex.cxx
 ** Lexers for Motorola S-Record and Intel HEX.
 **
 ** Written by Markus Heidelberg
 **/
// Copyright 1998-2001 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

/*
 *  Motorola S-Record file format
 * ===============================
 *
 * Each record (line) is built as follows:
 *
 *    field       digits          states
 *
 *  +----------+
 *  | start    |  1 ('S')         SCE_HEX_RECSTART
 *  +----------+
 *  | type     |  1               SCE_HEX_RECTYPE
 *  +----------+
 *  | count    |  2               SCE_HEX_BYTECOUNT, SCE_HEX_BYTECOUNT_WRONG
 *  +----------+
 *  | address  |  4/6/8           SCE_HEX_NOADDRESS, SCE_HEX_DATAADDRESS, SCE_HEX_RECCOUNT, SCE_HEX_STARTADDRESS, (SCE_HEX_ADDRESSFIELD_UNKNOWN)
 *  +----------+
 *  | data     |  0..504/502/500  SCE_HEX_DATA_ODD, SCE_HEX_DATA_EVEN, (SCE_HEX_DATA_UNKNOWN)
 *  +----------+
 *  | checksum |  2               SCE_HEX_CHECKSUM, SCE_HEX_CHECKSUM_WRONG
 *  +----------+
 *
 *
 *  Intel HEX file format
 * ===============================
 *
 * Each record (line) is built as follows:
 *
 *    field       digits          states
 *
 *  +----------+
 *  | start    |  1 (':')         SCE_HEX_RECSTART
 *  +----------+
 *  | count    |  2               SCE_HEX_BYTECOUNT, SCE_HEX_BYTECOUNT_WRONG
 *  +----------+
 *  | address  |  4               SCE_HEX_NOADDRESS, SCE_HEX_DATAADDRESS, (SCE_HEX_ADDRESSFIELD_UNKNOWN)
 *  +----------+
 *  | type     |  2               SCE_HEX_RECTYPE
 *  +----------+
 *  | data     |  0..510          SCE_HEX_DATA_ODD, SCE_HEX_DATA_EVEN, SCE_HEX_DATA_EMPTY, SCE_HEX_EXTENDEDADDRESS, SCE_HEX_STARTADDRESS, (SCE_HEX_DATA_UNKNOWN)
 *  +----------+
 *  | checksum |  2               SCE_HEX_CHECKSUM, SCE_HEX_CHECKSUM_WRONG
 *  +----------+
 *
 *
 *  General notes for all lexers
 * ===============================
 *
 * - Depending on where the helper functions are invoked, some of them have to
 *   read beyond the current position. In case of malformed data (record too
 *   short), it has to be ensured that this either does not have bad influence
 *   or will be captured deliberately.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"

#ifdef SCI_NAMESPACE
using namespace Scintilla;
#endif

// prototypes for general helper functions
static inline bool IsNewline(const int ch);
static int GetHexaChar(char hd1, char hd2);
static int GetHexaChar(unsigned int pos, Accessor &styler);
static bool ForwardWithinLine(StyleContext &sc, int nb = 1);
static bool PosInSameRecord(unsigned int pos1, unsigned int pos2, Accessor &styler);
static int CountByteCount(unsigned int startPos, int uncountedDigits, Accessor &styler);
static int CalcChecksum(unsigned int startPos, int cnt, bool twosCompl, Accessor &styler);

// prototypes for file format specific helper functions
static unsigned int GetSrecRecStartPosition(unsigned int pos, Accessor &styler);
static int GetSrecByteCount(unsigned int recStartPos, Accessor &styler);
static int CountSrecByteCount(unsigned int recStartPos, Accessor &styler);
static int GetSrecAddressFieldSize(unsigned int recStartPos, Accessor &styler);
static int GetSrecAddressFieldType(unsigned int recStartPos, Accessor &styler);
static int GetSrecChecksum(unsigned int recStartPos, Accessor &styler);
static int CalcSrecChecksum(unsigned int recStartPos, Accessor &styler);

static unsigned int GetIHexRecStartPosition(unsigned int pos, Accessor &styler);
static int GetIHexByteCount(unsigned int recStartPos, Accessor &styler);
static int CountIHexByteCount(unsigned int recStartPos, Accessor &styler);
static int GetIHexAddressFieldType(unsigned int recStartPos, Accessor &styler);
static int GetIHexDataFieldType(unsigned int recStartPos, Accessor &styler);
static int GetIHexRequiredDataFieldSize(unsigned int recStartPos, Accessor &styler);
static int GetIHexChecksum(unsigned int recStartPos, Accessor &styler);
static int CalcIHexChecksum(unsigned int recStartPos, Accessor &styler);

static inline bool IsNewline(const int ch)
{
    return (ch == '\n' || ch == '\r');
}

static int GetHexaChar(char hd1, char hd2)
{
	int hexValue = 0;

	if (hd1 >= '0' && hd1 <= '9') {
		hexValue += 16 * (hd1 - '0');
	} else if (hd1 >= 'A' && hd1 <= 'F') {
		hexValue += 16 * (hd1 - 'A' + 10);
	} else if (hd1 >= 'a' && hd1 <= 'f') {
		hexValue += 16 * (hd1 - 'a' + 10);
	} else {
		return -1;
	}

	if (hd2 >= '0' && hd2 <= '9') {
		hexValue += hd2 - '0';
	} else if (hd2 >= 'A' && hd2 <= 'F') {
		hexValue += hd2 - 'A' + 10;
	} else if (hd2 >= 'a' && hd2 <= 'f') {
		hexValue += hd2 - 'a' + 10;
	} else {
		return -1;
	}

	return hexValue;
}

static int GetHexaChar(unsigned int pos, Accessor &styler)
{
	char highNibble, lowNibble;

	highNibble = styler.SafeGetCharAt(pos);
	lowNibble = styler.SafeGetCharAt(pos + 1);

	return GetHexaChar(highNibble, lowNibble);
}

// Forward <nb> characters, but abort (and return false) if hitting the line
// end. Return true if forwarding within the line was possible.
// Avoids influence on highlighting of the subsequent line if the current line
// is malformed (too short).
static bool ForwardWithinLine(StyleContext &sc, int nb)
{
	for (int i = 0; i < nb; i++) {
		if (sc.atLineEnd) {
			// line is too short
			sc.SetState(SCE_HEX_DEFAULT);
			sc.Forward();
			return false;
		} else {
			sc.Forward();
		}
	}

	return true;
}

// Checks whether the given positions are in the same record.
static bool PosInSameRecord(unsigned int pos1, unsigned int pos2, Accessor &styler)
{
	return styler.GetLine(pos1) == styler.GetLine(pos2);
}

// Count the number of digit pairs from <startPos> till end of record, ignoring
// <uncountedDigits> digits.
// If the record is too short, a negative count may be returned.
static int CountByteCount(unsigned int startPos, int uncountedDigits, Accessor &styler)
{
	int cnt;
	unsigned int pos;

	pos = startPos;

	while (!IsNewline(styler.SafeGetCharAt(pos, '\n'))) {
		pos++;
	}

	// number of digits in this line minus number of digits of uncounted fields
	cnt = static_cast<int>(pos - startPos) - uncountedDigits;

	// Prepare round up if odd (digit pair incomplete), this way the byte
	// count is considered to be valid if the checksum is incomplete.
	if (cnt >= 0) {
		cnt++;
	}

	// digit pairs
	cnt /= 2;

	return cnt;
}

// Calculate the checksum of the record.
// <startPos> is the position of the first character of the starting digit
// pair, <cnt> is the number of digit pairs.
static int CalcChecksum(unsigned int startPos, int cnt, bool twosCompl, Accessor &styler)
{
	int cs = 0;

	for (unsigned int pos = startPos; pos < startPos + cnt; pos += 2) {
		int val = GetHexaChar(pos, styler);

		if (val < 0) {
			return val;
		}

		// overflow does not matter
		cs += val;
	}

	if (twosCompl) {
		// low byte of two's complement
		return -cs & 0xFF;
	} else {
		// low byte of one's complement
		return ~cs & 0xFF;
	}
}

// Get the position of the record "start" field (first character in line) in
// the record around position <pos>.
static unsigned int GetSrecRecStartPosition(unsigned int pos, Accessor &styler)
{
	while (styler.SafeGetCharAt(pos) != 'S') {
		pos--;
	}

	return pos;
}

// Get the value of the "byte count" field, it counts the number of bytes in
// the subsequent fields ("address", "data" and "checksum" fields).
static int GetSrecByteCount(unsigned int recStartPos, Accessor &styler)
{
	int val;

	val = GetHexaChar(recStartPos + 2, styler);
	if (val < 0) {
	       val = 0;
	}

	return val;
}

// Count the number of digit pairs for the "address", "data" and "checksum"
// fields in this record. Has to be equal to the "byte count" field value.
// If the record is too short, a negative count may be returned.
static int CountSrecByteCount(unsigned int recStartPos, Accessor &styler)
{
	return CountByteCount(recStartPos, 4, styler);
}

// Get the size of the "address" field.
static int GetSrecAddressFieldSize(unsigned int recStartPos, Accessor &styler)
{
	switch (styler.SafeGetCharAt(recStartPos + 1)) {
		case '0':
		case '1':
		case '5':
		case '9':
			return 2; // 16 bit

		case '2':
		case '6':
		case '8':
			return 3; // 24 bit

		case '3':
		case '7':
			return 4; // 32 bit

		default:
			return 0;
	}
}

// Get the type of the "address" field content.
static int GetSrecAddressFieldType(unsigned int recStartPos, Accessor &styler)
{
	switch (styler.SafeGetCharAt(recStartPos + 1)) {
		case '0':
			return SCE_HEX_NOADDRESS;

		case '1':
		case '2':
		case '3':
			return SCE_HEX_DATAADDRESS;

		case '5':
		case '6':
			return SCE_HEX_RECCOUNT;

		case '7':
		case '8':
		case '9':
			return SCE_HEX_STARTADDRESS;

		default: // handle possible format extension in the future
			return SCE_HEX_ADDRESSFIELD_UNKNOWN;
	}
}

// Get the value of the "checksum" field.
static int GetSrecChecksum(unsigned int recStartPos, Accessor &styler)
{
	int byteCount;

	byteCount = GetSrecByteCount(recStartPos, styler);

	return GetHexaChar(recStartPos + 2 + byteCount * 2, styler);
}

// Calculate the checksum of the record.
static int CalcSrecChecksum(unsigned int recStartPos, Accessor &styler)
{
	int byteCount;

	byteCount = GetSrecByteCount(recStartPos, styler);

	// sum over "byte count", "address" and "data" fields (6..510 digits)
	return CalcChecksum(recStartPos + 2, byteCount * 2, false, styler);
}

// Get the position of the record "start" field (first character in line) in
// the record around position <pos>.
static unsigned int GetIHexRecStartPosition(unsigned int pos, Accessor &styler)
{
	while (styler.SafeGetCharAt(pos) != ':') {
		pos--;
	}

	return pos;
}

// Get the value of the "byte count" field, it counts the number of bytes in
// the "data" field.
static int GetIHexByteCount(unsigned int recStartPos, Accessor &styler)
{
	int val;

	val = GetHexaChar(recStartPos + 1, styler);
	if (val < 0) {
	       val = 0;
	}

	return val;
}

// Count the number of digit pairs for the "data" field in this record. Has to
// be equal to the "byte count" field value.
// If the record is too short, a negative count may be returned.
static int CountIHexByteCount(unsigned int recStartPos, Accessor &styler)
{
	return CountByteCount(recStartPos, 11, styler);
}

// Get the type of the "address" field content.
static int GetIHexAddressFieldType(unsigned int recStartPos, Accessor &styler)
{
	if (!PosInSameRecord(recStartPos, recStartPos + 7, styler)) {
		// malformed (record too short)
		// type cannot be determined
		return SCE_HEX_ADDRESSFIELD_UNKNOWN;
	}

	switch (GetHexaChar(recStartPos + 7, styler)) {
		case 0x00:
			return SCE_HEX_DATAADDRESS;

		case 0x01:
		case 0x02:
		case 0x03:
		case 0x04:
		case 0x05:
			return SCE_HEX_NOADDRESS;

		default: // handle possible format extension in the future
			return SCE_HEX_ADDRESSFIELD_UNKNOWN;
	}
}

// Get the type of the "data" field content.
static int GetIHexDataFieldType(unsigned int recStartPos, Accessor &styler)
{
	switch (GetHexaChar(recStartPos + 7, styler)) {
		case 0x00:
			return SCE_HEX_DATA_ODD;

		case 0x01:
			return SCE_HEX_DATA_EMPTY;

		case 0x02:
		case 0x04:
			return SCE_HEX_EXTENDEDADDRESS;

		case 0x03:
		case 0x05:
			return SCE_HEX_STARTADDRESS;

		default: // handle possible format extension in the future
			return SCE_HEX_DATA_UNKNOWN;
	}
}

// Get the required size of the "data" field. Useless for an ordinary data
// record (type 00), return the "byte count" in this case.
static int GetIHexRequiredDataFieldSize(unsigned int recStartPos, Accessor &styler)
{
	switch (GetHexaChar(recStartPos + 7, styler)) {
		case 0x01:
			return 0;

		case 0x02:
		case 0x04:
			return 2;

		case 0x03:
		case 0x05:
			return 4;

		default:
			return GetIHexByteCount(recStartPos, styler);
	}
}

// Get the value of the "checksum" field.
static int GetIHexChecksum(unsigned int recStartPos, Accessor &styler)
{
	int byteCount;

	byteCount = GetIHexByteCount(recStartPos, styler);

	return GetHexaChar(recStartPos + 9 + byteCount * 2, styler);
}

// Calculate the checksum of the record.
static int CalcIHexChecksum(unsigned int recStartPos, Accessor &styler)
{
	int byteCount;

	byteCount = GetIHexByteCount(recStartPos, styler);

	// sum over "byte count", "address", "type" and "data" fields (8..518 digits)
	return CalcChecksum(recStartPos + 1, 8 + byteCount * 2, true, styler);
}

static void ColouriseSrecDoc(unsigned int startPos, int length, int initStyle, WordList *[], Accessor &styler)
{
	StyleContext sc(startPos, length, initStyle, styler);

	while (sc.More()) {
		unsigned int recStartPos;
		int byteCount, addrFieldSize, addrFieldType, dataFieldSize;
		int cs1, cs2;

		switch (sc.state) {
			case SCE_HEX_DEFAULT:
				if (sc.atLineStart && sc.Match('S')) {
					sc.SetState(SCE_HEX_RECSTART);
				}
				ForwardWithinLine(sc);
				break;

			case SCE_HEX_RECSTART:
				sc.SetState(SCE_HEX_RECTYPE);
				ForwardWithinLine(sc);
				break;

			case SCE_HEX_RECTYPE:
				recStartPos = sc.currentPos - 2;
				byteCount = GetSrecByteCount(recStartPos, styler);

				if (byteCount == CountSrecByteCount(recStartPos, styler)) {
					sc.SetState(SCE_HEX_BYTECOUNT);
				} else {
					sc.SetState(SCE_HEX_BYTECOUNT_WRONG);
				}

				ForwardWithinLine(sc, 2);
				break;

			case SCE_HEX_BYTECOUNT:
			case SCE_HEX_BYTECOUNT_WRONG:
				recStartPos = sc.currentPos - 4;
				addrFieldSize = GetSrecAddressFieldSize(recStartPos, styler);
				addrFieldType = GetSrecAddressFieldType(recStartPos, styler);

				sc.SetState(addrFieldType);
				ForwardWithinLine(sc, addrFieldSize * 2);
				break;

			case SCE_HEX_NOADDRESS:
			case SCE_HEX_DATAADDRESS:
			case SCE_HEX_RECCOUNT:
			case SCE_HEX_STARTADDRESS:
			case SCE_HEX_ADDRESSFIELD_UNKNOWN:
				recStartPos = GetSrecRecStartPosition(sc.currentPos, styler);
				byteCount = GetSrecByteCount(recStartPos, styler);
				addrFieldSize = GetSrecAddressFieldSize(recStartPos, styler);
				dataFieldSize = byteCount - addrFieldSize - 1; // -1 for checksum field

				if (sc.state == SCE_HEX_ADDRESSFIELD_UNKNOWN) {
					sc.SetState(SCE_HEX_DATA_UNKNOWN);
					ForwardWithinLine(sc, dataFieldSize * 2);
					break;
				}

				sc.SetState(SCE_HEX_DATA_ODD);

				for (int i = 0; i < dataFieldSize * 2; i++) {
					if ((i & 0x3) == 0) {
						sc.SetState(SCE_HEX_DATA_ODD);
					} else if ((i & 0x3) == 2) {
						sc.SetState(SCE_HEX_DATA_EVEN);
					}

					if (!ForwardWithinLine(sc)) {
						break;
					}
				}
				break;

			case SCE_HEX_DATA_ODD:
			case SCE_HEX_DATA_EVEN:
			case SCE_HEX_DATA_UNKNOWN:
				recStartPos = GetSrecRecStartPosition(sc.currentPos, styler);
				cs1 = CalcSrecChecksum(recStartPos, styler);
				cs2 = GetSrecChecksum(recStartPos, styler);

				if (cs1 != cs2 || cs1 < 0 || cs2 < 0) {
					sc.SetState(SCE_HEX_CHECKSUM_WRONG);
				} else {
					sc.SetState(SCE_HEX_CHECKSUM);
				}

				ForwardWithinLine(sc, 2);
				break;

			case SCE_HEX_CHECKSUM:
			case SCE_HEX_CHECKSUM_WRONG:
				// record finished
				sc.SetState(SCE_HEX_DEFAULT);
				ForwardWithinLine(sc);
				break;
		}
	}
	sc.Complete();
}

static void ColouriseIHexDoc(unsigned int startPos, int length, int initStyle, WordList *[], Accessor &styler)
{
	StyleContext sc(startPos, length, initStyle, styler);

	while (sc.More()) {
		unsigned int recStartPos;
		int byteCount, addrFieldType, dataFieldSize, dataFieldType;
		int cs1, cs2;

		switch (sc.state) {
			case SCE_HEX_DEFAULT:
				if (sc.atLineStart && sc.Match(':')) {
					sc.SetState(SCE_HEX_RECSTART);
				}
				ForwardWithinLine(sc);
				break;

			case SCE_HEX_RECSTART:
				recStartPos = sc.currentPos - 1;
				byteCount = GetIHexByteCount(recStartPos, styler);
				dataFieldSize = GetIHexRequiredDataFieldSize(recStartPos, styler);

				if (byteCount == CountIHexByteCount(recStartPos, styler)
						&& byteCount == dataFieldSize) {
					sc.SetState(SCE_HEX_BYTECOUNT);
				} else {
					sc.SetState(SCE_HEX_BYTECOUNT_WRONG);
				}

				ForwardWithinLine(sc, 2);
				break;

			case SCE_HEX_BYTECOUNT:
			case SCE_HEX_BYTECOUNT_WRONG:
				recStartPos = sc.currentPos - 3;
				addrFieldType = GetIHexAddressFieldType(recStartPos, styler);

				sc.SetState(addrFieldType);
				ForwardWithinLine(sc, 4);
				break;

			case SCE_HEX_NOADDRESS:
			case SCE_HEX_DATAADDRESS:
			case SCE_HEX_ADDRESSFIELD_UNKNOWN:
				sc.SetState(SCE_HEX_RECTYPE);
				ForwardWithinLine(sc, 2);
				break;

			case SCE_HEX_RECTYPE:
				recStartPos = sc.currentPos - 9;
				dataFieldType = GetIHexDataFieldType(recStartPos, styler);

				sc.SetState(dataFieldType);

				if (dataFieldType == SCE_HEX_DATA_ODD) {
					dataFieldSize = GetIHexByteCount(recStartPos, styler);

					for (int i = 0; i < dataFieldSize * 2; i++) {
						if ((i & 0x3) == 0) {
							sc.SetState(SCE_HEX_DATA_ODD);
						} else if ((i & 0x3) == 2) {
							sc.SetState(SCE_HEX_DATA_EVEN);
						}

						if (!ForwardWithinLine(sc)) {
							break;
						}
					}
				} else if (dataFieldType == SCE_HEX_DATA_UNKNOWN) {
					dataFieldSize = GetIHexByteCount(recStartPos, styler);
					ForwardWithinLine(sc, dataFieldSize * 2);
				} else {
					// Using the required size here has the effect that the checksum is
					// highlighted at a fixed position after this field, independent on
					// the "byte count" value.
					dataFieldSize = GetIHexRequiredDataFieldSize(recStartPos, styler);
					ForwardWithinLine(sc, dataFieldSize * 2);
				}
				break;

			case SCE_HEX_DATA_ODD:
			case SCE_HEX_DATA_EVEN:
			case SCE_HEX_DATA_EMPTY:
			case SCE_HEX_EXTENDEDADDRESS:
			case SCE_HEX_STARTADDRESS:
			case SCE_HEX_DATA_UNKNOWN:
				recStartPos = GetIHexRecStartPosition(sc.currentPos, styler);
				cs1 = CalcIHexChecksum(recStartPos, styler);
				cs2 = GetIHexChecksum(recStartPos, styler);

				if (cs1 != cs2 || cs1 < 0 || cs2 < 0) {
					sc.SetState(SCE_HEX_CHECKSUM_WRONG);
				} else {
					sc.SetState(SCE_HEX_CHECKSUM);
				}

				ForwardWithinLine(sc, 2);
				break;

			case SCE_HEX_CHECKSUM:
			case SCE_HEX_CHECKSUM_WRONG:
				// record finished
				sc.SetState(SCE_HEX_DEFAULT);
				ForwardWithinLine(sc);
				break;
		}
	}
	sc.Complete();
}

LexerModule lmSrec(SCLEX_SREC, ColouriseSrecDoc, "srec", 0, NULL);
LexerModule lmIHex(SCLEX_IHEX, ColouriseIHexDoc, "ihex", 0, NULL);
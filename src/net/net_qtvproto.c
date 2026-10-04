// net_qtvproto.c -- QTV's request, as the server's TCP port takes it from a
// viewer: what net_qtv.c reads, without sockets
//
// A request is lines to an empty one, LF or CRLF: "QTV", then "KEY: value"
// lines. VERSION is the protocol's (1, or 1.x), RAW asks for the stream
// without the reply's lines, and USERINFO is the viewer's, for its name; the
// others (SOURCE, AUTH, QTV_EZQUAKE_EXT...) are a relay's, and passed over.

#include "net_qtv.h"

#include <stdlib.h>
#include <string.h>

static bool QTV_IsKey (const char *line, size_t length, const char *key)
{
	return strlen (key) == length && !memcmp (line, key, length);
}

// the name in an info string (\key\value..., quoted or not), without what
// wouldn't print
static void QTV_InfoName (const char *info, size_t length, char *name, size_t size)
{
	const char	*end = info + length, *key, *value;
	size_t		n;

	*name = 0;
	if (info < end && *info == '"')
	{
		info++;
		if (info < end && end[-1] == '"')
			end--;
	}
	while (info < end && *info == '\\')
	{
		key = ++info;
		while (info < end && *info != '\\')
			info++;
		if (info == end)
			return;
		value = ++info;
		while (info < end && *info != '\\')
			info++;
		if (value - key - 1 != 4 || memcmp (key, "name", 4))
			continue;
		for (n = 0 ; value < info && n < size - 1 ; value++)
			if ((unsigned char)*value >= 32)
				name[n++] = *value;
		name[n] = 0;
		return;
	}
}

int QTV_ParseRequest (const char *text, int length, qtvrequest_t *request)
{
	const char	*end = NULL, *line, *next, *colon, *value;
	size_t		linelength, keylength;
	int			i;

	// the lines end at an empty one
	for (i = 1 ; i < length ; i++)
		if (text[i] == '\n' && (text[i-1] == '\n' || (i >= 2 && text[i-1] == '\r' && text[i-2] == '\n')))
		{
			end = text + i + 1;
			break;
		}
	if (!end)
		return length >= QTV_REQUESTMAX ? -1 : 0;
	if (end - text > QTV_REQUESTMAX)
		return -1;

	memset (request, 0, sizeof(*request));
	for (line = text ; line < end ; line = next + 1)
	{
		next = memchr (line, '\n', (size_t)(end - line));
		linelength = (size_t)(next - line);
		if (linelength && line[linelength - 1] == '\r')
			linelength--;
		if (line == text)
		{	// QTV's, or not a request of ours
			if (!QTV_IsKey (line, linelength, "QTV"))
				return -1;
			continue;
		}
		colon = memchr (line, ':', linelength);
		if (!colon)
			continue;
		keylength = (size_t)(colon - line);
		for (value = colon + 1 ; value < line + linelength && *value == ' ' ; value++)
			;
		if (QTV_IsKey (line, keylength, "VERSION"))
			request->version = atoi (value);		// 1.x is 1
		else if (QTV_IsKey (line, keylength, "RAW"))
			request->raw = atoi (value) != 0;
		else if (QTV_IsKey (line, keylength, "USERINFO"))
			QTV_InfoName (value, (size_t)(line + linelength - value), request->name, sizeof(request->name));
	}
	return (int)(end - text);
}

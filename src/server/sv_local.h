/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#pragma once
// sv_local.h -- everything the server module's own files use

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include "args.h"
#include "arena.h"
#include "bspfile.h"
#include "cmd.h"
#include "crc.h"
#include "cvar.h"
#include "fs.h"
#include "host.h"
#include "info.h"
#include "link.h"
#include "mathlib.h"
#include "md4.h"
#include "mem.h"
#include "msg.h"
#include "print.h"
#include "protocol.h"
#include "q_endian.h"
#include "q_string.h"
#include "q_types.h"
#include "sys.h"
#include "version.h"
#include "vispatch.h"
#include "vmarray.h"
#include "net.h"
#include "cmodel.h"
#include "pmove.h"
#include "progs.h"
#include "server.h"
#include "world.h"

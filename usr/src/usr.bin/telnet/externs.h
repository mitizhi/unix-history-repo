/*
 * Copyright (c) 1988 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of California at Berkeley. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 *
 *	@(#)externs.h	1.7 (Berkeley) %G%
 */

#include <stdio.h>
#include <setjmp.h>

#define	SUBBUFSIZE	100

extern int errno;		/* outside this world */

extern int
	flushout,		/* flush output */
	connected,		/* Are we connected to the other side? */
	globalmode,		/* Mode tty should be in */
	In3270,			/* Are we in 3270 mode? */
	telnetport,		/* Are we connected to the telnet port? */
	localchars,		/* we recognize interrupt/quit */
	donelclchars,		/* the user has set "localchars" */
	showoptions,
	net,
	crlf,		/* Should '\r' be mapped to <CR><LF> (or <CR><NUL>)? */
	autoflush,		/* flush output when interrupting? */
	autosynch,		/* send interrupt characters with SYNCH? */
	SYNCHing,		/* Is the stream in telnet SYNCH mode? */
	donebinarytoggle,	/* the user has put us in binary */
	dontlecho,		/* do we suppress local echoing right now? */
	crmod,
	netdata,		/* Print out network data flow */
	debug;			/* Debug level */

extern char
	echoc,			/* Toggle local echoing */
	escape,			/* Escape to command mode */
	doopt[],
	dont[],
	will[],
	wont[],
	hisopts[],
	myopts[],
	*hostname,		/* Who are we connected to? */
	*prompt;		/* Prompt for command. */

extern FILE
	*NetTrace;		/* Where debugging output goes */

extern jmp_buf
	peerdied,
	toplevel;		/* For error conditions. */

extern void
	dosynch(),
	setconnmode(),
	setcommandmode();

extern char
    termEofChar,
    termEraseChar,
    termFlushChar,
    termIntChar,
    termKillChar,
    termLiteralNextChar,
    termQuitChar;

/* Ring buffer structures which are shared */

extern Ring
	netoring,
	netiring,
	ttyoring,
	ttyiring;

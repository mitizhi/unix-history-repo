/*	dver.c	1.9	83/10/22
 *
 * Versatec driver for the new troff
 *
 * Authors:	BWK(BELL)
 *		VCAT(berkley)
 *		Richard L. Hyde, Perdue University
 *		and David Slattengren, U.C. Berkeley
 */

 
/*******************************************************************************

    output language from troff:
    all numbers are character strings

#..\n	comment
sn	size in points
fn	font as number from 1 to n
cx	ascii character x
Cxyz	funny char \(xyz. terminated by white space
Hn	go to absolute horizontal position n
Vn	go to absolute vertical position n (down is positive)
hn	go n units horizontally (relative)
vn	ditto vertically
nnc	move right nn, then print c (exactly 2 digits!)
		(this wart is an optimization that shrinks output file size
		 about 35% and run-time about 15% while preserving ascii-ness)
pn	new page begins (number n) -- set v to 0
P	spread ends -- output it.
nb a	end of line (information only -- no action needed)
	b = space before line, a = after
w	paddable word space -- no action needed

Dt ..\n	draw operation 't':
	Dl x y		line from here by x,y
	Dc d		circle of diameter d with left side here
	De x y		ellipse of axes x,y with left side here
	Da x y r	arc counter-clockwise by x,y of radius r
	D~ x y x y ...	B-spline curve by x,y then x,y ...

x ..\n	device control functions:
     x i	init
     x T s	name of device is s
     x r n h v	resolution is n/inch h = min horizontal motion, v = min vert
     x p	pause (can restart)
     x s	stop -- done for ever
     x t	generate trailer
     x f n s	font position n contains font s
     x H n	set character height to n
     x S n	set slant to N

	Subcommands like "i" are often spelled out like "init".

*******************************************************************************/


#include <stdio.h>
#include <ctype.h>
#include <sys/vcmd.h>
#include "dev.h"


/* #define DEBUGABLE		/* No, not debugable... */
#define DRIVER			/* Yes, we're driving directly */
#define	NFONTS	60		/* total number of fonts useable */
#define	MAXSTATE 6		/* number of environments rememberable */
#define OPENREAD 0		/* mode for openning files */
#define RESTART	1		/* upon exit, return either RESTART */
#define ABORT	2		/*     or ABORT */
#define	FATAL	1		/* type of error */
#define	BMASK	0377		/* byte grabber */
#define FONTDIR	"/usr/lib/font"	/* default place to find font descriptions */
#define BITDIR "/usr/lib/vfont" /* default place to look for font rasters */
#define MAXWRIT 4096		/* max characters allowed to write at once */

#define  hmot(n)	hpos += n
#define  hgoto(n)	hpos = n
#define  vmot(n)	vgoto(vpos + (n))


char	SccsId[]= "dver.c	1.9	83/10/22";

int	output	= 0;	/* do we do output at all? */
int	nolist	= 0;	/* output page list if > 0 */
int	olist[20];	/* pairs of page numbers */
int	spage	= 9999;	/* stop every spage pages */
int	scount	= 0;
struct	dev	dev;
struct	font	*fontbase[NFONTS+1];
short *	pstab;		/* point size table pointer */
int	nsizes;		/* number of sizes device is capable of printing */
int	nfonts;		/* number of fonts device is capable of printing */
int	nchtab;
char *	chname;
short *	chtab;
char *	fitab[NFONTS+1];	/* font inclusion table - maps ascii to ch # */
char *	widtab[NFONTS+1];	/* width table for each font */
char *	codetab[NFONTS+1];	/* device codes */
char *	fontdir = FONTDIR;	/* place to find devxxx directories */
char *	bitdir = BITDIR;	/* place to find raster fonts and fontmap */
char *	fontname[NFONTS+1];	/* list of what font is on what position */
struct {			/* table of what font */
	char fname[3];		/*   name maps to what */
	char *ffile;		/*   filename in bitdirectory */
} fontmap[NFONTS+1];

#ifdef DEBUGABLE
int	dbg	= 0;
#endif
int	size	= 1;	/* current point size (internal pstable index) */
int	font	= 1;	/* current font - assume reasonable starting font */
int	hpos;		/* horizontal position we are to be at next; left = 0 */
int	vpos;		/* current vertical position (down positive) */
int	maxv;		/* farthest down the page we've been */
extern	linethickness;	/* thickness (in pixels) of any drawn object */
extern	linmod;		/* line style (a bit mask - dotted, etc.) of objects */
int	lastw;		/* width of last character printed */


#define DISPATCHSIZE	256		/* must be a power of two */
#define CHARMASK	(DISPATCHSIZE-1)
#define DSIZ		((sizeof *dispatch)*DISPATCHSIZE)
#define OUTFILE 	fileno (stdout)

#define	RES		200		/* resolution of the device (dots/in) */
#define RASTER_LENGTH	7040		/* device line length */
#define BYTES_PER_LINE	(RASTER_LENGTH/8)
#define BAND		2			/* length of a band in inches */
#define NLINES		(int)(BAND * RES)	/* BAND" long bands */
#define BUFFER_SIZE	(NLINES*BYTES_PER_LINE)	/* number of chars in picture */

#define BUFTOP		(&buffer[0])
#define BUFBOTTOM	(&buffer[BUFFER_SIZE] - 1)
#define buf0p		BUFTOP			/* vorigin in circular buffer */
#define PAGEEND		1			/* flags to "outband" to tell */
#define OVERBAND	0			/* whether to fill out a page */


int	pltmode[] = { VPLOT };
int	prtmode[] = { VPRINT };
char	buffer[BUFFER_SIZE];	/* versatec-wide NLINES buffer */
int	vorigin = 0;		/* where on the page startbuf maps to */
int	pagelen = 0;		/* how long the current "page" has printed */

char *	calloc();
char *	nalloc();
char *	allpanic();
char *	operand();

struct header {
	short	magic;
	unsigned short	size;
	short	maxx;
	short	maxy;
	short	xtnd;
} header;

struct	dispatch{
	unsigned short	addr;
	short	nbytes;
	char	up;
	char	down;
	char	left;
	char	right;
	short	width;
};

struct	fontdes {
	int	fnum;			/* if == -1, this position unused */
	int	psize;
	struct	dispatch *disp;
	char	*bits;
} fontdes[NFONTS+1];			/* is initialized at start of main */

struct dispatch *dispatch;
int	cfnum = -1;
int	cpsize = 10;
int	cfont = 1;
char	*bits;
int	fontwanted = 1;		/* flag:  "has a new font been requested?" */
int	nfontnum = -1;
int	npsize = 10;

 

main(argc, argv)
char *argv[];
{
	register FILE *fp;
	register int i;

	for (i = 0; i <= NFONTS; fontdes[i++].fnum = -1);
	while (--argc > 0 && **++argv == '-') {
		switch ((*argv)[1]) {
		case 'F':
			bitdir = operand(&argc, &argv);
			break;
		case 'f':
			fontdir = operand(&argc, &argv);
			break;
		case 'o':
			outlist(operand(&argc, &argv));
			break;
#ifdef DEBUGABLE
		case 'd':
			dbg = atoi(operand(&argc, &argv));
			if (dbg == 0) dbg = 1;
			break;
#endif
		case 's':
			spage = atoi(operand(&argc, &argv));
			if (spage <= 0)
				spage = 9999;
			break;
		}
	}
#ifdef DRIVER
	ioctl(OUTFILE, VSETSTATE, pltmode);
#endif

	if (argc < 1)
		conv(stdin);
	else
		while (argc-- > 0) {
			if (strcmp(*argv, "-") == 0)
				fp = stdin;
			else if ((fp = fopen(*argv, "r")) == NULL)
				error(FATAL, "can't open %s", *argv);
			conv(fp);
			fclose(fp);
			argv++;
		}
	exit(0);
}


/*----------------------------------------------------------------------------*
 | Routine:	char  * operand (& argc,  & argv)
 |
 | Results:	returns address of the operand given with a command-line
 |		option.  It uses either "-Xoperand" or "-X operand", whichever
 |		is present.  The program is terminated if no option is present.
 |
 | Side Efct:	argc and argv are updated as necessary.
 *----------------------------------------------------------------------------*/

char *operand(argcp, argvp)
int * argcp;
char ***argvp;
{
	if ((**argvp)[2]) return(**argvp + 2); /* operand immediately follows */
	if ((--*argcp) <= 0) {			/* no operand */
	    error (FATAL, "command-line option operand missing.");
	}
	return(*(++(*argvp)));			/* operand next word */
}


outlist(s)	/* process list of page numbers to be printed */
char *s;
{
	int n1, n2;
#ifdef DEBUGABLE
	int i;
#endif

	nolist = 0;
	while (*s) {
		n1 = 0;
		if (isdigit(*s))
			do
				n1 = 10 * n1 + *s++ - '0';
			while (isdigit(*s));
		else
			n1 = -9999;
		n2 = n1;
		if (*s == '-') {
			s++;
			n2 = 0;
			if (isdigit(*s))
				do
					n2 = 10 * n2 + *s++ - '0';
				while (isdigit(*s));
			else
				n2 = 9999;
		}
		olist[nolist++] = n1;
		olist[nolist++] = n2;
		if (*s != '\0')
			s++;
	}
	olist[nolist] = 0;
#ifdef DEBUGABLE
	if (dbg)
		for (i=0; i<nolist; i += 2)
			fprintf(stderr,"%3d %3d\n", olist[i], olist[i+1]);
#endif
}

conv(fp)
register FILE *fp;
{
	register int c, k;
	int m, n, n1, m1;
	char str[100], buf[300];

	while ((c = getc(fp)) != EOF) {
		switch (c) {
		case '\n':	/* when input is text */
		case ' ':
		case 0:		/* occasional noise creeps in */
			break;
		case '{':	/* push down current environment */
			t_push();
			break;
		case '}':
			t_pop();
			break;
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			/* two motion digits plus a character */
			hmot((c-'0')*10 + getc(fp)-'0');
			put1(getc(fp));
			break;
		case 'c':	/* single ascii character */
			put1(getc(fp));
			break;
		case 'C':
			fscanf(fp, "%s", str);
			put1s(str);
			break;
		case 't':	/* straight text */
			(void) fgets(buf, sizeof(buf), fp);
			t_text(buf);
			break;
		case 'D':	/* draw function */
			(void) fgets(buf, sizeof(buf), fp);
			switch (buf[0]) {
			case 'l':	/* draw a line */
			    sscanf(buf+1, "%d %d", &n, &m);
			    drawline(n, m);
			    break;
			case 'c':	/* circle */
			    sscanf(buf+1, "%d", &n);
			    drawcirc(n);
			    break;
			case 'e':	/* ellipse */
			    sscanf(buf+1, "%d %d", &m, &n);
			    drawellip(m, n);
			    break;
			case 'a':	/* arc */
			    sscanf(buf+1, "%d %d %d %d", &n, &m, &n1, &m1);
			    drawarc(n, m, n1, m1);
			    break;
			case '~':	/* wiggly line */
			case 'g':	/* gremlin spline */
			    drawwig(buf+1, fp, buf[0] == '~');
			    break;
			case 't':	/* line thickness */
			    sscanf(buf+1, "%d", &n);
			    drawthick(n);
			    break;
			case 's':	/* line style */
			    sscanf(buf+1, "%d", &n);
			    drawstyle(n);
			    break;
			default:
			    error(FATAL, "unknown drawing function %s", buf);
			    break;
			}
			break;
		case 's':
			fscanf(fp, "%d", &n);	/* ignore fractional sizes */
			setsize(t_size(n));
			break;
		case 'f':
			fscanf(fp, "%s", str);
			setfont(t_font(str));
			break;
		case 'H':	/* absolute horizontal motion */
			/* fscanf(fp, "%d", &n); */
			while ((c = getc(fp)) == ' ')
				;
			k = 0;
			do {
				k = 10 * k + c - '0';
			} while (isdigit(c = getc(fp)));
			ungetc(c, fp);
			hgoto(k);
			break;
		case 'h':	/* relative horizontal motion */
			while ((c = getc(fp)) == ' ')
				;
			k = 0;
			do {
				k = 10 * k + c - '0';
			} while (isdigit(c = getc(fp)));
			ungetc(c, fp);
			hmot(k);
			break;
		case 'w':	/* word space */
			break;
		case 'V':
			fscanf(fp, "%d", &n);
			vgoto(n);
			break;
		case 'v':
			fscanf(fp, "%d", &n);
			vmot(n);
			break;
		case 'P':	/* new spread */
			if (output) outband(OVERBAND);
			break;
		case 'p':	/* new page */
			fscanf(fp, "%d", &n);
			t_page(n);
			break;
		case 'n':	/* end of line */
			while (getc(fp) != '\n')
				;
			t_newline();
			break;
		case '#':	/* comment */
			while (getc(fp) != '\n')
				;
			break;
		case 'x':	/* device control */
			devcntrl(fp);
			break;
		default:
			error(FATAL, "unknown input character %o %c", c, c);
		}
	}
}

devcntrl(fp)	/* interpret device control functions */
FILE *fp;
{
        char str[20], str1[50], buf[50];
	int c, n;

	fscanf(fp, "%s", str);
	switch (str[0]) {	/* crude for now */
	case 'i':	/* initialize */
		fileinit();
		t_init();
		break;
	case 't':	/* trailer */
		break;
	case 'p':	/* pause -- can restart */
		t_reset('p');
		break;
	case 's':	/* stop */
		t_reset('s');
		break;
	case 'r':	/* resolution assumed when prepared */
		fscanf(fp, "%d", &n);
		if (n!=RES) error(FATAL,"Input computed with wrong resolution");
		break;
	case 'f':	/* font used */
		fscanf(fp, "%d %s", &n, str);
		(void) fgets(buf, sizeof buf, fp);   /* in case of filename */
		ungetc('\n', fp);		/* fgets goes too far */
		str1[0] = 0;			/* in case nothing comes in */
		sscanf(buf, "%s", str1);
		loadfont(n, str, str1);
		break;
						/* these don't belong here... */
	case 'H':	/* char height */
		fscanf(fp, "%d", &n);
		t_charht(n);
		break;
	case 'S':	/* slant */
		fscanf(fp, "%d", &n);
		t_slant(n);
		break;
	}
	while ((c = getc(fp)) != '\n')	/* skip rest of input line */
		if (c == EOF)
			break;
}

/* fileinit:	read in font and code files, etc.
		Must open table for device, read in resolution,
		size info, font info, etc. and set params.
		Also read in font name mapping.
*/
fileinit()
{
	register int i;
	register int fin;
	register int nw;
	register char *filebase;
	register char *p;
	register FILE *fp;
	char	temp[100];


		/* first, read in font map file.  The file must be of Format:
			XX  FILENAME  (XX = troff font name)
			with one entry per text line of the file.
		   Extra stuff after FILENAME is ignored */

	sprintf(temp, "%s/fontmap", bitdir);
	if ((fp = fopen(temp, "r")) == NULL)
		error(FATAL, "Can't open %s", temp);
	for (i = 0; i <= NFONTS && fgets(temp, 100, fp) != NULL; i++) {
		sscanf(temp, "%2s", fontmap[i].fname);
		p = &temp[0];
		while (*p != ' ' && *p != '	') p++;
		while (*p == ' ' || *p == '	') p++;
		filebase = p;
		for (nw = 1; *p != '\n' && *p != ' ' && *p != '\t'; p++) nw++;
		fontmap[i].ffile = nalloc(1, nw);
		sscanf(filebase, "%s", fontmap[i].ffile);
	}
	fontmap[++i].fname[0] = '0';		/* finish off with zeros */
	fontmap[i].ffile = (char *) 0;
	fclose(fp);
#ifdef DEBUGABLE
	if(dbg) {
	    fprintf(stderr, "font map:\n");
	    for (i = 0; fontmap[i].ffile; i++)
		fprintf(stderr,"%s = %s\n", fontmap[i].fname, fontmap[i].ffile);
	}
#endif


	sprintf(temp, "%s/devvp/DESC.out", fontdir);
	if ((fin = open(temp, 0)) < 0)
		error(FATAL, "can't open tables for %s", temp);
	if (read(fin, &dev, sizeof(struct dev)) != sizeof(struct dev))
		error(FATAL, "can't read header for %s", temp);
	nfonts = dev.nfonts;
	nsizes = dev.nsizes;
	nchtab = dev.nchtab;
	filebase = calloc(1, dev.filesize);	/* enough room for whole file */
	if (read(fin, filebase, dev.filesize) != dev.filesize)	/* at once */
		error(FATAL, "can't read width table for %s", temp);
	pstab = (short *) filebase;
	chtab = pstab + nsizes + 1;
	chname = (char *) (chtab + dev.nchtab);
	p = chname + dev.lchname;
	for (i = 1; i <= nfonts; i++) {
		fontbase[i] = (struct font *) p;
		nw = *p & BMASK;		/* 1st thing is width count */
		p += sizeof(struct font);
		widtab[i] = p;
		codetab[i] = p + 2 * nw;
		fitab[i] = p + 3 * nw;
		p += 3 * nw + dev.nchtab + 128 - 32;
		t_fp(i, fontbase[i]->namefont, fontbase[i]->intname);
#ifdef DEBUGABLE
		if (dbg > 1) fontprint(i);
#endif
	}
	fontbase[0] = (struct font *)
		calloc(1,3*255 + dev.nchtab + (128-32) + sizeof (struct font));
	widtab[0] = (char *) fontbase[0] + sizeof (struct font);
	fontbase[0]->nwfont = 255;
	close(fin);
}

#ifdef DEBUGABLE
fontprint(i)	/* debugging print of font i (0,...) */
{
	int j, n;
	char *p;

	fprintf(stderr,"font %d:\n", i);
	p = (char *) fontbase[i];
	n = fontbase[i]->nwfont & BMASK;
	fprintf(stderr,
	    "base=0%o, nchars=%d, spec=%d, name=%s, widtab=0%o, fitab=0%o\n",p,
	    n,fontbase[i]->specfont,fontbase[i]->namefont,widtab[i],fitab[i]);
	fprintf(stderr,"widths:\n");
	for (j=0; j <= n; j++) {
		fprintf(stderr," %2d", widtab[i][j] & BMASK);
		if (j % 20 == 19) fprintf(stderr,"\n");
	}
	fprintf(stderr,"\ncodetab:\n");
	for (j=0; j <= n; j++) {
		fprintf(stderr," %2d", codetab[i][j] & BMASK);
		if (j % 20 == 19) fprintf(stderr,"\n");
	}
	fprintf(stderr,"\nfitab:\n");
	for (j=0; j <= dev.nchtab + 128-32; j++) {
		fprintf(stderr," %2d", fitab[i][j] & BMASK);
		if (j % 20 == 19) fprintf(stderr,"\n");
	}
	fprintf(stderr,"\n");
}
#endif


loadfont(n, s, s1)	/* load font info for font s on position n (0...) */
int n;
char *s, *s1;
{
	char temp[60];
	int fin, nw, norig;

	if (n < 0 || n > NFONTS)
		error(FATAL, "illegal fp command %d %s", n, s);
	if (strcmp(s, fontbase[n]->namefont) == 0)
		return;
	if (s1 == NULL || s1[0] == '\0')
		sprintf(temp, "%s/devvp/%s.out", fontdir, s);
	else
		sprintf(temp, "%s/%s.out", s1, s);
	if ((fin = open(temp, 0)) < 0)
		error(FATAL, "can't open font table %s", temp);
	norig = fontbase[n]->nwfont & BMASK;
	if (read(fin,fontbase[n],3*norig+nchtab+128-32+sizeof(struct font)) < 0)
		error(FATAL, "Can't read in font %s on position %d", s, n);
	if ((fontbase[n]->nwfont & BMASK) > norig)
		error(FATAL, "Font %s too big for position %d", s, n);
	close(fin);
	nw = fontbase[n]->nwfont & BMASK;
	widtab[n] = (char *) fontbase[n] + sizeof(struct font);
	codetab[n] = (char *) widtab[n] + 2 * nw;
	fitab[n] = (char *) widtab[n] + 3 * nw;
	t_fp(n, fontbase[n]->namefont, fontbase[n]->intname);
	fontbase[n]->nwfont = norig;	/* to later use full original size */
#ifdef DEBUGABLE
	if (dbg > 1) fontprint(n);
#endif
}


/*VARARGS1*/
error(f, s, a1, a2, a3, a4, a5, a6, a7) {
	fprintf(stderr, "dver: ");
	fprintf(stderr, s, a1, a2, a3, a4, a5, a6, a7);
	fprintf(stderr, "\n");
	if (f) exit(ABORT);
}


t_init()	/* initialize device */
{
	vorigin = pagelen = maxv = hpos = vpos = 0;

	output = 0;		/* don't output anything yet */
	setsize(t_size(10));	/* start somewhere */
	setfont(1);
}


struct state {
	int	ssize;
	int	sfont;
	int	shpos;
	int	svpos;
	int	sstyle;
	int	sthick;
};
struct	state	state[MAXSTATE];
struct	state	*statep = state;

t_push()	/* begin a new block */
{
	statep->ssize = size;
	statep->sfont = font;
	statep->sstyle = linmod;
	statep->sthick = linethickness;
	statep->shpos = hpos;
	statep->svpos = vpos;
	if (statep++ >= state+MAXSTATE)
		error(FATAL, "{ nested too deep");
}

t_pop()	/* pop to previous state */
{
	if (--statep < state)
		error(FATAL, "extra }");
	size = statep->ssize;
	font = statep->sfont;
	hpos = statep->shpos;
	vpos = statep->svpos;
	linmod = statep->sstyle;
	linethickness = statep->sthick;
}


t_page(n)	/* do whatever new page functions */
{
	int i;


	if (output) outband(PAGEEND);

	maxv = vpos = 0;

	output = 1;
	if (nolist == 0)
		return;		/* no -o specified */
	output = 0;
	for (i = 0; i < nolist; i += 2)
		if (n >= olist[i] && n <= olist[i+1]) {
			output = 1;
			break;
		}
}


outband(page)
int page;
{
    register int outsize;

    if (page == PAGEEND) {		/* set outsize to inch boundary */
	outsize = (maxv + (RES - 1) - pagelen) / RES;
	if (outsize < 1) return;	/* if outsize <= zero, forget it */

	outsize *= RES * BYTES_PER_LINE;	/* are assured that outsize */
	vwrite(buf0p, outsize);			/* will NOT be > BUFFER_SIZE */
	vclear(buf0p, outsize);			/* since vsort makes sure of */
	vorigin = pagelen = 0;			/* putting P commands in */
    } else {
	vorigin += NLINES;
	pagelen += NLINES;
	vwrite(buf0p, BUFFER_SIZE);
	vclear(buf0p, BUFFER_SIZE);
    }
}


t_newline()	/* do whatever for the end of a line */
{
	hpos = 0;	/* because we're now back at the left margin */
}

t_size(n)	/* convert integer to internal size number*/
int n;
{
	int i;

	if (n <= pstab[0])
		return(0);
	else if (n >= pstab[nsizes - 1])
		return(nsizes - 1);
	for (i = 0; n > pstab[i]; i++)
		;
	return(i);
}

/*ARGSUSED*/
t_charht(n)	/* set character height to n */
int n;
{
#ifdef DEBUGABLE
	if (dbg) error(!FATAL, "can't set height on versatec");
#endif
}

/*ARGSUSED*/
t_slant(n)	/* set slant to n */
int n;
{
#ifdef DEBUGABLE
	if (dbg) error(!FATAL, "can't set slant on versatec");
#endif
}

t_font(s)	/* convert string to internal font number */
char *s;
{
	int n;

	n = atoi(s);
	if (n < 0 || n > nfonts)
		n = 1;
	return(n);
}

t_text(s)	/* print string s as text */
char *s;
{
	int c;
	char str[100];

	if (!output)
		return;
	while (c = *s++) {
		if (c == '\\') {
			switch (c = *s++) {
			case '\\':
			case 'e':
				put1('\\');
				break;
			case '(':
				str[0] = *s++;
				str[1] = *s++;
				str[2] = '\0';
				put1s(str);
				break;
			}
		} else {
			put1(c);
		}
		hmot(lastw);
#ifdef DEBUGABLE
		if (dbg) fprintf(stderr,"width = %d\n", lastw);
#endif
	}
}


t_reset(c)
{	
	switch(c){
	case 's':
		t_page(0);
#ifdef DRIVER
		ioctl(OUTFILE, VSETSTATE, prtmode);
#endif
		break;
	}
}


vgoto (n)
int n;
{
	vpos = n;
	if (vpos > maxv) maxv = vpos;
}


put1s(s)	/* s is a funny char name */
char *s;
{
	int i;

	if (!output)
		return;
#ifdef DEBUGABLE
	if (dbg) fprintf(stderr,"%s ", s);
#endif
	for (i = 0; i < nchtab; i++)
		if (strcmp(&chname[chtab[i]], s) == 0)
			break;
	if (i < nchtab)
		put1(i + 128);
}

put1(c)	/* output char c */
int c;
{
	char *pw;
	register char *p;
	register int i, k;
	int j, ofont, code;

	if (!output)
		return;
	c -= 32;
	if (c <= 0) {
#ifdef DEBUGABLE
		if (dbg) fprintf(stderr,"non-exist 0%o\n", c + 32);
#endif
 		lastw = (widtab[font][0] * pstab[size] + dev.unitwidth/2)
								/ dev.unitwidth;
		return;
	}
	k = ofont = font;
	i = fitab[font][c] & BMASK;
	if (i != 0) {			/* it's on this font */
		p = codetab[font];	/* get the printing value of ch */
		pw = widtab[font];	/* get the width */
	} else		/* on another font (we hope) */
		for (k=font, j=0; j <= nfonts; j++, k = (k+1) % (nfonts+1)){
			if (fitab[k] == 0)
				continue;
			if ((i = fitab[k][c] & BMASK) != 0) {
				p = codetab[k];
				pw = widtab[k];
				setfont(k);
				break;
			}
		}

	if (i == 0 || (code = p[i] & BMASK) == 0 || k > nfonts) {
#ifdef DEBUGABLE
		if (dbg) fprintf(stderr,"not found 0%o\n", c+32);
#endif
		return;
	}
#ifdef DEBUGABLE
	if (dbg) {
		if (isprint(c+32))
			fprintf(stderr,"%c %d\n", c+32, code);
		else
			fprintf(stderr,"%03o %d\n", c+32, code);
	}
#endif
	outc(code);	/* character is < 254 */
	if (font != ofont)
		setfont(ofont);
	lastw = ((pw[i]&077) * pstab[size] + dev.unitwidth/2) / dev.unitwidth;
}



setsize(n)	/* set point size to n (internal) */
int n;
{

	if (n == size)
		return;	/* already there */
	if (vloadfont(font, pstab[n]) != -1)
		size = n;
}

/*ARGSUSED*/
t_fp(n, s, si)	/* font position n now contains font s, intname si */
int n;		/* internal name is ignored */
char *s, *si;
{
	register int i;

			/* first convert s to filename if possible */
	for (i = 0; fontmap[i].ffile != (char *) 0; i++) {
#ifdef DEBUGABLE
		if(dbg>1)fprintf(stderr,"testing :%s:%s:\n",s,fontmap[i].fname);
#endif
		if (strcmp(s, fontmap[i].fname) == 0) {
			s = fontmap[i].ffile;
#ifdef DEBUGABLE
			if(dbg)fprintf(stderr, "found :%s:\n",fontmap[i].ffile);
#endif
			break;
		}
	}

	fontname[n] = s;
	for(i = 0;i <= NFONTS;i++)	/* free the bits of that font */
		if (fontdes[i].fnum == n){
			nfree(fontdes[i].bits);
			fontdes[i].fnum = -1;
		}
}

setfont(n)	/* set font to n */
int n;
{
	if (n < 0 || n > NFONTS)
		error(FATAL, "illegal font %d", n);
	if (vloadfont(n,pstab[size]) != -1)
		font = n;
}

vloadfont(fnum, fsize)
register int fnum;
register int fsize;
{
	register int i;

	fontwanted = 0;
	if (fnum == cfnum && fsize == cpsize)
		return(0);
	for (i = 0; i <= NFONTS; i++) {
		if (fontdes[i].fnum == fnum && fontdes[i].psize == fsize) {
			cfnum = fontdes[i].fnum;
			cpsize = fontdes[i].psize;
			dispatch = &fontdes[i].disp[0];
			bits = fontdes[i].bits;
			cfont = i;
			return (0);
		}
	}
		/* this is a new font */
	if (fnum < 0 || fnum > NFONTS || fontname[fnum] == 0) {
#ifdef DEBUGABLE
	    if (dbg) error(!FATAL, "illegal font %d size %d", fnum, fsize);
#endif
	    return(-1);
	}
		/* Need to verify the existance of that font/size here*/
	nfontnum = fnum;
	npsize = fsize;
	fontwanted++;
	return (0);
}


getfont()
{
	register int fnum;
	register int fsize;
	register int fontd;
	register int d;
	register int sizehunt = size;
	char cbuf[BUFSIZ];


	fnum = nfontnum;
	fsize = npsize;
			/* try to open font file - if unsuccessful, hunt for */
			/* a file of same style, different size to substitute */
	d = -1;	/* direction to look in pstab (smaller first) */
	do {
	    sprintf(cbuf, "%s/%s.%d", bitdir, fontname[fnum], fsize);
	    fontd = open(cbuf, OPENREAD);
	    if (fontd == -1) {		/* File wasn't found. Try another ps */
		sizehunt += d;
		if (sizehunt < 0) {	/* past beginning - look higher */
		    d = 1;
		    sizehunt = size + 1;
		}
		if (sizehunt > nsizes) {	/* past top - forget it */
		    d = 0;
		} else {
		    fsize = pstab[sizehunt];
		}
	    }
	} while (fontd == -1 && d != 0);

	if (fontd == -1) {		/* completely unsuccessful */
	    perror(cbuf);
	    error(!FATAL,"fnum = %d, psize = %d, name = %s",
		fnum, npsize, fontname[fnum]);
	    fontwanted = 0;
	    return (-1);
	}
	if (read(fontd, &header, sizeof  (header)) != sizeof (header)
						|| header.magic != 0436)
		fprintf(stderr, "%s: Bad font file", cbuf);
	else {
		cfont = relfont();
		if ((bits=nalloc(header.size+DSIZ+1,1))== NULL)
			if ((bits=allpanic(header.size+DSIZ+1))== NULL) {
				error(FATAL, "%s: ran out of memory", cbuf);
			}

			/*
			 * have allocated one chunk of mem for font, dispatch.
			 * get the dispatch addr, align to word boundary.
			 */

		d = (int) bits+header.size;
		d += 1;
		d &= ~1;
		if (read (fontd, d, DSIZ) != DSIZ
			    || read (fontd, bits, header.size) != header.size)
			fprintf(stderr, "bad font header");
		else {
			close(fontd);
			cfnum = fontdes[cfont].fnum = fnum;
			cpsize = fontdes[cfont].psize = fsize;
			fontdes [cfont].bits = bits;
			fontdes [cfont].disp = (struct dispatch *) d;
			dispatch = &fontdes[cfont].disp[0];
			fontwanted = 0;
			return (0);
		}
	}
	close(fontd);
	fontwanted = 0;
	return(-1);
}

/*
 * "release" a font position - find an empty one, if possible
 */

relfont()
{
    register int newfont;

    for (newfont = 0; newfont < NFONTS; newfont++)
	if (fontdes [newfont].fnum == -1)
	    break;
    if (fontdes [newfont].fnum != -1) {
	nfree (fontdes [newfont].bits);
#ifdef DEBUGABLE
	if (dbg) fprintf (stderr, "freeing position %d\n", newfont);
    } else {
	if (dbg)
	    fprintf (stderr, "taking, not freeing, position %d\n", newfont);
#endif
    }
    fontdes[newfont].fnum = -1;
    return (newfont);
}

char *allpanic (nbytes)
int nbytes;
{
	register int i;

	for (i = 0; i <= NFONTS; i++) {
		if (fontdes[i].fnum != -1) nfree(fontdes[i].bits);
		fontdes[i].fnum = fontdes[i].psize = -1;
		cfnum = cpsize = -1;
	}
	return(nalloc(nbytes,1));
}

int	M[] = { 0xffffffff, 0xfefefefe, 0xfcfcfcfc, 0xf8f8f8f8,
		0xf0f0f0f0, 0xe0e0e0e0, 0xc0c0c0c0, 0x80808080, 0x0 };
int	N[] = { 0x00000000, 0x01010101, 0x03030303, 0x07070707,
		0x0f0f0f0f, 0x1f1f1f1f, 0x3f3f3f3f, 0x7f7f7f7f, 0xffffffff };
int	strim[] = { 0xffffffff, 0xffffff00, 0xffff0000, 0xff000000, 0 };

outc(code)
int code;		/* character to print */
{
    register struct dispatch *dis; /* ptr to character font record */
    register char *addr;	/* addr of font data */
    int llen;			/* length of each font line */
    int nlines;			/* number of font lines */
    register char *scanp;	/* ptr to output buffer */
    int scanp_inc;		/* increment to start of next buffer */
    int offset;			/* bit offset to start of font data */
    register int i;		/* loop counter */
    register int count;		/* font data ptr */
    register unsigned fontdata;	/* font data temporary */
    register int off8;		/* offset + 8 */

    if (fontwanted)
	if (getfont()) return;
    dis = dispatch + code;
    if (dis->nbytes) {
	addr = bits + dis->addr;
	llen = (dis->left + dis->right + 7) / 8;
	nlines = dis->up + dis->down;
	if ((i = vpos + dis->down) > maxv) maxv = i;
	scanp = buf0p + (vpos - (vorigin + dis->up)) * BYTES_PER_LINE
			+ (hpos - dis->left) / 8;
	scanp_inc = BYTES_PER_LINE - llen;
	offset = - ((hpos - dis->left) &07);
	off8 = offset + 8;
	for (i = 0; i < nlines; i++) {
	    if ((unsigned) (scanp + (count = llen)) > (unsigned) BUFBOTTOM)
		return;
	    if (scanp >= BUFTOP) {
		do {
		    fontdata = *(unsigned *)addr;
		    addr += 4;
		    if (count < 4)
			fontdata &= ~strim[count];
		    *(unsigned*)scanp |=(fontdata << offset) & ~M[off8];
		    scanp++;
		    *(unsigned*)scanp |=(fontdata << off8) & ~N[off8];
		    scanp += 3;
		    count -= 4;
		} while (count > 0);
	    }
	    scanp += scanp_inc+count;
	    addr += count;
	}
    }
}


vwrite(buf,usize)
char *buf;
int usize;
{
	register int tsize = 0;

	while (usize) {
		buf += tsize;
		tsize = usize > MAXWRIT ? MAXWRIT : usize;
#ifdef DEBUGABLE
		if (dbg)fprintf(stderr,"buf = %d size = %d\n",buf,tsize);
#endif
		if ((tsize = write(OUTFILE, buf, tsize)) < 0) {
			perror("dver: write failed");
			exit(RESTART);
		}
		usize -= tsize;
	}
}

vclear (ptr, nbytes)
char	*ptr;
int nbytes;
{
    register tsize = 0;

    while (nbytes){
	if ((unsigned)(16*1024) < nbytes) {
	    tsize = 16 * 1024;
	} else
	    tsize = nbytes;
	nbytes -= tsize;
#ifdef DEBUGABLE
	if (dbg) fprintf(stderr,"clearing ptr = %d size = %d\n",ptr,tsize);
#endif
	clear(ptr,tsize);
	ptr += tsize;
    }
}

/*ARGSUSED*/
clear(lp, nbytes)
char *lp;
int nbytes;
{
	asm("movc5 $0,(sp),$0,8(ap),*4(ap)");
}

char *
nalloc(i, j)
int i, j;
{
	register char *cp;

	cp = calloc(i, j);
#ifdef DEBUGABLE
	if (dbg) fprintf(stderr, "allocated %d bytes at %x\n", i * j, cp);
#endif
	return(cp);
}

nfree(cp)
char *cp;
{
#ifdef DEBUGABLE
	if (dbg) fprintf(stderr, "freeing at %x\n", cp);
#endif
	free(cp);
}


/*
 * Plot a dot at (x, y).  Points should be in the range 0 <= x < RASTER_LENGTH,
 * vorigin <= y < vorigin + NLINES.  If the point will not fit on the buffer,
 * it is left out.  Things outside the x boundary are wrapped around the end.
 */
point(x, y)
int x, y;
{
    register char *ptr = buf0p + (y - vorigin) * BYTES_PER_LINE + (x >> 3);

    if (ptr <= BUFBOTTOM && ptr >= BUFTOP)	/* ignore it if it wraps over */
	*ptr |= 1 << (7 - (x & 07));
}

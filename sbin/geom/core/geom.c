/*-
 * Copyright (c) 2004 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <libutil.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <assert.h>
#include <libgeom.h>
#include <geom.h>

#include "misc/subr.h"


static char comm[MAXPATHLEN], *class_name = NULL, *gclass_name = NULL;
static uint32_t *version = NULL;
static int verbose = 0;
static struct g_command *class_commands = NULL;
static void (*usage)(const char *name);

static struct g_command *find_command(const char *cmdstr, int all);
static int std_available(const char *name);

static void std_help(struct gctl_req *req, unsigned flags);
static void std_list(struct gctl_req *req, unsigned flags);
static void std_load(struct gctl_req *req, unsigned flags);
static void std_unload(struct gctl_req *req, unsigned flags);

struct g_command std_commands[] = {
	{ "help", 0, std_help, G_NULL_OPTS },
	{ "list", 0, std_list, G_NULL_OPTS },
	{ "load", G_FLAG_VERBOSE | G_FLAG_LOADKLD, std_load, G_NULL_OPTS },
	{ "unload", G_FLAG_VERBOSE, std_unload, G_NULL_OPTS },
	G_CMD_SENTINEL
};

static void
std_usage(const char *name)
{
	struct g_command *cmd;
	struct g_option *opt;
	unsigned i, j;

	for (i = 0; ; i++) {
		cmd = &class_commands[i];
		if (cmd->gc_name == NULL)
			break;
		fprintf(stderr, "%s %s %s %s", i == 0 ? "usage:" : "      ",
		    name, class_name, cmd->gc_name);
		if ((cmd->gc_flags & G_FLAG_VERBOSE) != 0)
			fprintf(stderr, " [-v]");
		for (j = 0; ; j++) {
			opt = &cmd->gc_options[j];
			if (opt->go_name == NULL)
				break;
			if (opt->go_val != NULL || opt->go_type == G_TYPE_NONE)
				fprintf(stderr, " [");
			else
				fprintf(stderr, " ");
			fprintf(stderr, "-%c", opt->go_char);
			if (opt->go_type != G_TYPE_NONE)
				fprintf(stderr, " %s", opt->go_name);
			if (opt->go_val != NULL || opt->go_type == G_TYPE_NONE)
				fprintf(stderr, "]");
		}
		fprintf(stderr, " ...\n");
	}
	exit(EXIT_FAILURE);
}

static void
geom_usage(void)
{

	if (class_name == NULL) {
		errx(EXIT_FAILURE, "usage: %s <class> <command> [options]",
		    "geom");
	} else {
		const char *prefix;
		unsigned i;

		if (usage == NULL)
			prefix = "usage:";
		else {
			usage(comm);
			prefix = "      ";
		}
		for (i = 0; ; i++) {
			struct g_command *cmd;

			cmd = &std_commands[i];
			if (cmd->gc_name == NULL)
				break;
			if (find_command(cmd->gc_name, 0) != NULL)
				continue;
			if (!std_available(cmd->gc_name))
				continue;
			fprintf(stderr, "%s %s %s", prefix, comm, cmd->gc_name);
			if ((cmd->gc_flags & G_FLAG_VERBOSE) != 0)
				fprintf(stderr, " [-v]");
			fprintf(stderr, "\n");
			prefix = "      ";
		}
		exit(EXIT_FAILURE);
	}
}

static void
load_module(void)
{
	char name1[64], name2[64];

	snprintf(name1, sizeof(name1), "g_%s", class_name);
	snprintf(name2, sizeof(name2), "geom_%s", class_name);
	if (modfind(name1) < 0) {
		/* Not present in kernel, try loading it. */
		if (kldload(name2) < 0 || modfind(name1) < 0) {
			if (errno != EEXIST) {
				errx(EXIT_FAILURE,
				    "%s module not available!", name2);
			}
		}
	}
}

static int
strlcatf(char *str, size_t size, const char *format, ...)
{
	size_t len;
	va_list ap;
	int ret;

	len = strlen(str);
	str += len;
	size -= len;

	va_start(ap, format);
	ret = vsnprintf(str, size, format, ap);
	va_end(ap);

	return (ret);
}

/*
 * Find given option in options available for given command.
 */
static struct g_option *
find_option(struct g_command *cmd, char ch)
{
	struct g_option *opt;
	unsigned i;

	for (i = 0; ; i++) {
		opt = &cmd->gc_options[i];
		if (opt->go_name == NULL)
			return (NULL);
		if (opt->go_char == ch)
			return (opt);
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Add given option to gctl_req.
 */
static void
set_option(struct gctl_req *req, struct g_option *opt, const char *val)
{

	if (opt->go_type == G_TYPE_NUMBER) {
		intmax_t number;

		errno = 0;
		number = strtoimax(optarg, NULL, 0);
		if (errno != 0) {
			err(EXIT_FAILURE, "Invalid value for '%c' argument.",
			    opt->go_char);
		}
		opt->go_val = malloc(sizeof(intmax_t));
		if (opt->go_val == NULL)
			errx(EXIT_FAILURE, "No memory.");
		*(intmax_t *)opt->go_val = number;

		gctl_ro_param(req, opt->go_name, sizeof(intmax_t), opt->go_val);
	} else if (opt->go_type == G_TYPE_STRING) {
		gctl_ro_param(req, opt->go_name, -1, optarg);
	} else /* if (opt->go_type == G_TYPE_NONE) */ {
		opt->go_val = malloc(sizeof(int));
		if (opt->go_val == NULL)
			errx(EXIT_FAILURE, "No memory.");
		*(int *)opt->go_val = *val - '0';

		gctl_ro_param(req, opt->go_name, sizeof(int),
		    opt->go_val);
	}
}

/*
 * 1. Add given argument by caller.
 * 2. Add default values of not given arguments.
 * 3. Add the rest of arguments.
 */
static void
parse_arguments(struct g_command *cmd, struct gctl_req *req, int *argc,
    char ***argv)
{
	struct g_option *opt;
	char opts[64];
	unsigned i;
	int ch;

	*opts = '\0';
	if ((cmd->gc_flags & G_FLAG_VERBOSE) != 0)
		strlcat(opts, "v", sizeof(opts));
	for (i = 0; ; i++) {
		opt = &cmd->gc_options[i];
		if (opt->go_name == NULL)
			break;
		strlcatf(opts, sizeof(opts), "%c", opt->go_char);
		if (opt->go_type != G_TYPE_NONE)
			strlcat(opts, ":", sizeof(opts));
	}

	/*
	 * Add specified arguments.
	 */
	while ((ch = getopt(*argc, *argv, opts)) != -1) {
		/* Standard (not passed to kernel) options. */
		switch (ch) {
		case 'v':
			verbose = 1;
			continue;
		}
		/* Options passed to kernel. */
		opt = find_option(cmd, ch);
		if (opt == NULL)
			geom_usage();
		if (G_OPT_ISDONE(opt)) {
			fprintf(stderr, "Flag '%c' specified twice.\n",
			    opt->go_char);
			geom_usage();
		}
		G_OPT_DONE(opt);

		if (opt->go_type == G_TYPE_NONE)
			set_option(req, opt, "1");
		else
			set_option(req, opt, optarg);
	}
	*argc -= optind;
	*argv += optind;

	/*
	 * Add not specified arguments, but with default values.
	 */
	for (i = 0; ; i++) {
		opt = &cmd->gc_options[i];
		if (opt->go_name == NULL)
			break;
		if (G_OPT_ISDONE(opt))
			continue;

		if (opt->go_type == G_TYPE_NONE) {
			assert(opt->go_val == NULL);
			set_option(req, opt, "0");
		} else {
			if (opt->go_val == NULL) {
				fprintf(stderr, "Flag '%c' not specified.\n",
				    opt->go_char);
				geom_usage();
			} else {
				if (opt->go_type == G_TYPE_NUMBER) {
					gctl_ro_param(req, opt->go_name,
					    sizeof(intmax_t), opt->go_val);
				} else /* if (opt->go_type == G_TYPE_STRING)*/ {
					gctl_ro_param(req, opt->go_name, -1,
					    opt->go_val);
				}
			}
		}
	}
	/*
	 * Add rest of given arguments.
	 */
	gctl_ro_param(req, "nargs", sizeof(int), argc);
	for (i = 0; i < (unsigned)*argc; i++) {
		char argname[16];

		snprintf(argname, sizeof(argname), "arg%u", i);
		gctl_ro_param(req, argname, -1, (*argv)[i]);
	}
}

/*
 * Find given command in commands available for given class.
 */
static struct g_command *
find_command(const char *cmdstr, int all)
{
	struct g_command *cmd;
	unsigned i;

	/*
	 * First try to find command defined by loaded library.
	 */
	if (class_commands != NULL) {
		for (i = 0; ; i++) {
			cmd = &class_commands[i];
			if (cmd->gc_name == NULL)
				break;
			if (strcmp(cmd->gc_name, cmdstr) == 0)
				return (cmd);
		}
	}
	if (!all)
		return (NULL);
	/*
	 * Now try to find in standard commands.
	 */
	for (i = 0; ; i++) {
		cmd = &std_commands[i];
		if (cmd->gc_name == NULL)
			break;
		if (!std_available(cmd->gc_name))
			continue;
		if (strcmp(cmd->gc_name, cmdstr) == 0)
			return (cmd);
	}
	return (NULL);
}

static unsigned
set_flags(struct g_command *cmd)
{
	unsigned flags = 0;

	if ((cmd->gc_flags & G_FLAG_VERBOSE) != 0 && verbose)
		flags |= G_FLAG_VERBOSE;

	return (flags);
}

/*
 * Run command.
 */
static void
run_command(int argc, char *argv[])
{
	struct g_command *cmd;
	struct gctl_req *req;
	const char *errstr;
	char buf[4096];

	cmd = find_command(argv[0], 1);
	if (cmd == NULL) {
		fprintf(stderr, "Unknown command: %s\n", argv[0]);
		geom_usage();
	}
	if ((cmd->gc_flags & G_FLAG_LOADKLD) != 0)
		load_module();

	req = gctl_get_handle();
	gctl_ro_param(req, "class", -1, gclass_name);
	gctl_ro_param(req, "verb", -1, argv[0]);
	if (version != NULL)
		gctl_ro_param(req, "version", sizeof(*version), version);
	parse_arguments(cmd, req, &argc, &argv);

	if (cmd->gc_func != NULL) {
		unsigned flags;

		flags = set_flags(cmd);
		cmd->gc_func(req, flags);
		errstr = req->error;
	} else {
		bzero(buf, sizeof(buf));
		gctl_rw_param(req, "output", sizeof(buf), buf);
		errstr = gctl_issue(req);
	}
	if (errstr != NULL) {  
		fprintf(stderr, "%s\n", errstr);
		gctl_free(req);
		exit(EXIT_FAILURE);
	}
	if (*buf != '\0')
		printf("%s", buf);
	gctl_free(req);
	if (verbose)
		printf("Done.\n");
	exit(EXIT_SUCCESS);
}

static void
load_library(void)
{
	char path[MAXPATHLEN];
	uint32_t *lib_version;
	void *dlh;

	snprintf(path, sizeof(path), "%s/geom_%s.so", CLASSDIR, class_name);
	dlh = dlopen(path, RTLD_NOW);
	if (dlh == NULL) {
#if 0
		fprintf(stderr, "Cannot open library %s, but continuing "
		    "anyway.\n", path);
#endif
		/*
		 * Even if library cannot be loaded, standard commands are
		 * available, so don't panic!
		 */
		return;
	}
	lib_version = dlsym(dlh, "lib_version");
	if (lib_version == NULL) {
		fprintf(stderr, "Cannot find symbol %s: %s.\n", "lib_version",
		    dlerror());
		dlclose(dlh);
		exit(EXIT_FAILURE);
	}
	if (*lib_version != G_LIB_VERSION) {
		dlclose(dlh);
		errx(EXIT_FAILURE, "%s and %s are not synchronized.", comm,
		    path);
	}
	version = dlsym(dlh, "version");
	if (version == NULL) {
		fprintf(stderr, "Cannot find symbol %s: %s.\n", "version",
		    dlerror());
		dlclose(dlh);
		exit(EXIT_FAILURE);
	}
	class_commands = dlsym(dlh, "class_commands");
	if (class_commands == NULL) {
		fprintf(stderr, "Cannot find symbol %s: %s.\n",
		    "class_commands", dlerror());
		dlclose(dlh);
		exit(EXIT_FAILURE);
	}
	usage = dlsym(dlh, "usage");
	if (usage == NULL)
		usage = std_usage;
}

/*
 * Class name should be all capital letters.
 */
static void
set_class_name(void)
{
	char *s1, *s2;

	gclass_name = malloc(strlen(class_name));
	if (gclass_name == NULL)
		errx(EXIT_FAILURE, "No memory");
	s1 = gclass_name;
	s2 = class_name;
	for (; *s2 != '\0'; s2++)
		*s1++ = toupper(*s2);
	*s1 = '\0';
}

static void
get_class(int *argc, char ***argv)
{

	snprintf(comm, sizeof(comm), "%s", basename((*argv)[0]));
	if (strcmp(comm, "geom") == 0) {
		if (*argc < 2)
			geom_usage();
		strlcatf(comm, sizeof(comm), " %s", (*argv)[1]);
		class_name = (*argv)[1];
		*argc -= 2;
		*argv += 2;
	} else if (*comm == 'g') {
		class_name = comm + 1;
		*argc -= 1;
		*argv += 1;
	} else {
		errx(EXIT_FAILURE, "Invalid utility name.");
	}
	set_class_name();
	load_library();
	if (*argc < 1)
		geom_usage();
}

int
main(int argc, char *argv[])
{

	get_class(&argc, &argv);
	run_command(argc, argv);
	/* NOTREACHED */

	exit(EXIT_FAILURE);
}

static struct gclass *
find_class(struct gmesh *mesh, const char *name)
{
	struct gclass *classp;

	LIST_FOREACH(classp, &mesh->lg_class, lg_class) {
		if (strcmp(classp->lg_name, name) == 0)
			return (classp);
	}
	return (NULL);
}

static struct gprovider *
find_provider(struct gclass *classp, const char *name)
{
	struct ggeom *gp;
	struct gprovider *pp;

	LIST_FOREACH(gp, &classp->lg_geom, lg_geom) {
		LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
			if (strcmp(pp->lg_name, name) == 0)
				return (pp);
		}
	}
	return (NULL);
}

static char *
genspaces(const char *text, size_t len)
{
	static char spaces[256];
	size_t outlen;

	if (strlen(text) >= len) {
		spaces[0] = '\0';
		return (spaces);
	}
	memset(spaces, ' ', sizeof(spaces));
	outlen = len - strlen(text);
	if (outlen >= sizeof(spaces)) {
		spaces[sizeof(spaces) - 1] = '\0';
		return (spaces);
	}
	spaces[outlen] = '\0';
	return (spaces);
}

static void
show_one(struct gprovider *pp)
{
	struct gconfig *conf;
	char buf[5];

	printf("       NAME: %s\n", pp->lg_name);
	printf("  geom name: %s\n", pp->lg_geom->lg_name);
	humanize_number(buf, sizeof(buf), (int64_t)pp->lg_mediasize, "",
	    HN_AUTOSCALE, HN_B | HN_NOSPACE | HN_DECIMAL);
	printf("  mediasize: %jd (%s)\n", (intmax_t)pp->lg_mediasize, buf);
	printf(" sectorsize: %u\n", pp->lg_sectorsize);
	printf("       mode: %s\n", pp->lg_mode);
	LIST_FOREACH(conf, &pp->lg_config, lg_config) {
		printf("%s%s: %s\n", genspaces(conf->lg_name, 11),
		    conf->lg_name, conf->lg_val);
	}
	printf("\n");
}

static void
std_help(struct gctl_req *req __unused, unsigned flags __unused)
{

	geom_usage();
}

static int
std_list_available(void)
{
	struct gmesh mesh;
	struct gclass *classp;
	int error;

	error = geom_gettree(&mesh);
	if (error != 0)
		exit(EXIT_FAILURE);
	classp = find_class(&mesh, gclass_name);
	geom_deletetree(&mesh);
	if (classp != NULL)
		return (1);
	return (0);
}

static void
std_list(struct gctl_req *req, unsigned flags __unused)
{
	struct gmesh mesh;
	struct gclass *classp;
	struct gprovider *pp;
	int error, *nargs;

	error = geom_gettree(&mesh);
	if (error != 0)
		exit(EXIT_FAILURE);
	classp = find_class(&mesh, gclass_name);
	if (classp == NULL) {
		geom_deletetree(&mesh);
		fprintf(stderr, "Class %s not found.\n", gclass_name);
		return;
	}
	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	if (nargs == NULL) {
		gctl_error(req, "No '%s' argument.", "nargs");
		geom_deletetree(&mesh);
		return;
	}
	if (*nargs > 0) {
		int i;

		for (i = 0; i < *nargs; i++) {
			const char *name;
			char param[16];

			snprintf(param, sizeof(param), "arg%d", i);
			name = gctl_get_asciiparam(req, param);
			assert(name != NULL);
			pp = find_provider(classp, name);
			if (pp != NULL)
				show_one(pp);
			else {
				fprintf(stderr, "No such provider: %s.\n",
				    name);
			}
		}
	} else {
		struct ggeom *gp;

		LIST_FOREACH(gp, &classp->lg_geom, lg_geom) {
			LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
				show_one(pp);
			}
		}
	}
	geom_deletetree(&mesh);
}

static int
std_load_available(void)
{
	char name[MAXPATHLEN], paths[MAXPATHLEN * 8], *p;
	struct stat sb;
	size_t len;

	snprintf(name, sizeof(name), "g_%s", class_name);
	/*
	 * If already in kernel, "load" command is not available.
	 */
	if (modfind(name) >= 0)
		return (0);
	bzero(paths, sizeof(paths));
	len = sizeof(paths);
	if (sysctlbyname("kern.module_path", paths, &len, NULL, 0) < 0)
		err(EXIT_FAILURE, "sysctl(kern.module_path)");
	for (p = strtok(paths, ";"); p != NULL; p = strtok(NULL, ";")) {
		snprintf(name, sizeof(name), "%s/geom_%s.ko", p, class_name);
		/*
		 * If geom_<name>.ko file exists, "load" command is available.
		 */
		if (stat(name, &sb) == 0)
			return (1);
	}
	return (0);
}

static void
std_load(struct gctl_req *req __unused, unsigned flags)
{

	/*
	 * Do nothing special here, because of G_FLAG_LOADKLD flag,
	 * module is already loaded.
	 */
	if ((flags & G_FLAG_VERBOSE) != 0)
		printf("Module available.\n");
}

static int
std_unload_available(void)
{
	char name[64];
	int id;

	snprintf(name, sizeof(name), "geom_%s", class_name);
	id = kldfind(name);
	if (id >= 0)
		return (1);
	return (0);
}

static void
std_unload(struct gctl_req *req, unsigned flags __unused)
{
	char name[64];
	int id;

	snprintf(name, sizeof(name), "geom_%s", class_name);
	id = kldfind(name);
	if (id < 0) {
		gctl_error(req, "Could not find module: %s.", strerror(errno));
		return;
	}
	if (kldunload(id) < 0) {
		gctl_error(req, "Could not unload module: %s.",
		    strerror(errno));
		return;
	}
}

static int
std_available(const char *name)
{

	if (strcmp(name, "help") == 0)
		return (1);
	else if (strcmp(name, "list") == 0)
		return (std_list_available());
	else if (strcmp(name, "load") == 0)
		return (std_load_available());
	else if (strcmp(name, "unload") == 0)
		return (std_unload_available());
	else
		assert(!"Unknown standard command.");
	return (0);
}

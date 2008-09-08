/*
 * Copyright 2008 Gentoo Foundation
 * Distributed under the terms of the GNU General Public License v2
 * $Header: /var/cvsroot/gentoo-projects/pax-utils/scanmacho.c,v 1.2 2008/09/08 08:01:35 grobian Exp $
 *
 * based on scanelf by:
 * Copyright 2003-2007 Ned Ludd        - <solar@gentoo.org>
 * Copyright 2004-2007 Mike Frysinger  - <vapier@gentoo.org>
 * for Darwin specific fun:
 *                2008 Fabian Groffen  - <grobian@gentoo.org>
 */

static const char *rcsid = "$Id: scanmacho.c,v 1.2 2008/09/08 08:01:35 grobian Exp $";
const char * const argv0 = "scanmacho";

#include "paxinc.h"

#define IS_MODIFIER(c) (c == '%' || c == '#' || c == '+')

/* prototypes */
static int scanmacho_fatobj(fatobj *fobj);
static int scanmacho_file(const char *filename, const struct stat *st_cache);
static int scanmacho_from_file(const char *filename);
static int scanmacho_dir(const char *path);
static void scanelf_envpath(void);
static void usage(int status);
static int parseargs(int argc, char *argv[]);

/* variables to control behavior */
static char match_etypes[126] = "";
static char scan_envpath = 0;
static char scan_symlink = 1;
static char scan_archives = 0;
static char dir_recurse = 0;
static char dir_crossmount = 1;
static char show_perms = 0;
static char show_size = 0;
static char show_needed = 0;
static char show_interp = 0;
static char show_bind = 0;
static char show_soname = 0;
static char show_banner = 1;
static char show_endian = 0;
static char be_quiet = 0;
static char be_verbose = 0;
static char be_wewy_wewy_quiet = 0;
static char be_semi_verbose = 0;
static char *find_lib = NULL;
static char *out_format = NULL;
static char *search_path = NULL;
static char g_match = 0;

int match_bits = 0;
unsigned int match_perms = 0;
caddr_t ldcache = 0;
size_t ldcache_size = 0;
unsigned long setpax = 0UL;

int has_objdump = 0;

static char *getstr_perms(const char *fname);
static char *getstr_perms(const char *fname)
{
	struct stat st;
	static char buf[8];

	if ((stat(fname, &st)) == (-1))
		return (char *) "";

	snprintf(buf, sizeof(buf), "%o", st.st_mode);

	return (char *) buf + 2;
}

static const char *macho_file_needed_lib(
		fatobj *fobj,
		char *found_needed,
		char *found_lib,
		int op,
		char **ret,
		size_t *ret_len
) {
	char *needed;
	loadcmd *lcmd;
	struct dylib_command *dlcmd;
	uint32_t lc_load_dylib = LC_LOAD_DYLIB;

	if ((op == 0 && !show_needed) || (op == 1 && !find_lib))
		return NULL;

	lcmd = firstloadcmd(fobj);

	if (fobj->swapped)
		lc_load_dylib = bswap_32(lc_load_dylib);

	do {
		if (lcmd->lcmd->cmd == lc_load_dylib) {
			dlcmd = (struct dylib_command*)lcmd->data;
			if (fobj->swapped) {
				needed = (char *)(lcmd->data +
						bswap_32(dlcmd->dylib.name.offset));
			} else {
				needed = (char *)(lcmd->data + dlcmd->dylib.name.offset);
			}
			if (op == 0) {
				if (!be_wewy_wewy_quiet) {
					if (*found_needed)
						xchrcat(ret, ',', ret_len);
					xstrcat(ret, needed, ret_len);
				}
				*found_needed = 1;
			} else {
				if (!strncmp(find_lib, needed,
							strlen(!g_match ? needed : find_lib)))
				{
					*found_lib = 1;
					free(lcmd);
					return (be_wewy_wewy_quiet ? NULL : needed);
				}
			}
		}
	} while (nextloadcmd(lcmd));

	if (op == 0 && !*found_needed && be_verbose)
		warn("Mach-O lacks LC_LOAD_DYLIB commands: %s", fobj->filename);

	return NULL;
}

static char *macho_file_interp(fatobj *fobj, char *found_interp)
{
	loadcmd *lcmd;
	uint32_t lc_load_dylinker = LC_LOAD_DYLINKER;

	if (!show_interp)
		return NULL;

	lcmd = firstloadcmd(fobj);

	if (fobj->swapped)
		lc_load_dylinker = bswap_32(lc_load_dylinker);

	do {
		if (lcmd->lcmd->cmd == lc_load_dylinker) {
			struct dylinker_command *dlcmd =
				(struct dylinker_command*)lcmd->data;
			char *dylinker;
			if (fobj->swapped) {
				dylinker = (char *)(lcmd->data +
						bswap_32(dlcmd->name.offset));
			} else {
				dylinker = (char *)(lcmd->data + dlcmd->name.offset);
			}
			*found_interp = 1;
			free(lcmd);
			return (be_wewy_wewy_quiet ? NULL : dylinker);
		}
	} while (nextloadcmd(lcmd));
	
	return NULL;
}

static char *macho_file_soname(fatobj *fobj, char *found_soname)
{
	loadcmd *lcmd;
	char *soname;
	uint32_t lc_id_dylib = LC_ID_DYLIB;

	if (!show_soname)
		return NULL;

	lcmd = firstloadcmd(fobj);

	if (fobj->swapped)
		lc_id_dylib = bswap_32(lc_id_dylib);

	do {
		if (lcmd->lcmd->cmd == lc_id_dylib) {
			struct dylib_command *dlcmd = (struct dylib_command*)lcmd->data;
			if (fobj->swapped) {
				soname = (char *)(lcmd->data +
						bswap_32(dlcmd->dylib.name.offset));
			} else {
				soname = (char *)(lcmd->data + dlcmd->dylib.name.offset);
			}
			*found_soname = 1;
			free(lcmd);
			return (be_wewy_wewy_quiet ? NULL : soname);
		}
	} while (nextloadcmd(lcmd));
	
	return NULL;
}


/* scan a macho file and show all the fun stuff */
#define prints(str) write(fileno(stdout), str, strlen(str))
static int scanmacho_fatobj(fatobj *fobj)
{
	unsigned long i;
	char found_needed, found_interp, found_soname, found_lib, found_file;
	static char *out_buffer = NULL;
	static size_t out_len;

	found_needed = found_interp = found_soname = \
	found_lib = found_file = 0;

	if (be_verbose > 2)
		printf("%s: scanning file {%s,%s}\n", fobj->filename,
		       get_machocputype(fobj),
		       get_machosubcputype(fobj));
	else if (be_verbose > 1)
		printf("%s: scanning file\n", fobj->filename);

	/* init output buffer */
	if (!out_buffer) {
		out_len = sizeof(char) * 80;
		out_buffer = xmalloc(out_len);
	}
	*out_buffer = '\0';

	/* show the header */
	if (!be_quiet && show_banner) {
		for (i = 0; out_format[i]; ++i) {
			if (!IS_MODIFIER(out_format[i])) continue;

			switch (out_format[++i]) {
			case '+': break;
			case '%': break;
			case '#': break;
			case 'F':
			case 'p':
			case 'f': prints("FILE "); found_file = 1; break;
			case 'o': prints("  TYPE   "); break;
			case 'M': prints("CPU "); break;
			case 'n': prints("NEEDED "); break;
			case 'i': prints("DYLINKER "); break;
			case 'b': prints("FLAGS "); break;
			case 'Z': prints("SIZE "); break;
			case 'S': prints("INSTALLNAME "); break;
			case 'N': prints("LIB "); break;
			case 'a': prints("ARCH "); break;
			case 'O': prints("PERM "); break;
			case 'D': prints("ENDIAN "); break;
			default: warnf("'%c' has no title ?", out_format[i]);
			}
		}
		if (!found_file) prints("FILE ");
		prints("\n");
		found_file = 0;
		show_banner = 0;
	}

	/* dump all the good stuff */
	for (i = 0; out_format[i]; ++i) {
		const char *out;
		const char *tmp;
		static char ubuf[sizeof(unsigned long)*2];
		if (!IS_MODIFIER(out_format[i])) {
			xchrcat(&out_buffer, out_format[i], &out_len);
			continue;
		}

		out = NULL;
		be_wewy_wewy_quiet = (out_format[i] == '#');
		be_semi_verbose = (out_format[i] == '+');
		switch (out_format[++i]) {
		case '+':
		case '%':
		case '#':
			xchrcat(&out_buffer, out_format[i], &out_len); break;
		case 'F':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			xstrcat(&out_buffer, fobj->filename, &out_len);
			break;
		case 'p':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			tmp = fobj->filename;
			if (search_path) {
				ssize_t len_search = strlen(search_path);
				ssize_t len_file = strlen(fobj->filename);
				if (!strncmp(fobj->filename, search_path, len_search) && \
				    len_file > len_search)
					tmp += len_search;
				if (*tmp == '/' && search_path[len_search-1] == '/') tmp++;
			}
			xstrcat(&out_buffer, tmp, &out_len);
			break;
		case 'f':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			xstrcat(&out_buffer, fobj->base_filename, &out_len);
			break;
		case 'o': out = get_machomhtype(fobj); break;
		case 'M': out = get_machocputype(fobj); break;
		case 'D': out = get_machoendian(fobj); break;
		case 'O': out = getstr_perms(fobj->filename); break;
		case 'n':
		case 'N': out = macho_file_needed_lib(fobj, &found_needed, &found_lib, (out_format[i]=='N'), &out_buffer, &out_len); break;
		case 'i': out = macho_file_interp(fobj, &found_interp); break;
		case 'b': get_machomhflags(fobj, &out_buffer, &out_len); break;
		case 'S': out = macho_file_soname(fobj, &found_soname); break;
		case 'a': out = get_machomtype(fobj); break;
		case 'Z': snprintf(ubuf, sizeof(ubuf), "%llu", (unsigned long long int)fobj->len); out = ubuf; break;;
		default: warnf("'%c' has no scan code?", out_format[i]);
		}
		if (out) {
			/* hack for comma delimited output like `scanelf -s sym1,sym2,sym3` */
			if (out_format[i] == 's' && (tmp=strchr(out,',')) != NULL)
				xstrncat(&out_buffer, out, &out_len, (tmp-out));
			else
				xstrcat(&out_buffer, out, &out_len);
		}
	}

#define FOUND_SOMETHING() \
	( found_needed || found_interp || found_soname || found_lib )

	if (!found_file && (!be_quiet || (be_quiet && FOUND_SOMETHING()))) {
		xchrcat(&out_buffer, ' ', &out_len);
		xstrcat(&out_buffer, fobj->filename, &out_len);
	}
	if (!be_quiet || (be_quiet && FOUND_SOMETHING())) {
		puts(out_buffer);
		fflush(stdout);
	}

	return 0;
}

/* scan a single Mach-O */
static int scanmacho_fat(const char *filename, int fd, size_t len)
{
	int ret = 1;
	fatobj *fobj;
	fatobj *walk;

	/* verify this is real Mach-O */
	if ((fobj = readmacho_fd(filename, fd, len)) == NULL) {
		if (be_verbose > 2) printf("%s: not a Mach-O object\n", filename);
		return ret;
	}
	switch (match_bits) {
		case 32:
		case 64:
			walk = fobj;
			do {
				if ((walk->ismach64 && match_bits == 64) ||
						(!walk->ismach64 && match_bits == 32))
				{
					ret = scanmacho_fatobj(walk);
				}
			} while (walk->next != NULL && (walk = walk->next));
			goto label_done;
		default:
			break;
	}
	if (strlen(match_etypes)) {
		char sbuf[128];
		char ftype[32];

		snprintf(sbuf, 128, ",%s,", match_etypes);

		walk = fobj;
		do {
			snprintf(ftype, 32, ",%s,", get_machomhtype(walk));
			if (strstr(sbuf, ftype) != NULL) {
				ret = scanmacho_fatobj(walk);
			}
		} while (walk->next != NULL && (walk = walk->next));
		goto label_done;
	}

	walk = fobj;
	do {
		ret = scanmacho_fatobj(walk);
	} while (walk->next != NULL && (walk = walk->next));

label_done:
	unreadmacho(fobj);
	return ret;
}

/* scan an archive of Mach-Os */
static int scanmacho_archive(const char *filename, int fd, size_t len)
{
	archive_handle *ar;
	archive_member *m;
	char *ar_buffer;
	fatobj *fobj;
	fatobj *walk;

	ar = ar_open_fd(filename, fd);
	if (ar == NULL)
		return 1;

	ar_buffer = (char*)mmap(0, len, PROT_READ, MAP_PRIVATE, fd, 0);
	while ((m = ar_next(ar)) != NULL) {
		fobj = readmacho_buffer(m->name, ar_buffer + lseek(fd, 0, SEEK_CUR), m->size);
		if (fobj) {
			walk = fobj;
			do {
				scanmacho_fatobj(walk);
			} while (walk->next != NULL && (walk = walk->next));
			unreadmacho(fobj);
		}
	}
	munmap(ar_buffer, len);

	return 0;
}
/* scan a file which may be an Mach-O or an archive or some other
 * magical beast */
static int scanmacho_file(const char *filename, const struct stat *st_cache)
{
	const struct stat *st = st_cache;
	struct stat symlink_st;
	int fd;

	/* always handle regular files and handle symlinked files if no -y */
	if (S_ISLNK(st->st_mode)) {
		if (!scan_symlink)
			return 1;
		stat(filename, &symlink_st);
		st = &symlink_st;
	}

	if (!S_ISREG(st->st_mode)) {
		if (be_verbose > 2) printf("%s: skipping non-file\n", filename);
		return 1;
	}

	if (match_perms) {
		if ((st->st_mode | match_perms) != st->st_mode)
			return 1;
	}
	if ((fd = open(filename, O_RDONLY)) == -1)
		return 1;

	if (scanmacho_fat(filename, fd, st->st_size) == 1 && scan_archives)
		/* if it isn't an Mach-O, maybe it's an .a archive */
		scanmacho_archive(filename, fd, st->st_size);

	close(fd);
	return 0;
}

/* scan a directory for ET_EXEC files and print when we find one */
static int scanmacho_dir(const char *path)
{
	register DIR *dir;
	register struct dirent *dentry;
	struct stat st_top, st;
	char buf[__PAX_UTILS_PATH_MAX];
	size_t pathlen = 0, len = 0;
	int ret = 0;

	/* make sure path exists */
	if (lstat(path, &st_top) == -1) {
		if (be_verbose > 2) printf("%s: does not exist\n", path);
		return 1;
	}

	/* ok, if it isn't a directory, assume we can open it */
	if (!S_ISDIR(st_top.st_mode)) {
		return scanmacho_file(path, &st_top);
	}

	/* now scan the dir looking for fun stuff */
	if ((dir = opendir(path)) == NULL) {
		warnf("could not opendir %s: %s", path, strerror(errno));
		return 1;
	}
	if (be_verbose > 1) printf("%s: scanning dir\n", path);

	pathlen = strlen(path);
	while ((dentry = readdir(dir))) {
		if (!strcmp(dentry->d_name, ".") || !strcmp(dentry->d_name, ".."))
			continue;
		len = (pathlen + 1 + strlen(dentry->d_name) + 1);
		if (len >= sizeof(buf)) {
			warnf("Skipping '%s': len > sizeof(buf); %lu > %lu\n", path,
			      (unsigned long)len, (unsigned long)sizeof(buf));
			continue;
		}
		snprintf(buf, sizeof(buf), "%s%s%s", path, (path[pathlen-1] == '/') ? "" : "/", dentry->d_name);
		if (lstat(buf, &st) != -1) {
			if (S_ISREG(st.st_mode))
				ret = scanmacho_file(buf, &st);
			else if (dir_recurse && S_ISDIR(st.st_mode)) {
				if (dir_crossmount || (st_top.st_dev == st.st_dev))
					ret = scanmacho_dir(buf);
			}
		}
	}
	closedir(dir);
	return ret;
}

static int scanmacho_from_file(const char *filename)
{
	FILE *fp = NULL;
	char *p;
	char path[__PAX_UTILS_PATH_MAX];
	int ret = 0;

	if (strcmp(filename, "-") == 0)
		fp = stdin;
	else if ((fp = fopen(filename, "r")) == NULL)
		return 1;

	while ((fgets(path, __PAX_UTILS_PATH_MAX, fp)) != NULL) {
		if ((p = strchr(path, '\n')) != NULL)
			*p = 0;
		search_path = path;
		ret = scanmacho_dir(path);
	}
	if (fp != stdin)
		fclose(fp);
	return ret;
}


/* scan env PATH for paths */
static void scanelf_envpath(void)
{
	char *path, *p;

	path = getenv("PATH");
	if (!path)
		err("PATH is not set in your env !");
	path = xstrdup(path);

	while ((p = strrchr(path, ':')) != NULL) {
		scanmacho_dir(p + 1);
		*p = 0;
	}

	free(path);
}

/* usage / invocation handling functions */ /* Free Flags: c d e j k l r s t u w x z C G H I J K L P Q T U W X Y */
#define PARSE_FLAGS "pRmyAnibSN:gE:M:DO:ZaqvF:f:o:BhV"
#define a_argument required_argument
static struct option const long_opts[] = {
	{"path",      no_argument, NULL, 'p'},
	{"recursive", no_argument, NULL, 'R'},
	{"mount",     no_argument, NULL, 'm'},
	{"symlink",   no_argument, NULL, 'y'},
	{"archives",  no_argument, NULL, 'A'},
	{"needed",    no_argument, NULL, 'n'},
	{"interp",    no_argument, NULL, 'i'},
	{"bind",      no_argument, NULL, 'b'},
	{"soname",    no_argument, NULL, 'S'},
	{"lib",        a_argument, NULL, 'N'},
	{"gmatch",    no_argument, NULL, 'g'},
	{"etype",      a_argument, NULL, 'E'},
	{"bits",       a_argument, NULL, 'M'},
	{"endian",    no_argument, NULL, 'D'},
	{"perms",      a_argument, NULL, 'O'},
	{"size",      no_argument, NULL, 'Z'},
	{"all",       no_argument, NULL, 'a'},
	{"quiet",     no_argument, NULL, 'q'},
	{"verbose",   no_argument, NULL, 'v'},
	{"format",     a_argument, NULL, 'F'},
	{"from",       a_argument, NULL, 'f'},
	{"file",       a_argument, NULL, 'o'},
	{"nobanner",  no_argument, NULL, 'B'},
	{"help",      no_argument, NULL, 'h'},
	{"version",   no_argument, NULL, 'V'},
	{NULL,        no_argument, NULL, 0x0}
};

static const char *opts_help[] = {
	"Scan all directories in PATH environment",
	"Scan directories recursively",
	"Don't recursively cross mount points",
	"Don't scan symlinks",
	"Scan archives (.a files)",
	"Print LC_LOAD_DYLIB information (ELF: NEEDED)",
	"Print LC_LOAD_DYLINKER information (ELF: INTERP)",
	"Print flags from mach_header (ELF: BIND)",
	"Print LC_ID_DYLIB information (ELF: SONAME)",
	"Find a specified library",
	"Use strncmp to match libraries. (use with -N)",
	"Print only Mach-O files matching mach_header\n"
		"                        MH_OBJECT,MH_EXECUTE ... (ELF: etype)",
	"Print only Mach-O files matching numeric bits",
	"Print Endianness",
	"Print only Mach-O files matching octal permissions",
	"Print Mach-O file size",
	"Print all scanned info (-F\"%o %O %D %b %F\")\n",
	"Only output 'bad' things",
	"Be verbose (can be specified more than once)",
	"Use specified format for output",
	"Read input stream from a filename",
	"Write output stream to a filename",
	"Don't display the header",
	"Print this help and exit",
	"Print version and exit",
	NULL
};

/* display usage and exit */
static void usage(int status)
{
	unsigned long i;
	printf("* Scan Mach-O binaries for stuff\n\n"
	       "Usage: %s [options] <dir1/file1> [dir2 dirN file2 fileN ...]\n\n", argv0);
	printf("Options: -[%s]\n", PARSE_FLAGS);
	for (i = 0; long_opts[i].name; ++i)
		if (long_opts[i].has_arg == no_argument)
			printf("  -%c, --%-14s* %s\n", long_opts[i].val,
			       long_opts[i].name, opts_help[i]);
		else
			printf("  -%c, --%-7s <arg> * %s\n", long_opts[i].val,
			       long_opts[i].name, opts_help[i]);

	puts("\nFor more information, see the scanmacho(1) manpage");
	exit(status);
}

/* parse command line arguments and preform needed actions */
static int parseargs(int argc, char *argv[])
{
	int i;
	const char *from_file = NULL;
	int ret = 0;

	opterr = 0;
	while ((i = getopt_long(argc, argv, PARSE_FLAGS, long_opts, NULL)) != -1) {
		switch (i) {

		case 'V':
			printf("pax-utils-%s: %s compiled %s\n%s\n"
			       "%s written for Gentoo by <solar, vapier and grobian @ gentoo.org>\n",
			       VERSION, __FILE__, __DATE__, rcsid, argv0);
			exit(EXIT_SUCCESS);
			break;
		case 'h': usage(EXIT_SUCCESS); break;
		case 'f':
			if (from_file) warn("You prob don't want to specify -f twice");
			from_file = optarg;
			break;
		case 'E':
			strncpy(match_etypes, optarg, sizeof(match_etypes));
			break;
		case 'M':
			match_bits = atoi(optarg);
			break;
		case 'O':
			if (sscanf(optarg, "%o", &match_perms) == (-1))
				match_bits = 0;
			break;
		case 'o': {
			if (freopen(optarg, "w", stdout) == NULL)
				err("Could not open output stream '%s': %s", optarg, strerror(errno));
			break;
		}
		case 'N': {
			if (find_lib) warn("You prob don't want to specify -N twice");
			find_lib = optarg;
			break;
		}
		case 'F': {
			if (out_format) warn("You prob don't want to specify -F twice");
			out_format = optarg;
			break;
		}
		case 'Z': show_size = 1; break;
		case 'g': g_match = 1; break;
		case 'y': scan_symlink = 0; break;
		case 'A': scan_archives = 1; break;
		case 'B': show_banner = 0; break;
		case 'p': scan_envpath = 1; break;
		case 'R': dir_recurse = 1; break;
		case 'm': dir_crossmount = 0; break;
		case 'n': show_needed = 1; break;
		case 'i': show_interp = 1; break;
		case 'b': show_bind = 1; break;
		case 'S': show_soname = 1; break;
		case 'q': be_quiet = 1; break;
		case 'v': be_verbose = (be_verbose % 20) + 1; break;
		case 'a': show_perms = show_endian = show_bind = 1; break;
		case 'D': show_endian = 1; break;
		case ':':
			err("Option '%c' is missing parameter", optopt);
		case '?':
			err("Unknown option '%c' or argument missing", optopt);
		default:
			err("Unhandled option '%c'; please report this", i);
		}
	}
	/* let the format option override all other options */
	if (out_format) {
		show_needed = show_interp = show_bind = show_soname = \
		show_perms = show_endian = show_size = 0;
		for (i = 0; out_format[i]; ++i) {
			if (!IS_MODIFIER(out_format[i])) continue;

			switch (out_format[++i]) {
			case '+': break;
			case '%': break;
			case '#': break;
			case 'F': break;
			case 'p': break;
			case 'f': break;
			case 'k': break;
			case 'N': break;
			case 'o': break;
			case 'a': break;
			case 'M': break;
			case 'Z': show_size = 1; break;
			case 'D': show_endian = 1; break;
			case 'O': show_perms = 1; break;
			case 'n': show_needed = 1; break;
			case 'i': show_interp = 1; break;
			case 'b': show_bind = 1; break;
			case 'S': show_soname = 1; break;
			default:
				err("Invalid format specifier '%c' (byte %i)",
				    out_format[i], i+1);
			}
		}

	/* construct our default format */
	} else {
		size_t fmt_len = 30;
		out_format = xmalloc(sizeof(char) * fmt_len);
		*out_format = '\0';
		if (!be_quiet)     xstrcat(&out_format, "%o ", &fmt_len);
		if (show_perms)    xstrcat(&out_format, "%O ", &fmt_len);
		if (show_size)     xstrcat(&out_format, "%Z ", &fmt_len);
		if (show_endian)   xstrcat(&out_format, "%D ", &fmt_len);
		if (show_needed)   xstrcat(&out_format, "%n ", &fmt_len);
		if (show_interp)   xstrcat(&out_format, "%i ", &fmt_len);
		if (show_bind)     xstrcat(&out_format, "%b ", &fmt_len);
		if (show_soname)   xstrcat(&out_format, "%S ", &fmt_len);
		if (find_lib)      xstrcat(&out_format, "%N ", &fmt_len);
		if (!be_quiet)     xstrcat(&out_format, "%F ", &fmt_len);
	}
	if (be_verbose > 2) printf("Format: %s\n", out_format);

	/* now lets actually do the scanning */
	if (scan_envpath)
		scanelf_envpath();
	if (!from_file && optind == argc && ttyname(0) == NULL && !scan_envpath)
		from_file = "-";
	if (from_file) {
		scanmacho_from_file(from_file);
		from_file = *argv;
	}
	if (optind == argc && !scan_envpath && !from_file)
		err("Nothing to scan !?");
	while (optind < argc) {
		search_path = argv[optind++];
		ret = scanmacho_dir(search_path);
	}

	return ret;
}

int main(int argc, char *argv[])
{
	int ret;
	if (argc < 2)
		usage(EXIT_FAILURE);
	ret = parseargs(argc, argv);
	fclose(stdout);
	return ret;
}
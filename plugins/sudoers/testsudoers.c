/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 1996, 1998-2005, 2007-2023
 *	Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif /* HAVE_STDBOOL_H */
#include <string.h>
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#include <unistd.h>
#include <errno.h>

#include "testsudoers_pwutil.h"
#include "toke.h"
#include "tsgetgrpw.h"
#include "sudoers.h"
#include "interfaces.h"
#include "sudo_conf.h"
#include "sudo_lbuf.h"
#include <gram.h>

#ifndef YYDEBUG
# define YYDEBUG 0
#endif

enum sudoers_formats {
    format_ldif,
    format_sudoers
};

/*
 * Function Prototypes
 */
static void dump_sudoers(void);
static void set_runaspw(const char *);
static void set_runasgr(const char *);
static int testsudoers_error(const char * restrict buf);
static int testsudoers_output(const char * restrict buf);
sudo_noreturn static void usage(void);
static void cb_lookup(const struct sudoers_parse_tree *parse_tree, const struct userspec *us, int user_match, const struct privilege *priv, int host_match, const struct cmndspec *cs, int date_match, int runas_match, int cmnd_match, void *closure);
static int testsudoers_query(const struct sudo_nss *nss, struct passwd *pw);

/*
 * Globals
 */
struct sudoers_user_context user_ctx;
struct passwd *list_pw;
static const char *orig_cmnd;
static char *runas_group, *runas_user;
unsigned int sudo_mode = MODE_RUN;

#if defined(SUDO_DEVEL) && defined(__OpenBSD__)
extern char *malloc_options;
#endif

sudo_dso_public int main(int argc, char *argv[]);

int
main(int argc, char *argv[])
{
    struct sudoers_parser_config sudoers_conf = SUDOERS_PARSER_CONFIG_INITIALIZER;
    struct sudo_nss_list snl = TAILQ_HEAD_INITIALIZER(snl);
    enum sudoers_formats input_format = format_sudoers;
    struct sudo_nss testsudoers_nss;
    char *p, *grfile, *pwfile;
    const char *errstr;
    int ch, dflag, exitcode = EXIT_FAILURE;
    unsigned int validated;
    int status = FOUND;
    int pwflag = 0;
    char cwdbuf[PATH_MAX];
    time_t now;
    id_t id;
    debug_decl(main, SUDOERS_DEBUG_MAIN);

#if defined(SUDO_DEVEL) && defined(__OpenBSD__)
    malloc_options = "S";
#endif
#if YYDEBUG
    sudoersdebug = 1;
#endif

    initprogname(argc > 0 ? argv[0] : "testsudoers");

    if (!sudoers_initlocale(setlocale(LC_ALL, ""), def_sudoers_locale))
	sudo_fatalx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
    sudo_warn_set_locale_func(sudoers_warn_setlocale);
    bindtextdomain("sudoers", LOCALEDIR); /* XXX - should have own domain */
    textdomain("sudoers");
    time(&now);

    /* Initialize the debug subsystem. */
    if (sudo_conf_read(NULL, SUDO_CONF_DEBUG) == -1)
	goto done;
    if (!sudoers_debug_register(getprogname(), sudo_conf_debug_files(getprogname())))
	goto done;

    dflag = 0;
    grfile = pwfile = NULL;
    while ((ch = getopt(argc, argv, "+D:dg:G:h:i:L:lP:p:R:T:tu:U:v")) != -1) {
	switch (ch) {
	    case 'D':
		user_ctx.runcwd = optarg;
		break;
	    case 'd':
		dflag = 1;
		break;
	    case 'G':
		id = sudo_strtoid(optarg, &errstr);
		if (errstr != NULL)
		    sudo_fatalx("group-ID %s: %s", optarg, errstr);
		sudoers_conf.sudoers_gid = (gid_t)id;
		break;
	    case 'g':
		runas_group = optarg;
		SET(user_ctx.flags, RUNAS_GROUP_SPECIFIED);
		break;
	    case 'h':
		user_ctx.host = optarg;
		break;
	    case 'i':
		if (strcasecmp(optarg, "ldif") == 0) {
		    input_format = format_ldif;
		} else if (strcasecmp(optarg, "sudoers") == 0) {
		    input_format = format_sudoers;
		} else {
		    sudo_warnx(U_("unsupported input format %s"), optarg);
		    usage();
		}
		break;
	    case 'L':
		list_pw = sudo_getpwnam(optarg);
		if (list_pw == NULL) {
		    sudo_warnx(U_("unknown user %s"), optarg);
		    usage();
		}
		FALLTHROUGH;
	    case 'l':
		if (sudo_mode != MODE_RUN) {
		    sudo_warnx(
			"only one of the -l or -v flags may be specified");
		    usage();
		}
		sudo_mode = MODE_LIST;
		pwflag = I_LISTPW;
		orig_cmnd = "list";
		break;
	    case 'p':
		pwfile = optarg;
		break;
	    case 'P':
		grfile = optarg;
		break;
	    case 'T':
		now = parse_gentime(optarg);
		if (now == -1)
		    sudo_fatalx("invalid time: %s", optarg);
		break;
	    case 'R':
		user_ctx.runchroot = optarg;
		break;
	    case 't':
		trace_print = testsudoers_error;
		break;
	    case 'U':
		id = sudo_strtoid(optarg, &errstr);
		if (errstr != NULL)
		    sudo_fatalx("user-ID %s: %s", optarg, errstr);
		sudoers_conf.sudoers_uid = (uid_t)id;
		break;
	    case 'u':
		runas_user = optarg;
		SET(user_ctx.flags, RUNAS_USER_SPECIFIED);
		break;
	    case 'v':
		if (sudo_mode != MODE_RUN) {
		    sudo_warnx(
			"only one of the -l or -v flags may be specified");
		    usage();
		}
		sudo_mode = MODE_VALIDATE;
		pwflag = I_VERIFYPW;
		orig_cmnd = "validate";
		break;
	    default:
		usage();
		/* NOTREACHED */
	}
    }
    argc -= optind;
    argv += optind;

    if (grfile != NULL || pwfile != NULL) {
	/* Set group/passwd file and init the cache. */
	if (grfile)
	    testsudoers_setgrfile(grfile);
	if (pwfile)
	    testsudoers_setpwfile(pwfile);

	/* Use custom passwd/group backend. */
	sudo_pwutil_set_backend(testsudoers_make_pwitem,
	    testsudoers_make_gritem, testsudoers_make_gidlist_item,
	    testsudoers_make_grlist_item);
    }

    if (argc < 2) {
	/* No command or user specified. */
	if (dflag) {
	    orig_cmnd = "true";
	} else if (pwflag == 0) {
	    usage();
	}
	user_ctx.name = argc ? *argv++ : (char *)"root";
	argc = 0;
    } else {
	if (argc > 2 && sudo_mode == MODE_LIST)
	    sudo_mode = MODE_CHECK;
	user_ctx.name = *argv++;
	argc--;
	if (orig_cmnd == NULL) {
	    orig_cmnd = *argv++;
	    argc--;
	}
    }
    user_ctx.cmnd = strdup(orig_cmnd);
    if (user_ctx.cmnd == NULL)
	sudo_fatalx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
    user_ctx.cmnd_base = sudo_basename(user_ctx.cmnd);

    if (getcwd(cwdbuf, sizeof(cwdbuf)) == NULL)
	strlcpy(cwdbuf, "/", sizeof(cwdbuf));
    user_ctx.cwd = cwdbuf;

    if ((user_ctx.pw = sudo_getpwnam(user_ctx.name)) == NULL)
	sudo_fatalx(U_("unknown user %s"), user_ctx.name);
    user_ctx.uid = user_ctx.pw->pw_uid;
    user_ctx.gid = user_ctx.pw->pw_gid;

    if (user_ctx.host == NULL) {
	if ((user_ctx.host = sudo_gethostname()) == NULL)
	    sudo_fatal("gethostname");
    }
    if ((p = strchr(user_ctx.host, '.'))) {
	*p = '\0';
	if ((user_ctx.shost = strdup(user_ctx.host)) == NULL)
	    sudo_fatalx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	*p = '.';
    } else {
	user_ctx.shost = user_ctx.host;
    }
    user_ctx.runhost = user_ctx.host;
    user_ctx.srunhost = user_ctx.shost;

    /* Fill in user_ctx.cmnd_args from argv. */
    if (argc > 0) {
	char *to, **from;
	size_t size, n;

	for (size = 0, from = argv; *from; from++)
	    size += strlen(*from) + 1;

	if ((user_ctx.cmnd_args = malloc(size)) == NULL)
	    sudo_fatalx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	for (to = user_ctx.cmnd_args, from = argv; *from; from++) {
	    n = strlcpy(to, *from, size - (size_t)(to - user_ctx.cmnd_args));
	    if (n >= size - (size_t)(to - user_ctx.cmnd_args))
		sudo_fatalx(U_("internal error, %s overflow"), getprogname());
	    to += n;
	    *to++ = ' ';
	}
	*--to = '\0';
    }

    /* Initialize default values. */
    if (!init_defaults())
	sudo_fatalx("%s", U_("unable to initialize sudoers default values"));

    /* Set group_plugin callback. */
    sudo_defs_table[I_GROUP_PLUGIN].callback = cb_group_plugin;

    /* Set runas callback. */
    sudo_defs_table[I_RUNAS_DEFAULT].callback = cb_runas_default;

    /* Set locale callback. */
    sudo_defs_table[I_SUDOERS_LOCALE].callback = sudoers_locale_callback;

    /* Load ip addr/mask for each interface. */
    if (get_net_ifs(&p) > 0) {
	if (!set_interfaces(p))
	    sudo_fatal("%s", U_("unable to parse network address list"));
	free(p);
    }

    /* Initialize the parser and set sudoers filename to "sudoers". */
    sudoers_conf.strict = true;
    sudoers_conf.verbose = 2;
    init_parser("sudoers", &sudoers_conf);

    /*
     * Set runas passwd/group entries based on command line or sudoers.
     * Note that if runas_group was specified without runas_user we
     * run the command as the invoking user.
     */
    if (runas_group != NULL) {
        set_runasgr(runas_group);
        set_runaspw(runas_user ? runas_user : user_ctx.name);
    } else
        set_runaspw(runas_user ? runas_user : def_runas_default);

    /* Parse the policy file. */
    sudoers_setlocale(SUDOERS_LOCALE_SUDOERS, NULL);
    switch (input_format) {
    case format_ldif:
        if (!sudoers_parse_ldif(&parsed_policy, stdin, NULL, true)) {
	    (void) puts("Parse error in LDIF");
	    parse_error = true;
	}
        break;
    case format_sudoers:
	if (sudoersparse() != 0)
	    parse_error = true;
        break;
    default:
        sudo_fatalx("error: unhandled input %d", input_format);
    }
    if (!update_defaults(&parsed_policy, NULL, SETDEF_ALL, false))
	parse_error = true;

    if (!parse_error)
	(void) puts("Parses OK");

    if (dflag) {
	(void) putchar('\n');
	dump_sudoers();
	if (argc < 2) {
	    exitcode = parse_error ? 1 : 0;
	    goto done;
	}
    }

    /* Fake up a minimal sudo nss list with the parsed policy. */
    TAILQ_INSERT_TAIL(&snl, &testsudoers_nss, entries);
    testsudoers_nss.query = testsudoers_query;
    testsudoers_nss.parse_tree = &parsed_policy;

    printf("\nEntries for user %s:\n", user_ctx.name);
    validated = sudoers_lookup(&snl, user_ctx.pw, now, cb_lookup, NULL,
	&status, pwflag);

    /* Validate user-specified chroot or cwd (if any) and runas user shell. */
    if (ISSET(validated, VALIDATE_SUCCESS)) {
	if (!check_user_shell(user_ctx.runas_pw)) {
	    printf(U_("\nInvalid shell for user %s: %s\n"),
		user_ctx.runas_pw->pw_name, user_ctx.runas_pw->pw_shell);
	    CLR(validated, VALIDATE_SUCCESS);
	    SET(validated, VALIDATE_FAILURE);
	}
	if (check_user_runchroot() != true) {
	    printf("\nUser %s is not allowed to change root directory to %s\n",
		user_ctx.name, user_ctx.runchroot);
	    CLR(validated, VALIDATE_SUCCESS);
	    SET(validated, VALIDATE_FAILURE);
	}
	if (check_user_runcwd() != true) {
	    printf("\nUser %s is not allowed to change directory to %s\n",
		user_ctx.name, user_ctx.runcwd);
	    CLR(validated, VALIDATE_SUCCESS);
	    SET(validated, VALIDATE_FAILURE);
	}
    }
    if (def_authenticate) {
	puts(U_("\nPassword required"));
    }

    /*
     * Exit codes:
     *	0 - parsed OK and command matched.
     *	1 - parse error
     *	2 - command not matched
     *	3 - command denied
     */
    if (parse_error || ISSET(validated, VALIDATE_ERROR)) {
	puts(U_("\nParse error"));
	exitcode = 1;
    } else if (ISSET(validated, VALIDATE_SUCCESS)) {
	puts(U_("\nCommand allowed"));
	exitcode = 0;
    } else if (ISSET(validated, VALIDATE_FAILURE)) {
	puts(U_("\nCommand denied"));
	exitcode = 3;
    } else {
	puts(U_("\nCommand unmatched"));
	exitcode = 2;
    }

done:
    sudo_freepwcache();
    sudo_freegrcache();
    sudo_debug_exit_int(__func__, __FILE__, __LINE__, sudo_debug_subsys, exitcode);
    return exitcode;
}

static void
set_runaspw(const char *user)
{
    struct passwd *pw = NULL;
    debug_decl(set_runaspw, SUDOERS_DEBUG_UTIL);

    if (*user == '#') {
	const char *errstr;
	uid_t uid = sudo_strtoid(user + 1, &errstr);
	if (errstr == NULL) {
	    if ((pw = sudo_getpwuid(uid)) == NULL)
		pw = sudo_fakepwnam(user, user_ctx.gid);
	}
    }
    if (pw == NULL) {
	if ((pw = sudo_getpwnam(user)) == NULL)
	    sudo_fatalx(U_("unknown user %s"), user);
    }
    if (user_ctx.runas_pw != NULL)
	sudo_pw_delref(user_ctx.runas_pw);
    user_ctx.runas_pw = pw;
    debug_return;
}

static void
set_runasgr(const char *group)
{
    struct group *gr = NULL;
    debug_decl(set_runasgr, SUDOERS_DEBUG_UTIL);

    if (*group == '#') {
	const char *errstr;
	gid_t gid = sudo_strtoid(group + 1, &errstr);
	if (errstr == NULL) {
	    if ((gr = sudo_getgrgid(gid)) == NULL)
		gr = sudo_fakegrnam(group);
	}
    }
    if (gr == NULL) {
	if ((gr = sudo_getgrnam(group)) == NULL)
	    sudo_fatalx(U_("unknown group %s"), group);
    }
    if (user_ctx.runas_gr != NULL)
	sudo_gr_delref(user_ctx.runas_gr);
    user_ctx.runas_gr = gr;
    debug_return;
}

bool
cb_log_input(const char *file, int line, int column,
    const union sudo_defs_val *sd_un, int op)
{
    return true;
}

bool
cb_log_output(const char *file, int line, int column,
    const union sudo_defs_val *sd_un, int op)
{
    return true;
}

/* 
 * Callback for runas_default sudoers setting.
 */
bool
cb_runas_default(const char *file, int line, int column,
    const union sudo_defs_val *sd_un, int op)
{
    /* Only reset runaspw if user didn't specify one. */
    if (!runas_user && !runas_group)
        set_runaspw(sd_un->str);
    return true;
}

bool
sudo_nss_can_continue(const struct sudo_nss *nss, int match)
{
    return true;
}

void
sudo_setspent(void)
{
    return;
}

void
sudo_endspent(void)
{
    return;
}

FILE *
open_sudoers(const char *file, char **outfile, bool doedit, bool *keepopen)
{
    struct stat sb;
    FILE *fp = NULL;
    const char *base;
    int error, fd;
    debug_decl(open_sudoers, SUDOERS_DEBUG_UTIL);

    /* Report errors using the basename for consistent test output. */
    base = sudo_basename(file);
    fd = sudo_secure_open_file(file, sudoers_file_uid(), sudoers_file_gid(),
	&sb, &error);
    if (fd != -1) {
	if ((fp = fdopen(fd, "r")) == NULL) {
	    sudo_warn("unable to open %s", base);
	    close(fd);
	}
    } else {
	switch (error) {
	case SUDO_PATH_MISSING:
	    sudo_warn("unable to open %s", base);
	    break;
	case SUDO_PATH_BAD_TYPE:
	    sudo_warnx("%s is not a regular file", base);
	    break;
	case SUDO_PATH_WRONG_OWNER:
	    sudo_warnx("%s should be owned by uid %u",
		base, (unsigned int) sudoers_file_uid());
	    break;
	case SUDO_PATH_WORLD_WRITABLE:
	    sudo_warnx("%s is world writable", base);
	    break;
	case SUDO_PATH_GROUP_WRITABLE:
	    sudo_warnx("%s should be owned by gid %u",
		base, (unsigned int) sudoers_file_gid());
	    break;
	default:
	    sudo_warnx("%s: internal error, unexpected error %d",
		__func__, error);
	    break;
	}
    }

    debug_return_ptr(fp);
}

bool
init_envtables(void)
{
    return(true);
}

bool
set_perms(int perm)
{
    return true;
}

bool
restore_perms(void)
{
    return true;
}

void
init_eventlog_config(void)
{
    return;
}

bool
pivot_root(const char *new_root, int fds[2])
{
    return true;
}

bool
unpivot_root(int fds[2])
{
    return true;
}

int
set_cmnd_path(const char *runchroot)
{
    /* Reallocate user_ctx.cmnd to catch bugs in command_matches(). */
    char *new_cmnd = strdup(orig_cmnd);
    if (new_cmnd == NULL)
	return NOT_FOUND_ERROR;
    free(user_ctx.cmnd);
    user_ctx.cmnd = new_cmnd;
    return FOUND;
}

static void
cb_lookup(const struct sudoers_parse_tree *parse_tree,
    const struct userspec *us, int user_match, const struct privilege *priv,
    int host_match, const struct cmndspec *cs, int date_match, int runas_match,
    int cmnd_match, void *closure)
{
    static const struct privilege *prev_priv;
    struct sudo_lbuf lbuf;

    /* Only output info for the selected user. */
    if (user_match != ALLOW) {
	prev_priv = NULL;
	return;
    }

    if (priv != prev_priv) {
	/* No word wrap on output. */
	sudo_lbuf_init(&lbuf, testsudoers_output, 0, NULL, 0);
	sudo_lbuf_append(&lbuf, "\n");
	sudoers_format_privilege(&lbuf, &parsed_policy, priv, false);
	sudo_lbuf_print(&lbuf);
	sudo_lbuf_destroy(&lbuf);

	printf("\thost  %s\n", host_match == ALLOW ? "allowed" :
	    host_match == DENY ? "denied" : "unmatched");
    }

    if (host_match == ALLOW) {
	if (date_match != UNSPEC)
	    printf("\tdate  %s\n", date_match == ALLOW ? "allowed" : "denied");
	if (date_match != DENY) {
	    printf("\trunas %s\n", runas_match == ALLOW ? "allowed" :
		runas_match == DENY ? "denied" : "unmatched");
	    if (runas_match == ALLOW) {
		printf("\tcmnd  %s\n", cmnd_match == ALLOW ? "allowed" :
		    cmnd_match == DENY ? "denied" : "unmatched");
	    }
	}
    }

    prev_priv = priv;
}

static int
testsudoers_query(const struct sudo_nss *nss, struct passwd *pw)
{
    /* Nothing to do. */
    return 0;
}

static bool
print_defaults(struct sudo_lbuf *lbuf)
{
    struct defaults *def, *next;
    debug_decl(print_defaults, SUDOERS_DEBUG_UTIL);

    TAILQ_FOREACH_SAFE(def, &parsed_policy.defaults, entries, next)
	sudoers_format_default_line(lbuf, &parsed_policy, def, &next, false);

    debug_return_bool(!sudo_lbuf_error(lbuf));
}

static int
print_alias(struct sudoers_parse_tree *parse_tree, struct alias *a, void *v)
{
    struct sudo_lbuf *lbuf = v;
    struct member *m;
    debug_decl(print_alias, SUDOERS_DEBUG_UTIL);

    sudo_lbuf_append(lbuf, "%s %s = ", alias_type_to_string(a->type),
	a->name);
    TAILQ_FOREACH(m, &a->members, entries) {
	if (m != TAILQ_FIRST(&a->members))
	    sudo_lbuf_append(lbuf, ", ");
	sudoers_format_member(lbuf, parse_tree, m, NULL, UNSPEC);
    }
    sudo_lbuf_append(lbuf, "\n");

    debug_return_int(sudo_lbuf_error(lbuf) ? -1 : 0);
}

static bool
print_aliases(struct sudo_lbuf *lbuf)
{
    debug_decl(print_aliases, SUDOERS_DEBUG_UTIL);

    alias_apply(&parsed_policy, print_alias, lbuf);

    debug_return_bool(!sudo_lbuf_error(lbuf));
}

static void
dump_sudoers(void)
{
    struct sudo_lbuf lbuf;
    debug_decl(dump_sudoers, SUDOERS_DEBUG_UTIL);

    /* No word wrap on output. */
    sudo_lbuf_init(&lbuf, testsudoers_output, 0, NULL, 0);

    /* Print Defaults */
    if (!print_defaults(&lbuf))
	goto done;
    if (lbuf.len > 0) {
	sudo_lbuf_print(&lbuf);
	sudo_lbuf_append(&lbuf, "\n");
    }

    /* Print Aliases */
    if (!print_aliases(&lbuf))
	goto done;
    if (lbuf.len > 1) {
	sudo_lbuf_print(&lbuf);
	sudo_lbuf_append(&lbuf, "\n");
    }

    /* Print User_Specs */
    if (!sudoers_format_userspecs(&lbuf, &parsed_policy, NULL, false, true))
	goto done;
    if (lbuf.len > 1) {
	sudo_lbuf_print(&lbuf);
    }

done:
    if (sudo_lbuf_error(&lbuf)) {
	if (errno == ENOMEM)
	    sudo_fatalx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
    }
    sudo_lbuf_destroy(&lbuf);

    debug_return;
}

static int
testsudoers_output(const char * restrict buf)
{
    return fputs(buf, stdout);
}

static int
testsudoers_error(const char *restrict buf)
{
    return fputs(buf, stderr);
}

sudo_noreturn static void
usage(void)
{
    (void) fprintf(stderr, "usage: %s [-dltv] [-G sudoers_gid] [-g group] [-h host] [-i input_format] [-L list_user] [-P grfile] [-p pwfile] [-U sudoers_uid] [-u user] <user> <command> [args]\n", getprogname());
    exit(EXIT_FAILURE);
}

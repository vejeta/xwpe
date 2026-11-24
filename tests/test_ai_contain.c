/* test_ai_contain.c -- unit test for how the assistant's children are held
 * (we_contain.c with the owner of children, we_child.c).
 *
 * Links we_contain.c, we_child.c and the fd loop only; no editor.  It covers
 * the environment each profile gives its child (what is dropped, what an
 * engine keeps, what the user's lists add, the test seam and what it may not
 * name), the PATH and the program resolved in the parent (an empty, "." or
 * relative entry, an engine's PATH inside the workspace), the file-size
 * limit's text, the report, the header word and the notice, the refusal of an
 * assistant start that names no profile and of every start under Strict, and
 * real children: what a command run through a profile sees in its
 * environment, its file-size limit, its core limit and its no_new_privs flag.
 *
 * The folders it makes are under TMPDIR, removed by exact path.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#if HAVE_DECL_PROC_NO_NEW_PRIVS_CTL
# include <sys/procctl.h>
#endif

#include "../we_child.h"
#include "../we_contain.h"
#include "../we_fdloop.h"
#include "test_watchdog.h"

#define HANG_SECONDS    30
#define OUT_MAX         8192
#define LINE_MAX_TRACE  160
#define END_WAIT_MS     test_scaled_ms(10000)
/* The limit a child gets by default: the 8 GiB default, or the hard limit this
   test inherited when that is lower (the rows also run under prlimit). */
static unsigned long long expected_default_fsize(void)
{
	struct rlimit now;
	unsigned long long want = WPE_CONTAIN_FSIZE_DEFAULT;

	if (getrlimit(RLIMIT_FSIZE, &now) == 0 && now.rlim_max != RLIM_INFINITY &&
	    (unsigned long long)now.rlim_max < want)
		want = (unsigned long long)now.rlim_max;
	return want;
}
/* Does the probe's output show the file-size limit this test itself runs
   under (unlimited, or the soft limit in 512- or 1024-byte blocks)? */
static int inherited_fsize_shown(const char *out)
{
	struct rlimit now;
	char want[64];

	if (getrlimit(RLIMIT_FSIZE, &now) != 0)
		return 0;
	if (now.rlim_cur == RLIM_INFINITY)
		return strstr(out, "FSIZE=unlimited") != NULL;
	snprintf(want, sizeof want, "FSIZE=%llu", (unsigned long long)now.rlim_cur / 512ULL);
	if (strstr(out, want))
		return 1;
	snprintf(want, sizeof want, "FSIZE=%llu", (unsigned long long)now.rlim_cur / 1024ULL);
	return strstr(out, want) != NULL;
}
#define FSIZE_512_BLOCKS (expected_default_fsize() / 512ULL)
#define FSIZE_1K_BLOCKS  (expected_default_fsize() / 1024ULL)
#define MARKER           "xwpe-contain-marker-7f3a"
/* The program a workspace plants: a name no machine has on its PATH. */
#define PLANTED_NAME     "xwpe-planted-engine"
/* The program the engine-note rows plant in the workspace's own bin folder. */
#define NOTE_NAME        "xwpe-note-engine"
/* A hard file-size limit below the 8 GiB default: 1 GB, a multiple of 1024 and 512. */
#define LOW_HARD_FSIZE   (1000000000UL / 1024UL * 1024UL)
/* The option of this program that prints the no-new-privileges state of the
   process that runs it (where the system has it by a call, not by a file). */
#define NNP_STATUS_OPTION "--no-new-privs-status"
#define PROBE_SCRIPT_MAX  (2 * PATH_MAX + 512)

/* What the child prints: the environment, then its limits, one per line, then
   the line "NoNewPrivs:<tab>0|1|none" (see nnp_probe_part). */
static char probe_script[PROBE_SCRIPT_MAX];
#define PROBE_SCRIPT probe_script

static int fails;

/**
 * print_no_new_privs_status - The helper mode: say whether this process can gain privileges.
 *
 * Run as the contained child.  FreeBSD keeps the state in the kernel, read by
 * procctl, and has no file that shows it.  Prints the one line
 * "NoNewPrivs:<tab>1" or "NoNewPrivs:<tab>0", the same line Linux's
 * /proc/self/status has.
 * Return: the program's exit status.
 */
static int print_no_new_privs_status(void)
{
#if HAVE_DECL_PROC_NO_NEW_PRIVS_CTL
	int status = 0;

	if (procctl(P_PID, 0, PROC_NO_NEW_PRIVS_STATUS, &status) != 0)
		return 1;
	printf("NoNewPrivs:\t%d\n", status == PROC_NO_NEW_PRIVS_ENABLE);
	return 0;
#else
	return 1;
#endif
}

/**
 * nnp_probe_part - The part of the probe script that prints the NoNewPrivs line.
 * @self: this program's own path, for the helper mode.
 *
 * Where the system gives the state by a call (HAVE_DECL_PROC_NO_NEW_PRIVS_CTL)
 * the child runs this program in the helper mode.  Elsewhere it reads the
 * Linux line of /proc/self/status and says "none" when the system has no such
 * line (a tab written by printf, not a raw tab in the script).
 */
static void nnp_probe_part(char *part, size_t n, const char *self)
{
#if HAVE_DECL_PROC_NO_NEW_PRIVS_CTL
	snprintf(part, n, "'%s' " NNP_STATUS_OPTION, self);
#else
	(void)self;
	snprintf(part, n, "if grep '^NoNewPrivs' /proc/self/status 2>/dev/null; then :; "
	                  "else printf 'NoNewPrivs:\\tnone\\n'; fi");
#endif
}

/** build_probe_script - Fill probe_script: the environment, the limits, the NoNewPrivs line. */
static void build_probe_script(const char *self)
{
	char part[PROBE_SCRIPT_MAX / 2];

	nnp_probe_part(part, sizeof part, self);
	snprintf(probe_script, sizeof probe_script,
	         "env; echo FSIZE=$(ulimit -f); echo CORE=$(ulimit -c); %s", part);
}

/**
 * nnp_state - What the probe's NoNewPrivs line says.
 * Return: 1 (cannot gain privileges), 0 (can), -1 (no such line: the system
 * shows no state to read, so the rows that need it say SKIP).
 */
static int nnp_state(const char *out)
{
	if (strstr(out, "NoNewPrivs:\t1") != NULL)
		return 1;
	if (strstr(out, "NoNewPrivs:\t0") != NULL)
		return 0;
	return -1;
}

/**
 * nnp_rows_run - Can the NoNewPrivs rows assert on this system?
 * @row: the row's name, for the SKIP line.  @out: what the probe printed.
 *
 * Not where the product has no such protection (WPE_HAS_NO_NEW_PRIVS is 0:
 * NetBSD, OpenBSD), and not where the probe found no state to read.  Each says
 * SKIP and names the fact.
 */
static int nnp_rows_run(const char *row, const char *out)
{
#if WPE_HAS_NO_NEW_PRIVS
	if (nnp_state(out) < 0) {
		printf("SKIP: %s: the child printed no NoNewPrivs line (no /proc/self/status line, "
		       "and no procctl status here)\n", row);
		return 0;
	}
	return 1;
#else
	(void)out;
	printf("SKIP: %s: this system has no no-new-privileges protection (WPE_HAS_NO_NEW_PRIVS is 0)\n", row);
	return 0;
#endif
}

static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static void chk_true(const char *what, int cond)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

/* ---- a vector the rows give the builder ----------------------------------- */

static char *has(char **vec, const char *name)
{
	size_t len = strlen(name);
	int i;

	for (i = 0; vec && vec[i]; i++)
		if (strncmp(vec[i], name, len) == 0 && vec[i][len] == '=')
			return vec[i] + len + 1;
	return NULL;
}

static char *const polluted[] = {
	"HOME=/home/someone", "LANG=C", "LC_ALL=C", "PATH=/usr/bin:/bin", "TERM=xterm",
	"TZ=UTC", "TMPDIR=/var/tmp", "USER=someone", "LOGNAME=someone",
	"JAVA_HOME=/opt/jdk", "GOPATH=/home/someone/go",
	"ANTHROPIC_API_KEY=" MARKER, "ANTHROPIC_BASE_URL=http://127.0.0.1:1",
	"ANTHROPIC_API_KEY_FILE=/home/someone/key", "ANTHROPIC_AUTH_TOKEN=tok",
	"CLAUDE_CODE_FOO=1", "OPENAI_API_KEY=" MARKER, "OPENAI_BASE_URL=http://127.0.0.1:2",
	"OPENAI_MODEL=m", "XWPE_AI_KEY=" MARKER, "SSH_AUTH_SOCK=/run/agent",
	"GPG_AGENT_INFO=/run/gpg", "DISPLAY=:0", "WAYLAND_DISPLAY=wayland-0",
	"XAUTHORITY=/home/someone/.Xauthority", "XDG_RUNTIME_DIR=/run/user/1000",
	"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/bus", "DOCKER_HOST=unix:///run/docker.sock",
	"LD_PRELOAD=/tmp/evil.so", "LD_LIBRARY_PATH=/opt/lib", "BASH_ENV=/tmp/evil.sh",
	"ENV=/tmp/evil.sh", "GIT_DIR=/elsewhere", "GIT_SSH_COMMAND=evil",
	"GIT_AUTHOR_NAME=a", "GIT_COMMITTER_EMAIL=a@x", "HTTPS_PROXY=http://127.0.0.1:3",
	"MY_ENGINE_TOKEN=secret", "XWPE_STUB_LOG=/tmp/log", "UNRELATED=1",
	"AWS_SECRET_ACCESS_KEY=" MARKER, "GH_TOKEN=" MARKER, NULL
};

static void clear_settings(void)
{
	unsetenv(WPE_CONTAIN_ENV_PASS);
	unsetenv(WPE_CONTAIN_ENV_PATH_OK);
	unsetenv(WPE_CONTAIN_ENV_MODE);
	unsetenv(WPE_CONTAIN_ENV_KEEP);
	unsetenv(WPE_CONTAIN_ENV_HOST);
	unsetenv(WPE_CONTAIN_ENV_FSIZE);
	wpe_contain_mode_set(WPE_CONTAIN_MODE_STANDARD);
	wpe_contain_keep_names_set(NULL);
	wpe_contain_host_names_set(NULL);
	wpe_contain_notice_reset();
}

static void check_dropped(const char *who, char **v, const char *const *names)
{
	char what[160];
	int i;

	for (i = 0; names[i]; i++) {
		snprintf(what, sizeof what, "%s drops %s", who, names[i]);
		chk_true(what, has(v, names[i]) == NULL);
	}
}

/* @scenario SC-ai-contain-01 */
static void test_a_command_gets_a_built_environment_not_the_editors(void)
{
	static const char *const dropped[] = {
		"ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "ANTHROPIC_API_KEY_FILE",
		"ANTHROPIC_AUTH_TOKEN", "CLAUDE_CODE_FOO", "OPENAI_API_KEY",
		"OPENAI_BASE_URL", "OPENAI_MODEL", "XWPE_AI_KEY", "SSH_AUTH_SOCK",
		"GPG_AGENT_INFO", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY",
		"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "DOCKER_HOST",
		"LD_PRELOAD", "LD_LIBRARY_PATH", "BASH_ENV", "ENV", "GIT_DIR",
		"GIT_SSH_COMMAND", "MY_ENGINE_TOKEN", "XWPE_STUB_LOG", "UNRELATED",
		"HTTPS_PROXY", "AWS_SECRET_ACCESS_KEY", "GH_TOKEN", NULL
	};
	static const WpeContainProfile *const plain[] = {
		&wpe_contain_tool, &wpe_contain_hook, &wpe_contain_git,
		&wpe_contain_git_commit, NULL
	};
	int i;

	clear_settings();
	for (i = 0; plain[i]; i++) {
		char **v = wpe_contain_env_make(plain[i], polluted, "/nonexistent-ws");

		chk_true("the vector is made", v != NULL);
		if (!v)
			continue;
		check_dropped(plain[i]->name, v, dropped);
		chk_true("HOME is kept", has(v, "HOME") && !strcmp(has(v, "HOME"), "/home/someone"));
		chk_true("LANG is kept", has(v, "LANG") != NULL);
		chk_true("LC_ALL is kept", has(v, "LC_ALL") != NULL);
		chk_true("TZ is kept", has(v, "TZ") != NULL);
		chk_true("TMPDIR is kept", has(v, "TMPDIR") != NULL);
		chk_true("USER is kept", has(v, "USER") != NULL);
		chk_true("a toolchain name is kept", has(v, "JAVA_HOME") != NULL);
		chk_true("GOPATH is kept", has(v, "GOPATH") != NULL);
		chk_true("git's author is kept", has(v, "GIT_AUTHOR_NAME") != NULL);
		chk_true("git's committer is kept", has(v, "GIT_COMMITTER_EMAIL") != NULL);
		chk_true("TERM is dumb", has(v, "TERM") && !strcmp(has(v, "TERM"), "dumb"));
		chk_true("PATH is there", has(v, "PATH") != NULL);
		wpe_contain_env_free(v);
	}
}

/* @scenario SC-ai-contain-02 */
static void test_an_engine_keeps_its_own_login_and_no_other(void)
{
	char **c = wpe_contain_env_make(&wpe_contain_cli_text, polluted, "/nonexistent-ws");
	char **h = wpe_contain_env_make(&wpe_contain_host_claude, polluted, "/nonexistent-ws");
	char **o = wpe_contain_env_make(&wpe_contain_host_command, polluted, "/nonexistent-ws");
	int i;
	char **both[2];

	clear_settings();
	both[0] = c;
	both[1] = h;
	for (i = 0; i < 2; i++) {
		char **v = both[i];

		chk_true("the claude engine keeps ANTHROPIC_API_KEY", has(v, "ANTHROPIC_API_KEY") != NULL);
		chk_true("and ANTHROPIC_BASE_URL", has(v, "ANTHROPIC_BASE_URL") != NULL);
		chk_true("and ANTHROPIC_AUTH_TOKEN", has(v, "ANTHROPIC_AUTH_TOKEN") != NULL);
		chk_true("and CLAUDE_CODE_*", has(v, "CLAUDE_CODE_FOO") != NULL);
		chk_true("and the way out (HTTPS_PROXY)", has(v, "HTTPS_PROXY") != NULL);
		chk_true("but not the editor's pointer to a key file", has(v, "ANTHROPIC_API_KEY_FILE") == NULL);
		chk_true("nor another provider's key", has(v, "OPENAI_API_KEY") == NULL);
		chk_true("nor the agent socket", has(v, "SSH_AUTH_SOCK") == NULL);
		chk_true("nor the desktop", has(v, "DISPLAY") == NULL);
		chk_true("nor the loader's preload", has(v, "LD_PRELOAD") == NULL);
		chk_true("nor the editor's key", has(v, "XWPE_AI_KEY") == NULL);
	}
	chk_true("the user's own command keeps OPENAI_API_KEY", has(o, "OPENAI_API_KEY") != NULL);
	chk_true("and OPENAI_BASE_URL", has(o, "OPENAI_BASE_URL") != NULL);
	chk_true("and OPENAI_MODEL", has(o, "OPENAI_MODEL") != NULL);
	chk_true("but not Anthropic's key", has(o, "ANTHROPIC_API_KEY") == NULL);
	chk_true("and no name AIHostEnv does not list", has(o, "MY_ENGINE_TOKEN") == NULL);
	wpe_contain_env_free(c);
	wpe_contain_env_free(h);
	wpe_contain_env_free(o);
}

/* @scenario SC-ai-contain-03 */
static void test_the_users_lists_add_names_but_never_a_key_or_a_preload(void)
{
	char **o, **t;

	clear_settings();
	wpe_contain_host_names_set("MY_ENGINE_TOKEN");
	o = wpe_contain_env_make(&wpe_contain_host_command, polluted, "/nonexistent-ws");
	t = wpe_contain_env_make(&wpe_contain_tool, polluted, "/nonexistent-ws");
	chk_true("AIHostEnv adds a name for the user's command", has(o, "MY_ENGINE_TOKEN") != NULL);
	chk_true("and for no other profile", has(t, "MY_ENGINE_TOKEN") == NULL);
	wpe_contain_env_free(o);
	wpe_contain_env_free(t);
	wpe_contain_host_names_set(NULL);

	wpe_contain_keep_names_set("UNRELATED,LD_LIBRARY_PATH,ANTHROPIC_API_KEY,LD_PRELOAD,SSH_AUTH_SOCK");
	t = wpe_contain_env_make(&wpe_contain_tool, polluted, "/nonexistent-ws");
	chk_true("AIContainEnv adds a name to every profile", has(t, "UNRELATED") != NULL);
	chk_true("LD_LIBRARY_PATH is kept when named", has(t, "LD_LIBRARY_PATH") != NULL);
	chk_true("a provider key named there is still dropped", has(t, "ANTHROPIC_API_KEY") == NULL);
	chk_true("LD_PRELOAD named there is still dropped", has(t, "LD_PRELOAD") == NULL);
	chk_true("the agent socket named there is still dropped", has(t, "SSH_AUTH_SOCK") == NULL);
	wpe_contain_env_free(t);
	wpe_contain_keep_names_set(NULL);

	setenv(WPE_CONTAIN_ENV_KEEP, "UNRELATED", 1);
	t = wpe_contain_env_make(&wpe_contain_hook, polluted, "/nonexistent-ws");
	chk_true("the environment shadow of AIContainEnv works", has(t, "UNRELATED") != NULL);
	wpe_contain_env_free(t);
	clear_settings();
}

/* @scenario SC-ai-contain-04 */
static void test_only_a_commit_signed_with_ssh_keeps_the_agent_socket(void)
{
	char **s, **c;

	clear_settings();
	s = wpe_contain_env_make(&wpe_contain_git_commit_ssh, polluted, "/nonexistent-ws");
	c = wpe_contain_env_make(&wpe_contain_git_commit, polluted, "/nonexistent-ws");
	chk_true("a commit signed with ssh keeps SSH_AUTH_SOCK", has(s, "SSH_AUTH_SOCK") != NULL);
	chk_true("any other commit does not", has(c, "SSH_AUTH_SOCK") == NULL);
	chk_true("the signing commit still drops the desktop", has(s, "DISPLAY") == NULL);
	chk_true("and the keys", has(s, "ANTHROPIC_API_KEY") == NULL);
	wpe_contain_env_free(s);
	wpe_contain_env_free(c);
}

/* @scenario SC-ai-contain-05 */
static void test_the_test_seam_keeps_a_prefix_but_never_one_that_would_pass_a_key(void)
{
	WpeContainReport r;
	char **v;

	clear_settings();
	setenv(WPE_CONTAIN_ENV_PASS, "XWPE_STUB_", 1);
	v = wpe_contain_env_make(&wpe_contain_tool, polluted, "/nonexistent-ws");
	chk_true("a stub prefix is kept", has(v, "XWPE_STUB_LOG") != NULL);
	chk_true("nothing else comes with it", has(v, "UNRELATED") == NULL);
	wpe_contain_env_free(v);
	wpe_contain_report(&wpe_contain_tool, &r);
	chk_true("the report names the prefix", !strcmp(r.pass, "XWPE_STUB_"));
	chk_true("and refuses none", r.refused[0] == '\0');

	setenv(WPE_CONTAIN_ENV_PASS, "ANTHROPIC_,OPENAI_,SSH_,DISPLAY,LD_,DBUS_,XWPE_,GIT_,XDG_RUNTIME,WAYLAND_", 1);
	v = wpe_contain_env_make(&wpe_contain_tool, polluted, "/nonexistent-ws");
	chk_true("the seam cannot pass the Anthropic key", has(v, "ANTHROPIC_API_KEY") == NULL);
	chk_true("nor the OpenAI key", has(v, "OPENAI_API_KEY") == NULL);
	chk_true("nor the agent socket", has(v, "SSH_AUTH_SOCK") == NULL);
	chk_true("nor the display", has(v, "DISPLAY") == NULL);
	chk_true("nor the preload", has(v, "LD_PRELOAD") == NULL);
	chk_true("nor the bus", has(v, "DBUS_SESSION_BUS_ADDRESS") == NULL);
	chk_true("nor git's command variables", has(v, "GIT_SSH_COMMAND") == NULL);
	chk_true("nor a prefix that covers the editor's key", has(v, "XWPE_AI_KEY") == NULL);
	chk_true("nor the XDG runtime folder", has(v, "XDG_RUNTIME_DIR") == NULL);
	wpe_contain_env_free(v);
	wpe_contain_report(&wpe_contain_tool, &r);
	chk_true("the report says what it refused", strstr(r.refused, "ANTHROPIC_") != NULL);
	chk_true("and keeps none of them", r.pass[0] == '\0');

	setenv(WPE_CONTAIN_ENV_PASS, "AWS_,GH_,UNRELATED,XWPE_STU", 1);
	v = wpe_contain_env_make(&wpe_contain_tool, polluted, "/nonexistent-ws");
	chk_true("the seam cannot pass a cloud key", has(v, "AWS_SECRET_ACCESS_KEY") == NULL);
	chk_true("nor a token", has(v, "GH_TOKEN") == NULL);
	chk_true("nor any name outside the stub prefix", has(v, "UNRELATED") == NULL);
	chk_true("nor a prefix that is only part of the stub prefix", has(v, "XWPE_STUB_LOG") == NULL);
	wpe_contain_env_free(v);

	setenv(WPE_CONTAIN_ENV_PASS, "", 1);
	wpe_contain_report(&wpe_contain_tool, &r);
	chk_true("an empty seam passes nothing", r.pass[0] == '\0');
	clear_settings();
}

/* @scenario SC-ai-contain-05 */
static void test_a_prefix_that_does_not_fit_the_report_is_left_out_whole(void)
{
	WpeContainReport r;
	char list[256];
	const char *first = "XWPE_STUB_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";      /* 40 bytes */
	const char *too_long = "XWPE_STUB_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; /* 60 */
	const char *fits_after = "XWPE_STUB_cccccccccc";                       /* 20 bytes */

	clear_settings();
	snprintf(list, sizeof list, "%s,%s,%s", first, too_long, fits_after);
	setenv(WPE_CONTAIN_ENV_PASS, list, 1);
	wpe_contain_report(&wpe_contain_tool, &r);
	/* The one that does not fit is not in the report, not even its start;
	   the one after it still has its turn. */
	snprintf(list, sizeof list, "%s,%s", first, fits_after);
	chk_true("the report keeps the prefixes that fit, whole", !strcmp(r.pass, list));
	chk_true("and shows no start of the one that did not", strstr(r.pass, "bbbb") == NULL);
	clear_settings();
}

/* ---- folders the PATH rows need ------------------------------------------- */

static char root[PATH_MAX];

static void make_dir(const char *path)
{
	if (mkdir(path, 0755) != 0) {
		printf("FAIL: mkdir %s: %s\n", path, strerror(errno));
		fails++;
	}
}

static void make_program(const char *path)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		printf("FAIL: make %s: %s\n", path, strerror(errno));
		fails++;
		return;
	}
	fputs("#!/bin/sh\nexit 0\n", f);
	fclose(f);
	chmod(path, 0755);
}

static void make_root(void)
{
	const char *base = getenv("TMPDIR");
	char tmpl[PATH_MAX];

	snprintf(tmpl, sizeof tmpl, "%s/xwpe-contain.XXXXXX", base && *base ? base : "/tmp");
	if (!mkdtemp(tmpl)) {
		printf("FAIL: mkdtemp: %s\n", strerror(errno));
		exit(1);
	}
	if (!realpath(tmpl, root))
		snprintf(root, sizeof root, "%s", tmpl);
}

static void drop_root(void)
{
	char path[PATH_MAX];

	snprintf(path, sizeof path, "%s/ws/bin/claude", root);
	unlink(path);
	snprintf(path, sizeof path, "%s/ws/bin/%s", root, NOTE_NAME);
	unlink(path);
	snprintf(path, sizeof path, "%s/ws/%s", root, PLANTED_NAME);
	unlink(path);
	snprintf(path, sizeof path, "%s/ws/bin", root);
	rmdir(path);
	snprintf(path, sizeof path, "%s/ws", root);
	rmdir(path);
	snprintf(path, sizeof path, "%s/tools/claude", root);
	unlink(path);
	snprintf(path, sizeof path, "%s/tools", root);
	rmdir(path);
	rmdir(root);
}

/* @scenario SC-ai-contain-06 */
static void test_a_path_drops_the_entries_a_hostile_folder_could_use(void)
{
	char ws[PATH_MAX], path[2 * PATH_MAX], want[2 * PATH_MAX], *got;

	clear_settings();
	snprintf(ws, sizeof ws, "%s/ws", root);
	got = wpe_contain_path_make(&wpe_contain_tool, ":.:bin:/usr/bin::../x:/bin:", ws);
	chk_true("a tool's PATH is made", got != NULL);
	chk_true("an empty, '.' and relative entries are dropped, the rest kept in order",
	         got && !strcmp(got, "/usr/bin:/bin"));
	free(got);

	snprintf(path, sizeof path, "/usr/bin:%s/ws/bin:/bin", root);
	got = wpe_contain_path_make(&wpe_contain_tool, path, ws);
	chk_true("a tool keeps an absolute entry inside the workspace", got && !strcmp(got, path));
	free(got);

	got = wpe_contain_path_make(&wpe_contain_cli_text, path, ws);
	chk_true("an engine's PATH drops the workspace's own folders",
	         got && !strcmp(got, "/usr/bin:/bin"));
	free(got);

	snprintf(want, sizeof want, "%s/ws/bin", root);
	setenv(WPE_CONTAIN_ENV_PATH_OK, want, 1);
	got = wpe_contain_path_make(&wpe_contain_host_claude, path, ws);
	chk_true("the seam keeps the folder it names", got && !strcmp(got, path));
	free(got);
	clear_settings();
}

/* @scenario SC-ai-contain-07 */
static void test_a_program_is_found_in_the_parent_along_the_safe_path(void)
{
	char dir[PATH_MAX], bin[PATH_MAX], tools[PATH_MAX], path[3 * PATH_MAX], *found;

	clear_settings();
	snprintf(dir, sizeof dir, "%s/ws", root);
	snprintf(bin, sizeof bin, "%s/ws/bin", root);
	snprintf(tools, sizeof tools, "%s/tools", root);
	found = wpe_contain_resolve("sh", "/usr/bin:/bin");
	chk_true("sh is found", found && found[0] == '/');
	free(found);
	chk_true("a missing program is not found", wpe_contain_resolve("xwpe-no-such", "/usr/bin:/bin") == NULL);
	snprintf(path, sizeof path, ".:bin:%s", tools);
	found = wpe_contain_resolve("claude", path);
	chk_true("a relative entry is never searched", found && strstr(found, "/tools/claude") != NULL);
	free(found);
	snprintf(path, sizeof path, ".:bin");
	chk_true("so a program in the working folder is not found", wpe_contain_resolve("claude", path) == NULL);
}

/* @scenario SC-ai-contain-08 */
static void test_the_file_size_limit_reads_its_text(void)
{
	int ok;

	chk("the default is 8 GiB", (long)(WPE_CONTAIN_FSIZE_DEFAULT >> 20), 8192);
	chk("1048576 is one MiB", (long)wpe_contain_fsize_from_text("1048576", &ok), 1048576);
	chk("and was a size", ok, 1);
	chk("2M is two MiB", (long)wpe_contain_fsize_from_text("2M", &ok), 2 * 1024 * 1024);
	chk("3k is three KiB", (long)wpe_contain_fsize_from_text("3k", &ok), 3 * 1024);
	chk("1G is one GiB", (long)(wpe_contain_fsize_from_text("1G", &ok) >> 20), 1024);
	chk("0 is no limit", (long)wpe_contain_fsize_from_text("0", &ok), 0);
	chk("0 was a size", ok, 1);
	chk("text that is no size gives the default",
	    (long)(wpe_contain_fsize_from_text("big", &ok) >> 20), 8192);
	chk("and was not a size", ok, 0);
	chk("a unit with trailing text is no size", (long)(wpe_contain_fsize_from_text("2Mx", &ok) >> 20), 8192);
	chk("and was not a size", ok, 0);
	chk("an empty text is no size", (long)(wpe_contain_fsize_from_text("", &ok) >> 20), 8192);
	chk("a modes' names read back", wpe_contain_mode_from_name("Strict"), WPE_CONTAIN_MODE_STRICT);
	chk("off", wpe_contain_mode_from_name("off"), WPE_CONTAIN_MODE_OFF);
	chk("anything else is standard", wpe_contain_mode_from_name("loose"), WPE_CONTAIN_MODE_STANDARD);
	chk("and no name is standard", wpe_contain_mode_from_name(NULL), WPE_CONTAIN_MODE_STANDARD);
	chk_true("a mode's name is the config word", !strcmp(wpe_contain_mode_name(WPE_CONTAIN_MODE_STRICT), "strict"));
}

/* @scenario SC-ai-contain-09 */
static void test_the_report_says_what_is_held_and_the_header_word_follows(void)
{
	char line[512], notice[800];
	WpeContainReport r;

	clear_settings();
	wpe_contain_report(&wpe_contain_tool, &r);
	chk_true("no file system view is held", !strcmp(r.fs, "none"));
	chk_true("the network is not controlled", !strcmp(r.net, "uncontrolled"));
	chk_true("a command's credentials are reachable through its files", !strcmp(r.credentials, "reachable(files)"));
	wpe_contain_report(&wpe_contain_cli_text, &r);
	chk_true("an engine's are inside its environment", !strcmp(r.credentials, "inside(env)"));
	wpe_contain_report_line(&wpe_contain_tool, line, sizeof line);
	chk_true("the line says fs=none", strstr(line, "fs=none") != NULL);
	chk_true("net=uncontrolled", strstr(line, "net=uncontrolled") != NULL);
	chk_true("env=whitelist", strstr(line, "env=whitelist") != NULL);
	chk_true("and the pass seam is not there", strstr(line, "pass:") == NULL);
	chk_true("the header word is UNCONTAINED", !strcmp(wpe_contain_word(), "UNCONTAINED"));
	setenv(WPE_CONTAIN_ENV_PASS, "XWPE_STUB_", 1);
	wpe_contain_report_line(&wpe_contain_tool, line, sizeof line);
	chk_true("a seam in force is printed", strstr(line, "env=whitelist+pass:XWPE_STUB_") != NULL);
	unsetenv(WPE_CONTAIN_ENV_PASS);

	chk_true("the notice is said once", wpe_contain_notice_once(notice, sizeof notice) != NULL);
	chk_true("and names what is not held", strstr(notice, "NOT held") != NULL);
	chk_true("and the files", strstr(notice, "files you can read") != NULL);
	chk_true("and the network", strstr(notice, "the network") != NULL);
	chk_true("and not again", wpe_contain_notice_once(notice, sizeof notice) == NULL);
	wpe_contain_notice_reset();
	chk_true("a reset says it again", wpe_contain_notice_once(notice, sizeof notice) != NULL);
	wpe_contain_notice_reset();
}

/* ---- the start gate and real children ------------------------------------- */

static char said[WPE_CONTAIN_NOTE_MAX];
static char traced[LINE_MAX_TRACE * 4];

static void on_say(const char *line)
{
	snprintf(said, sizeof said, "%s", line);
}

static void on_trace(const char *line)
{
	size_t n = strlen(traced);

	snprintf(traced + n, sizeof traced - n, "%s\n", line);
}

typedef struct {
	WpeChild c;
	int calls;
	int status;
} Run;

static void on_ended(WpeChild *c, int status, int how, void *data)
{
	Run *r = data;

	(void)c;
	(void)how;
	r->calls++;
	r->status = status;
}

/* The run the loop is being waited on for. */
static Run *waited;

static int run_ended(void)
{
	return waited->calls > 0;
}

static void wait_for_end(Run *r, const char *what)
{
	waited = r;
	wpe_fd_wait_for(run_ended, END_WAIT_MS, what);
}

static void reset_notes(void)
{
	said[0] = '\0';
	traced[0] = '\0';
}

static void spec_sh(WpeChildSpec *spec, char **argv, const char *script, const char *cwd)
{
	memset(spec, 0, sizeof *spec);
	argv[0] = "sh";
	argv[1] = "-c";
	argv[2] = (char *)script;
	argv[3] = NULL;
	spec->argv = argv;
	spec->cwd = cwd;
	spec->io = WPE_CHILD_IO_PIPES;
	spec->group = WPE_CHILD_OWN_SESSION;
	spec->stdin_null = 1;
	spec->stderr_to_stdout = 1;
	spec->side_pty_slave = WPE_CHILD_NO_FD;
	spec->tty_fd = WPE_CHILD_NO_FD;
	spec->what = "the test child";
}

/** read_all - What a finished child left on its output, into @out. */
static void read_all(Run *r, char *out, size_t n)
{
	size_t len = 0;
	ssize_t k;

	while (len + 1 < n && (k = read(r->c.out_fd, out + len, n - len - 1)) > 0)
		len += (size_t)k;
	out[len] = '\0';
	close(r->c.out_fd);
	r->c.out_fd = WPE_CHILD_NO_FD;
}

/** run_through - Run @script through @p and give back what it printed.
    Return: 0, or the start's failure with errno. */
static int run_through(const WpeContainProfile *p, const char *script, const char *cwd,
                       char *out, size_t n, int *status)
{
	WpeChildSpec spec;
	char *argv[4];
	Run r;
	int rc;

	memset(&r, 0, sizeof r);
	out[0] = '\0';
	spec_sh(&spec, argv, script, cwd);
	rc = wpe_contain_start(p, &spec, &r.c, on_ended, &r);
	if (rc != 0)
		return rc;
	wait_for_end(&r, "the contained test child's end");
	read_all(&r, out, n);
	if (status)
		*status = r.status;
	return 0;
}

static void set_polluted_process_environment(void)
{
	setenv("ANTHROPIC_API_KEY", MARKER, 1);
	setenv("OPENAI_API_KEY", MARKER, 1);
	setenv("XWPE_AI_KEY", MARKER, 1);
	setenv("SSH_AUTH_SOCK", "/run/agent-marker", 1);
	setenv("DISPLAY", ":marker", 1);
	setenv("LD_PRELOAD", "/nonexistent/xwpe-marker.so", 1);
	setenv("XWPE_STUB_X", "seam", 1);
}

static void clear_polluted_process_environment(void)
{
	unsetenv("ANTHROPIC_API_KEY");
	unsetenv("OPENAI_API_KEY");
	unsetenv("XWPE_AI_KEY");
	unsetenv("SSH_AUTH_SOCK");
	unsetenv("DISPLAY");
	unsetenv("LD_PRELOAD");
	unsetenv("XWPE_STUB_X");
}

/* @scenario SC-ai-contain-10 */
static void test_a_start_that_names_no_profile_is_refused_before_the_fork(void)
{
	WpeChildSpec spec;
	char *argv[4], marker[PATH_MAX], script[2 * PATH_MAX], out[OUT_MAX];
	Run r;
	int rc, st = 0;

	clear_settings();
	snprintf(marker, sizeof marker, "%s/ran-uncontained", root);
	snprintf(script, sizeof script, "touch %s", marker);
	reset_notes();
	memset(&r, 0, sizeof r);
	spec_sh(&spec, argv, script, NULL);
	rc = wpe_contain_start(NULL, &spec, &r.c, on_ended, &r);
	chk("through the one way to start, no profile is refused", rc, -1);
	chk("with EPERM", errno, EPERM);
	chk_true("the trace names the refusal", strstr(traced, "child uncontained the test child") != NULL);
	chk_true("the status row says why", !strcmp(said, WPE_CHILD_REFUSE_UNCONTAINED_TEXT));
	chk_true("and nothing ran", access(marker, F_OK) != 0);

	reset_notes();
	memset(&r, 0, sizeof r);
	spec_sh(&spec, argv, script, NULL);
	spec.origin = WPE_CHILD_ORIGIN_AI;
	spec.contain = NULL;
	rc = wpe_child_start(&spec, &r.c, on_ended, &r);
	chk("the owner's own gate refuses an assistant start with no profile", rc, -1);
	chk("with EPERM here too", errno, EPERM);
	chk_true("traced", strstr(traced, "child uncontained") != NULL);
	chk_true("and nothing ran", access(marker, F_OK) != 0);

	rc = run_through(&wpe_contain_tool, "echo ok", NULL, out, sizeof out, &st);
	chk("a start with a profile runs", rc, 0);
	chk_true("and its output arrives", !strcmp(out, "ok\n"));
	chk("and ends well", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
	reset_notes();
}

/* @scenario SC-ai-contain-11 */
static void test_strict_refuses_every_start_while_nothing_is_held(void)
{
	char out[OUT_MAX], marker[PATH_MAX], script[2 * PATH_MAX];
	int rc, i;
	const WpeContainProfile *all[] = {
		&wpe_contain_tool, &wpe_contain_hook, &wpe_contain_git, &wpe_contain_git_commit,
		&wpe_contain_cli_text, &wpe_contain_host_claude, &wpe_contain_host_command, NULL
	};

	clear_settings();
	snprintf(marker, sizeof marker, "%s/ran-strict", root);
	snprintf(script, sizeof script, "touch %s", marker);
	setenv(WPE_CONTAIN_ENV_MODE, "strict", 1);
	for (i = 0; all[i]; i++) {
		reset_notes();
		rc = run_through(all[i], script, NULL, out, sizeof out, NULL);
		chk("Strict refuses the start", rc, -1);
		chk("with EPERM", errno, EPERM);
		chk_true("naming the file system and the network",
		         strstr(said, "files") && strstr(said, "network") && strstr(said, "strict"));
		chk_true("and traced", strstr(traced, "child uncontained") != NULL);
		chk_true("the reason leads with the key's name", strncmp(said, "AIContain strict", 16) == 0);
		chk_true("nothing ran", access(marker, F_OK) != 0);
	}
	clear_settings();
	wpe_contain_mode_set(WPE_CONTAIN_MODE_STRICT);
	reset_notes();
	rc = run_through(&wpe_contain_tool, "echo x", NULL, out, sizeof out, NULL);
	chk("the config file's mode refuses too", rc, -1);
	wpe_contain_mode_set(WPE_CONTAIN_MODE_STANDARD);
	setenv(WPE_CONTAIN_ENV_MODE, "standard", 1);
	wpe_contain_mode_set(WPE_CONTAIN_MODE_STRICT);
	rc = run_through(&wpe_contain_tool, "echo x", NULL, out, sizeof out, NULL);
	chk("and the environment's word wins over the file", rc, 0);
	clear_settings();
	reset_notes();
}

/* @scenario SC-ai-contain-12 */
static void test_a_contained_child_sees_the_built_environment_and_its_limits(void)
{
	char out[OUT_MAX], want[64];
	int rc, st = 0;

	clear_settings();
	set_polluted_process_environment();
	setenv(WPE_CONTAIN_ENV_PASS, "XWPE_STUB_", 1);
	rc = run_through(&wpe_contain_tool, PROBE_SCRIPT, NULL, out, sizeof out, &st);
	chk("the probe runs", rc, 0);
	chk_true("it does not see the Anthropic key", strstr(out, "ANTHROPIC_API_KEY") == NULL);
	chk_true("nor the OpenAI key", strstr(out, "OPENAI_API_KEY") == NULL);
	chk_true("nor the editor's key", strstr(out, "XWPE_AI_KEY") == NULL);
	chk_true("nor any marker value", strstr(out, MARKER) == NULL);
	chk_true("nor the agent socket", strstr(out, "SSH_AUTH_SOCK") == NULL);
	chk_true("nor the display", strstr(out, "DISPLAY=") == NULL);
	chk_true("nor the preload", strstr(out, "LD_PRELOAD") == NULL);
	chk_true("it sees the seam's stub variable", strstr(out, "XWPE_STUB_X=seam") != NULL);
	chk_true("and TERM=dumb", strstr(out, "TERM=dumb") != NULL);
	snprintf(want, sizeof want, "FSIZE=%llu", FSIZE_512_BLOCKS);
	if (strstr(out, want) == NULL) {
		snprintf(want, sizeof want, "FSIZE=%llu", FSIZE_1K_BLOCKS);
		chk_true("its file-size limit is the documented 8 GiB", strstr(out, want) != NULL);
	}
	chk_true("it leaves no core file", strstr(out, "CORE=0") != NULL);
	if (nnp_rows_run("it can never gain privileges", out))
		chk_true("it can never gain privileges", nnp_state(out) == 1);

	setenv(WPE_CONTAIN_ENV_FSIZE, "64M", 1);
	rc = run_through(&wpe_contain_tool, "echo FSIZE=$(ulimit -f)", NULL, out, sizeof out, &st);
	chk("a user's limit runs", rc, 0);
	snprintf(want, sizeof want, "FSIZE=%llu", (64ULL << 20) / 512);
	if (strstr(out, want) == NULL) {
		snprintf(want, sizeof want, "FSIZE=%llu", (64ULL << 20) / 1024);
		chk_true("is the limit the shadow asked for", strstr(out, want) != NULL);
	}
	clear_settings();
	clear_polluted_process_environment();
}

/* @scenario SC-ai-contain-13 */
static void test_off_keeps_the_environment_and_nothing_else(void)
{
	char out[OUT_MAX];
	int rc;

	clear_settings();
	set_polluted_process_environment();
	setenv(WPE_CONTAIN_ENV_MODE, "off", 1);
	rc = run_through(&wpe_contain_tool, PROBE_SCRIPT, NULL, out, sizeof out, NULL);
	chk("the probe runs under Off", rc, 0);
	chk_true("the environment is still built", strstr(out, MARKER) == NULL);
	chk_true("and the limit is left as the editor inherited it", inherited_fsize_shown(out));
	if (nnp_rows_run("Off leaves no_new_privs unset", out))
		chk_true("nor no_new_privs", nnp_state(out) == 0);
	clear_settings();
	clear_polluted_process_environment();
}

/* @scenario SC-ai-contain-14 */
static void test_an_engine_never_runs_a_program_the_workspace_planted(void)
{
	char ws[PATH_MAX], plant[PATH_MAX], marker[PATH_MAX], out[OUT_MAX];
	char script[2 * PATH_MAX], oldpath[PATH_MAX];
	WpeChildSpec spec;
	char *argv[2];
	Run r;
	int rc, st = 0;
	FILE *f;
	const char *p = getenv("PATH");

	clear_settings();
	snprintf(oldpath, sizeof oldpath, "%s", p ? p : "/usr/bin:/bin");
	snprintf(ws, sizeof ws, "%s/ws", root);
	snprintf(plant, sizeof plant, "%s/ws/%s", root, PLANTED_NAME);
	snprintf(marker, sizeof marker, "%s/ran-planted", root);
	f = fopen(plant, "w");
	if (f) {
		snprintf(script, sizeof script, "#!/bin/sh\ntouch %s\nexit 0\n", marker);
		fputs(script, f);
		fclose(f);
		chmod(plant, 0755);
	}
	snprintf(script, sizeof script, ".:%s:bin:%s", ws, oldpath);
	setenv("PATH", script, 1);

	memset(&spec, 0, sizeof spec);
	argv[0] = PLANTED_NAME;
	argv[1] = NULL;
	spec.argv = argv;
	spec.cwd = ws;
	spec.io = WPE_CHILD_IO_PIPES;
	spec.group = WPE_CHILD_OWN_SESSION;
	spec.stdin_null = 1;
	spec.stderr_to_stdout = 1;
	spec.side_pty_slave = WPE_CHILD_NO_FD;
	spec.tty_fd = WPE_CHILD_NO_FD;
	spec.what = "the test engine";
	memset(&r, 0, sizeof r);
	rc = wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r);
	chk("the engine is refused: its program sits in a workspace folder", rc, -1);
	chk_true("the planted program in the workspace was not run", access(marker, F_OK) != 0);

	snprintf(script, sizeof script, ".:bin:%s", oldpath);
	setenv("PATH", script, 1);
	memset(&r, 0, sizeof r);
	spec.argv = argv;
	rc = wpe_contain_start(&wpe_contain_tool, &spec, &r.c, on_ended, &r);
	chk("a command's start also finds no such program in '.'", rc, 0);
	wait_for_end(&r, "the command's end");
	read_all(&r, out, sizeof out);
	chk_true("a relative entry is not searched for a command either", access(marker, F_OK) != 0);
	(void)st;
	setenv("PATH", oldpath, 1);
	unlink(plant);
	clear_settings();
}

/* @scenario SC-ai-contain-15 */
static void test_an_engine_that_sits_in_a_dropped_workspace_folder_is_explained(void)
{
	char ws[PATH_MAX], bin[PATH_MAX], path[3 * PATH_MAX], note[600], out[OUT_MAX];
	char *argv[2];
	WpeChildSpec spec;
	Run r;
	const char *old = getenv("PATH");
	char oldpath[PATH_MAX];
	int rc;

	clear_settings();
	snprintf(oldpath, sizeof oldpath, "%s", old ? old : "/usr/bin:/bin");
	snprintf(ws, sizeof ws, "%s/ws", root);
	snprintf(bin, sizeof bin, "%s/ws/bin", root);
	snprintf(path, sizeof path, "%s:%s", bin, oldpath);
	setenv("PATH", path, 1);

	chk("the engine's program is in a folder its PATH drops: a note",
	    wpe_contain_engine_note(&wpe_contain_cli_text, NOTE_NAME, ws, note, sizeof note), 1);
	chk_true("the note names the folder", strstr(note, bin) != NULL);
	chk_true("and says why", strstr(note, "inside the workspace") != NULL);
	chk_true("and what to do", strstr(note, "AIContainPathAllow") != NULL);
	chk("a command's profile trusts that folder: no note",
	    wpe_contain_engine_note(&wpe_contain_tool, NOTE_NAME, ws, note, sizeof note), 0);
	chk("the folder is judged against the workspace the engine runs in, not the editor's",
	    wpe_contain_engine_note(&wpe_contain_host_claude, NOTE_NAME, "/nonexistent-other-ws", note, sizeof note), 0);
	chk("a program that is nowhere: no note",
	    wpe_contain_engine_note(&wpe_contain_cli_text, "xwpe-no-such-engine", ws, note, sizeof note), 0);

	memset(&spec, 0, sizeof spec);
	argv[0] = NOTE_NAME;
	argv[1] = NULL;
	spec.argv = argv;
	spec.cwd = ws;
	spec.io = WPE_CHILD_IO_PIPES;
	spec.group = WPE_CHILD_OWN_SESSION;
	spec.stdin_null = 1;
	spec.side_pty_slave = WPE_CHILD_NO_FD;
	spec.tty_fd = WPE_CHILD_NO_FD;
	spec.what = "the test engine";
	memset(&r, 0, sizeof r);
	reset_notes();
	rc = wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r);
	chk("the start is refused, not left to fail without a word", rc, -1);
	chk_true("and says which folder and why", strstr(said, bin) != NULL && strstr(said, "inside the workspace"));

	wpe_contain_path_allow_set(bin);
	chk("a folder the user allowed is kept: no note",
	    wpe_contain_engine_note(&wpe_contain_cli_text, NOTE_NAME, ws, note, sizeof note), 0);
	memset(&r, 0, sizeof r);
	rc = wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r);
	chk("and the engine then starts", rc, 0);
	if (rc == 0) {
		wait_for_end(&r, "the allowed engine's end");
		read_all(&r, out, sizeof out);
	}
	wpe_contain_path_allow_set(NULL);
	setenv("PATH", oldpath, 1);
	clear_settings();
	reset_notes();
}

/* How long each of the two nested folder names of the long-folder row is. */
#define LONG_PART 200

/* @scenario SC-ai-contain-17 */
static void test_the_note_names_a_long_folder_whole(void)
{
	char ws[PATH_MAX], d1[PATH_MAX], d2[PATH_MAX], prog[PATH_MAX], path[3 * PATH_MAX];
	char a[LONG_PART + 1], b[LONG_PART + 1], oldpath[PATH_MAX], out_unused[OUT_MAX];
	char *argv[2];
	WpeChildSpec spec;
	Run r;
	const char *old = getenv("PATH");

	clear_settings();
	memset(a, 'a', LONG_PART);
	a[LONG_PART] = '\0';
	memset(b, 'b', LONG_PART);
	b[LONG_PART] = '\0';
	snprintf(oldpath, sizeof oldpath, "%s", old ? old : "/usr/bin:/bin");
	snprintf(ws, sizeof ws, "%s/ws", root);
	snprintf(d1, sizeof d1, "%s/%s", ws, a);
	snprintf(d2, sizeof d2, "%s/%s", d1, b);
	make_dir(d1);
	make_dir(d2);
	snprintf(prog, sizeof prog, "%s/%s", d2, NOTE_NAME);
	make_program(prog);
	snprintf(path, sizeof path, "%s:%s", d2, oldpath);
	setenv("PATH", path, 1);

	memset(&spec, 0, sizeof spec);
	argv[0] = NOTE_NAME;
	argv[1] = NULL;
	spec.argv = argv;
	spec.cwd = ws;
	spec.io = WPE_CHILD_IO_PIPES;
	spec.group = WPE_CHILD_OWN_SESSION;
	spec.stdin_null = 1;
	spec.side_pty_slave = WPE_CHILD_NO_FD;
	spec.tty_fd = WPE_CHILD_NO_FD;
	spec.what = "the test engine";
	memset(&r, 0, sizeof r);
	reset_notes();
	chk("a deep folder: the engine is refused", wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r), -1);
	chk_true("and the sentence holds the whole folder", strstr(said, d2) != NULL);
	chk_true("and still says what to do", strstr(said, "AIContainPathAllow") != NULL);

	wpe_contain_path_allow_set(d2);
	memset(&r, 0, sizeof r);
	reset_notes();
	chk("the same deep folder, allowed whole: the engine starts",
	    wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r), 0);
	wait_for_end(&r, "the engine of the deep allowed folder");
	read_all(&r, out_unused, sizeof out_unused);
	wpe_contain_path_allow_set(NULL);

	unlink(prog);
	rmdir(d2);
	rmdir(d1);
	setenv("PATH", oldpath, 1);
	clear_settings();
	reset_notes();
}

/* ---- long folders and long words in the lists the user writes ------------- */

/* How long each folder of a deep test folder is, and the length at which the
   old reader cut a word. */
#define DEEP_PART     200
#define OLD_CUT_AT    255
#define HUGE_ENTRY    5000
#define FOLDER_LONG_WANTED 3000
#define FOLDER_MEDIUM 300

/**
 * folder_long - How long the long test folder is: 3000 bytes, or the longest
 * path this system can open when that is shorter.
 *
 * A folder the system cannot name is one the editor rightly drops, so the
 * row that keeps a long folder whole asks for the longest one that can be
 * opened: OpenBSD's PATH_MAX is 1024, Linux's 4096.
 * Return: the length, in bytes.
 */
static size_t folder_long(void)
{
	if (FOLDER_LONG_WANTED < PATH_MAX)
		return FOLDER_LONG_WANTED;
	return PATH_MAX - 1;
}

/**
 * deep_dir_make - Make nested folders under @base so that the last one's
 * path is exactly @want bytes long.
 * @base: an existing folder; @want: the length of the result; @fill: the
 * letter the folder names are made of (two rows use two letters, so their
 * folders never meet); @out: receives it, @n bytes.
 *
 * A length that does not fit in @out is a fault of the row, said once: the
 * path could not grow, and the loop would make the same folder for ever.
 */
static void deep_dir_make(const char *base, size_t want, int fill, char *out, size_t n)
{
	char part[DEEP_PART + 1];
	size_t left;

	snprintf(out, n, "%s", base);
	if (want >= n) {
		printf("FAIL: a %zu-byte folder does not fit in its %zu-byte buffer\n", want, n);
		fails++;
		return;
	}
	while (strlen(out) + 1 + 1 <= want) {
		left = want - strlen(out) - 1;
		if (left > DEEP_PART)
			left = DEEP_PART;
		memset(part, fill, left);
		part[left] = '\0';
		snprintf(out + strlen(out), n - strlen(out), "/%s", part);
		make_dir(out);
	}
}

/**
 * deep_dir_drop - Remove the folders deep_dir_make made, from the last up to
 * @base (which stays).
 */
static void deep_dir_drop(const char *base, char *dir)
{
	char *slash;

	while (strlen(dir) > strlen(base) && (slash = strrchr(dir, '/')) != NULL) {
		rmdir(dir);
		*slash = '\0';
	}
}

/** engine_path_of - What an engine's PATH is when @path is the editor's and @ws the workspace. */
static char *engine_path_of(const char *path, const char *ws)
{
	return wpe_contain_path_make(&wpe_contain_cli_text, path, ws);
}

/* @scenario SC-ai-contain-38 */
static void test_an_allowed_folder_of_any_length_is_kept_whole(void)
{
	/* allow holds the huge word and two folders: sized from HUGE_ENTRY, not
	   from PATH_MAX alone, which on the BSDs (1024) left 3072 bytes for 6325
	   and cut the list before the long folder. */
	char ws[PATH_MAX], d300[PATH_MAX], d3000[PATH_MAX];
	char allow[HUGE_ENTRY + 2 * PATH_MAX + 3], path[3 * PATH_MAX];
	char huge[HUGE_ENTRY + 1];
	char *got;

	clear_settings();
	snprintf(ws, sizeof ws, "%s/ws", root);
	deep_dir_make(ws, FOLDER_MEDIUM, 'm', d300, sizeof d300);
	snprintf(allow, sizeof allow, "%s", d300);
	chk("the medium folder is 300 bytes", (long)strlen(d300), FOLDER_MEDIUM);
	snprintf(path, sizeof path, "%s:/usr/bin", d300);
	wpe_contain_path_allow_set(allow);
	got = engine_path_of(path, ws);
	chk_true("a 300-byte allowed folder is kept", got && !strcmp(got, path));
	free(got);
	wpe_contain_path_allow_set(NULL);

	deep_dir_make(ws, folder_long(), 'l', d3000, sizeof d3000);
	chk("the long folder is as long as asked", (long)strlen(d3000), (long)folder_long());
	/* a first word no system could open: it is skipped, the two after it are read */
	huge[0] = '/';
	memset(huge + 1, 'x', HUGE_ENTRY - 1);
	huge[HUGE_ENTRY] = '\0';
	snprintf(allow, sizeof allow, "%s:%s:%s", huge, d300, d3000);
	snprintf(path, sizeof path, "%s:/usr/bin:%s", d3000, d300);
	wpe_contain_path_allow_set(allow);
	got = engine_path_of(path, ws);
	chk_true("the long allowed folder is kept, with the 300-byte one, in order",
	         got && !strcmp(got, path));
	free(got);
	wpe_contain_path_allow_set(NULL);
	got = engine_path_of(path, ws);
	chk_true("and without the allowance both are dropped", got && !strcmp(got, "/usr/bin"));
	free(got);

	deep_dir_drop(ws, d3000);
	deep_dir_drop(ws, d300);
	clear_settings();
}

/* @scenario SC-ai-contain-39 */
static void test_a_word_cut_short_never_allows_the_folder_it_begins_with(void)
{
	char ws[PATH_MAX], keep[PATH_MAX], allow[2 * PATH_MAX], path[3 * PATH_MAX];
	char *got;

	clear_settings();
	snprintf(ws, sizeof ws, "%s/ws", root);
	deep_dir_make(ws, OLD_CUT_AT, 'k', keep, sizeof keep);
	snprintf(allow, sizeof allow, "%s/%0100d", keep, 0);
	snprintf(path, sizeof path, "%s:/usr/bin", keep);
	wpe_contain_path_allow_set(allow);
	got = engine_path_of(path, ws);
	chk_true("an allowed word longer than the old cut does not allow the folder it begins with",
	         got && !strcmp(got, "/usr/bin"));
	free(got);
	wpe_contain_path_allow_set(NULL);
	deep_dir_drop(ws, keep);
	clear_settings();
}

/* @scenario SC-ai-contain-40 */
static void test_a_path_entry_no_system_could_open_is_dropped_for_an_engine(void)
{
	char ws[PATH_MAX], huge[HUGE_ENTRY + 2], path[2 * HUGE_ENTRY], want[64];
	char *got;

	clear_settings();
	snprintf(ws, sizeof ws, "%s/ws", root);
	huge[0] = '/';
	memset(huge + 1, 'x', HUGE_ENTRY - 1);
	huge[HUGE_ENTRY] = '\0';
	snprintf(path, sizeof path, "/usr/bin:%s:/bin", huge);
	snprintf(want, sizeof want, "/usr/bin:/bin");
	got = engine_path_of(path, ws);
	chk_true("an engine drops the entry and keeps the ones around it, in order",
	         got && !strcmp(got, want));
	free(got);
	got = wpe_contain_path_make(&wpe_contain_tool, path, ws);
	chk_true("a command's PATH keeps it as written", got && !strcmp(got, path));
	free(got);
	clear_settings();
}

/* @scenario SC-ai-contain-41 */
static void test_a_folder_with_a_space_is_one_folder_in_the_allow_list(void)
{
	char ws[PATH_MAX], mine[PATH_MAX], tools[PATH_MAX], path[3 * PATH_MAX], want[3 * PATH_MAX];
	char *got;

	clear_settings();
	snprintf(ws, sizeof ws, "%s/ws", root);
	snprintf(mine, sizeof mine, "%s/My", ws);
	snprintf(tools, sizeof tools, "%s/My Tools", ws);
	make_dir(mine);
	make_dir(tools);
	snprintf(path, sizeof path, "%s:%s:/usr/bin", mine, tools);
	snprintf(want, sizeof want, "%s:/usr/bin", tools);
	wpe_contain_path_allow_set(tools);
	got = engine_path_of(path, ws);
	chk_true("the folder named with a space is kept and the one before the space is not",
	         got && !strcmp(got, want));
	free(got);
	wpe_contain_path_allow_set(NULL);
	rmdir(tools);
	rmdir(mine);
	clear_settings();
}

/* @scenario SC-ai-contain-42 */
static void test_a_name_cut_short_never_keeps_the_variable_it_begins_with(void)
{
	char name[OLD_CUT_AT + 1], longer[OLD_CUT_AT + 100], var[OLD_CUT_AT + 8];
	char *from[4];
	char **v;

	clear_settings();
	memset(name, 'N', OLD_CUT_AT);
	name[OLD_CUT_AT] = '\0';
	snprintf(longer, sizeof longer, "%sMORE_TEXT_TO_MAKE_IT_LONGER_THAN_ANY_NAME_THE_LIST_COULD_HOLD", name);
	snprintf(var, sizeof var, "%s=1", name);
	from[0] = "PATH=/usr/bin:/bin";
	from[1] = var;
	from[2] = "KEEPME=1";
	from[3] = NULL;

	wpe_contain_keep_names_set(name);
	v = wpe_contain_env_make(&wpe_contain_tool, from, "/nonexistent-ws");
	chk_true("a name that fits is kept", has(v, name) != NULL);
	wpe_contain_env_free(v);

	snprintf(longer + strlen(longer), sizeof longer - strlen(longer), ",KEEPME");
	wpe_contain_keep_names_set(longer);
	v = wpe_contain_env_make(&wpe_contain_tool, from, "/nonexistent-ws");
	chk_true("a longer name keeps no variable named by its first bytes", has(v, name) == NULL);
	chk_true("and the name after it in the list is still read", has(v, "KEEPME") != NULL);
	wpe_contain_env_free(v);
	clear_settings();
}

/* @scenario SC-ai-contain-43 */
static void test_a_workspace_no_system_could_open_refuses_the_start(void)
{
	char huge[HUGE_ENTRY + 1], *argv[2];
	WpeChildSpec spec;
	Run r;
	char *got;
	int rc;

	clear_settings();
	huge[0] = '/';
	memset(huge + 1, 'w', HUGE_ENTRY - 1);
	huge[HUGE_ENTRY] = '\0';
	got = engine_path_of("/usr/bin:/bin", huge);
	chk_true("an engine's PATH judged against a workspace that cannot be held keeps nothing",
	         got && got[0] == '\0');
	free(got);

	memset(&spec, 0, sizeof spec);
	argv[0] = "sh";
	argv[1] = NULL;
	spec.argv = argv;
	spec.cwd = huge;
	spec.io = WPE_CHILD_IO_PIPES;
	spec.group = WPE_CHILD_OWN_SESSION;
	spec.stdin_null = 1;
	spec.side_pty_slave = WPE_CHILD_NO_FD;
	spec.tty_fd = WPE_CHILD_NO_FD;
	spec.what = "the test engine";
	memset(&r, 0, sizeof r);
	reset_notes();
	rc = wpe_contain_start(&wpe_contain_cli_text, &spec, &r.c, on_ended, &r);
	chk("a workspace that does not fit refuses the start", rc, -1);
	chk_true("and says the path is longer than the system allows",
	         strstr(said, "longer than the system allows") != NULL && strstr(said, "workspace") != NULL);
	if (rc == 0)
		wait_for_end(&r, "the start that should have been refused");
	reset_notes();
	clear_settings();
}

/* @scenario SC-ai-contain-16 */
static void test_a_lower_inherited_hard_limit_is_kept_not_raised(void)
{
	struct rlimit lim;
	char out[OUT_MAX], want[64];
	int rc, st = 0;

	clear_settings();
	lim.rlim_cur = LOW_HARD_FSIZE;
	lim.rlim_max = LOW_HARD_FSIZE;
	chk("the test lowers its own hard limit (it cannot be raised again)", setrlimit(RLIMIT_FSIZE, &lim), 0);
	rc = run_through(&wpe_contain_tool, "echo FSIZE=$(ulimit -f)", NULL, out, sizeof out, &st);
	chk("a command still starts under a hard limit below the default", rc, 0);
	chk("and ends well", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
	snprintf(want, sizeof want, "FSIZE=%llu", (unsigned long long)LOW_HARD_FSIZE / 512);
	if (strstr(out, want) == NULL) {
		snprintf(want, sizeof want, "FSIZE=%llu", (unsigned long long)LOW_HARD_FSIZE / 1024);
		chk_true("it gets the inherited limit, no more", strstr(out, want) != NULL);
	}
	reset_notes();
}

int main(int argc, char **argv)
{
	char self[PATH_MAX];

	if (argc > 1 && strcmp(argv[1], NNP_STATUS_OPTION) == 0)
		return print_no_new_privs_status();
	if (realpath(argv[0], self) == NULL)
		snprintf(self, sizeof self, "%s", argv[0]);
	build_probe_script(self);
	test_watchdog_case("test_ai_contain (the assistant's children are held as "
	                   "their profile says): a child's end never came", HANG_SECONDS);
	WpeChildWakeOpen();
	wpe_child_reap_install();
	wpe_child_say_set(on_say);
	wpe_child_trace_set(on_trace);
	make_root();
	{
		char ws[PATH_MAX], bin[PATH_MAX], tools[PATH_MAX], prog[PATH_MAX];

		snprintf(ws, sizeof ws, "%s/ws", root);
		snprintf(bin, sizeof bin, "%s/ws/bin", root);
		snprintf(tools, sizeof tools, "%s/tools", root);
		make_dir(ws);
		make_dir(bin);
		make_dir(tools);
		snprintf(prog, sizeof prog, "%s/tools/claude", root);
		make_program(prog);
		snprintf(prog, sizeof prog, "%s/ws/bin/%s", root, NOTE_NAME);
		make_program(prog);
	}

	test_a_command_gets_a_built_environment_not_the_editors();
	test_an_engine_keeps_its_own_login_and_no_other();
	test_the_users_lists_add_names_but_never_a_key_or_a_preload();
	test_only_a_commit_signed_with_ssh_keeps_the_agent_socket();
	test_the_test_seam_keeps_a_prefix_but_never_one_that_would_pass_a_key();
	test_a_prefix_that_does_not_fit_the_report_is_left_out_whole();
	test_a_path_drops_the_entries_a_hostile_folder_could_use();
	test_a_program_is_found_in_the_parent_along_the_safe_path();
	test_the_file_size_limit_reads_its_text();
	test_the_report_says_what_is_held_and_the_header_word_follows();
	test_a_start_that_names_no_profile_is_refused_before_the_fork();
	test_strict_refuses_every_start_while_nothing_is_held();
	test_a_contained_child_sees_the_built_environment_and_its_limits();
	test_off_keeps_the_environment_and_nothing_else();
	test_an_engine_never_runs_a_program_the_workspace_planted();
	test_an_engine_that_sits_in_a_dropped_workspace_folder_is_explained();
	test_the_note_names_a_long_folder_whole();
	test_an_allowed_folder_of_any_length_is_kept_whole();
	test_a_word_cut_short_never_allows_the_folder_it_begins_with();
	test_a_path_entry_no_system_could_open_is_dropped_for_an_engine();
	test_a_folder_with_a_space_is_one_folder_in_the_allow_list();
	test_a_name_cut_short_never_keeps_the_variable_it_begins_with();
	test_a_workspace_no_system_could_open_refuses_the_start();
	test_a_lower_inherited_hard_limit_is_kept_not_raised();   /* last: the limit cannot be raised again */

	drop_root();
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}

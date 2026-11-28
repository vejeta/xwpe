#ifndef __PROGR_H
#define __PROGR_H
/* progr.h						  */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include <time.h>
#include <signal.h>
#include "WeProg.h"
#include "we_progname.h"

/* What Ctrl-F9 starts once the build has made it: a field of the language's
   entry, so a language that is not built in (a user's Lua) can say it in
   its xwperc as "Run : interpreter".  UNSET is not a kind: it is what an
   entry read from an option file holds until the file is read to its end
   (e_opt_lang_finish) and no Run key named one. */
#define E_RUN_KIND_UNSET   0
#define E_RUN_INTERPRETER  1   /* <Compiler> <source>: Python, Perl, a68g */
#define E_RUN_PROGRAM      2   /* the program the build made: C, Rust, Go */
#define E_RUN_VM           3   /* java <class>, in the build folder: Java, Scala */
#define E_RUN_NONE         4   /* nothing: LaTeX makes no program */

/* The toolchain the editor itself picked for one file by its content,
   carried by the entry it was applied to so the user's own copy of the
   language can be given the same one.  NONE is a language with a single
   toolchain. */
#define E_FILE_SWITCH_NONE 0
#define E_FILE_SWITCH_A68G 1   /* Algol 68 Genie: interprets, classic stropping */
#define E_FILE_SWITCH_GA68 2   /* GNU Algol 68: compiles and links, modern stropping */

struct e_s_prog {
 char *language, *compiler, *comp_str, *libraries,
   *exe_name, *intstr, key;
 char *start_symbol;
 char **filepostfix; /* Expandable array */
 int	comp_sw, x;
 int	run_kind;           /* E_RUN_*: what Ctrl-F9 starts */
 int	file_switch;        /* E_FILE_SWITCH_*: the pick this entry carries */
};

struct e_prog {
 int num;  
 char *arguments, *project, *exedir, *sys_include;
 struct e_s_prog **comp;
};

extern int e__project;
extern struct e_s_prog e_s_prog;
extern struct e_prog e_prog;

typedef struct {  FILE *fp;  BUFFER *b;  POINT p;  }  E_AFILE;

#ifdef DJGPP
typedef long M_TIME;
#else
typedef time_t M_TIME;
#endif

/*   we_prog.c   */

int e_prog_switch(FENSTER *f, int c);
int e_compile(FENSTER *f);
int e_p_make(FENSTER *f);
int e_run(FENSTER *f);
/* What runs when a build step ends: @result is 0 for a success, -1 for a
   failure and WPE_ESC for a build that was cancelled or could not start. */
typedef void (*e_build_cont)(FENSTER *f, int result, void *data);
int e_p_make_then(FENSTER *f, e_build_cont done, void *data);
int e_build_busy(void);
void e_build_cancel(void);
/* Is the build's end already on its way (Esc asked for it, the child not
   waited for yet), and did its step end behind a box so that its result
   waits for the key loop?  Both are asked by the key's owner (we_cancel.c). */
int e_build_ending(void);
int e_build_result_held(void);
/* Is the window on top the Messages pane, where a build's and a starting
   debugger's waiting line is written? */
int e_messages_window_on_top(const FENSTER *top);
void e_build_end_at_exit(void);
void e_build_refuse(FENSTER *f);
void e_status_say(FENSTER *f, const char *text);
void e_status_say_until_key(FENSTER *f, const char *text);
/* Messages as a background task (a build, a starting debugger) uses it. */
FENSTER *e_build_messages(ECNT *cn);
/* Rows of a view the editor writes: Messages and the output windows. */
int e_view_add_row(BUFFER *b);
void e_view_append(BUFFER *b, const char *text);
void e_messages_show_tail(ECNT *cn);
/* we_child.c: the editor's children, started, watched and ended in one
   place; their ends come back from the fd loop, nobody waits for them. */
#include "we_child.h"
int e_c_project(FENSTER *f, e_build_cont done, void *data);
int e_free_arg(char **arg, int argc);
int e_rel_brkwtch(FENSTER *f);
struct dirfile **e_make_prj_opt(FENSTER *f);
int e_comp(FENSTER *f, e_build_cont done, void *data);
char *e_build_dir(FENSTER *f);
int e_print_arg(FILE *fp, char *s, char **argv, int n);
int e_show_error(int n, FENSTER *f);
int e_make_error_list(FENSTER *f);
int e_set_error_list(int n, char **files, int *lines, int *ys, char **texts);
int e_previous_error(FENSTER *f);
int e_next_error(FENSTER *f);
int e_line_read(int n, char *s, int max);
int e_arguments(FENSTER *f);
int e_check_c_file(char *name);
int e_prj_ob_file(FENSTER *f);
int e_check_header(char *file, M_TIME otime, ECNT *cn, int sw);
char *e_cat_string(char *p, char *str);
int e_make_arg(char ***arg, char *str);
int e_ini_prog(ECNT *cn);
int e_copy_prog(struct e_s_prog *out, struct e_s_prog *in);
/* Free every field e_copy_prog fills and zero @p, so a copy that was never
   filled (all zero) is freed as safely as a full one. */
void e_prog_entry_free(struct e_s_prog *p);
/* Give @entry the toolchain its language picks for the file at @path (the
   Algol 68 dialect); a language with one toolchain gets NONE.  The one
   caller of the pick. */
void e_prog_switch_for_file(struct e_s_prog *entry, const char *path);
/* Give @entry the pick @like carries, so a copy of the user's own entry is
   in force for the same file as @like without reading the file again.  The
   pick never crosses languages: another language's @like gives NONE. */
void e_prog_switch_like(struct e_s_prog *entry, const struct e_s_prog *like);
/* @p's value for an xwperc key ("Compiler", "CompilerOptions", "Libraries",
   "Executable", "CompilerSwitch" as "0"/"1", "Run" as its word) into
   @out[@n].  Return: 0; -1 for a key that is none of these. */
int e_prog_field_text(const struct e_s_prog *p, const char *key, char *out, size_t n);
/* What Ctrl-F9 starts for a language's entry (E_RUN_*), the name its xwperc
   writes for it ("interpreter", "program", "vm", "none"), the kind a name
   means (E_RUN_KIND_UNSET for a word that is none), and the kind an entry
   that names none takes from its compiler style. */
int e_run_kind_of(const struct e_s_prog *p);
const char *e_run_kind_name(int kind);
int e_run_kind_parse(const char *name);
int e_run_kind_default(const struct e_s_prog *p);
/* The java beside @beside (a tool the class was made with, or the debugger),
   else java on PATH, else "java"; and the VM's words on a start plan: the
   java, @agent when not NULL, then the class.  Both Run and jdb start the
   VM through them.  The words refuse a class java would read as an option. */
struct WpeStartPlan;
int e_java_vm_path(const char *beside, char *out, size_t n);
int e_java_vm_words(struct WpeStartPlan *p, const char *java, const char *agent,
                    const char *cls, int from);
/* A language's compile and link line as the question shows it (malloc'd). */
char *e_prog_command_text(const struct e_s_prog *p);
/* Ask before a compile line a start-folder xwperc names runs: 0 to go on. */
int e_prog_approve_compile(FENSTER *f);
char *e_find_var(char *var);
/* A project variable as its file writes it, before expansion ("" if unset). */
const char *e_find_var_raw(const char *var);
const char *e_get_start_symbol(void);
/* The program the open project links, as a command names it (under Exedir). */
const char *e_prj_program(void);
/* The open file as the program-name owner (we_progname.h) reads it: the
   debugger names the program it loads and the binary it compiles through
   the same adapter F9 and Run use. */
void e_prog_source_of(WpeProgramSource *s, const char *name, const struct e_s_prog *entry,
                      const char *executable, const char *exedir);
int e_run_options(FENSTER *f);
int e_run_c_options(FENSTER *f);
int e_project_options(FENSTER *f);
int e_system(struct WpeStartPlan *plan, ECNT *cn);
void e_messages_scroll_to_bottom(FENSTER *f);
int e_p_show_messages(FENSTER *f);
int e_p_show_program_output(FENSTER *f);
int e_d_p_message(char *str, FENSTER *f, int sw);
int e_view_append_line(FENSTER *wf, char *str, int sw);
int e_install(FENSTER *f);
int e_exec_make(FENSTER *f);
int e_run_sh(FENSTER *f);
int e_project(FENSTER *f);
int e_open_project_file(ECNT *cn);
int e_select_project(ECNT *cn, char *path);
int e_open_picked_project(ECNT *cn, char *path);
int e_open_named_file(ECNT *cn, char *name);
int e_create_project(ECNT *cn, char *path);
int e_new_project(FENSTER *f);
int e_find_project_window(ECNT *cn);
int e_find_dirty_project_window(ECNT *cn);
int e_project_is_open(void);
const char *e_prj_file_name(void);
void e_prj_folder(char *out, size_t sz);
int e_p_mess_win(char *header, int argc, char **argv, PIC **pic, FENSTER *f);
int e_p_add_df(FLWND *fw, int sw);
int e_p_del_df(FLWND *fw, int sw);
int e_p_edit_df(FLWND *fw, int sw);
int e_d_car_ret(FENSTER *f);
int e_d_car_mouse(FENSTER *f);
void e_d_note_messages_focus_click(FENSTER *f);
int e_messages_return(FENSTER *f);
int e_add_arg(char ***arg, char *str, int n, int argc);
int e_new_message(FENSTER *f);
int e_project_name(FENSTER *f);
int e_wrt_prj_fl(FENSTER *f);
int e_p_update_prj_fl(FENSTER *f);
int e_prj_persist(FENSTER *f);
int e_prj_save_before_close(FENSTER *f);
int freedfN(struct dirfile **df, int n);
int e_read_var(FENSTER *f, WpeReadOutcome *why);
/* The two ways e_read_var fails: the file was refused by the read owner
   (@why says why, nothing was said) and memory ran out (said). */
#define E_PRJ_READ_NOT_OPENED (-1)
#define E_PRJ_READ_LOWMEM     (-2)
/* The box for a project the owner refused to read, or the system did. */
void e_prj_say_not_opened(FENSTER *f, const char *path, const WpeReadOutcome *why);
/* The open project's file resolved when the editor read it, the place its
   conversation is kept for; NULL when the editor holds no file as the open
   project (a project named and never read). */
const char *e_prj_hold_place(void);

/*   we_progn.c  */

int e_scfbol(int n, int mcsw, const unsigned char *str, struct wpeSyntaxRule *cs);
int e_syntax_row_continues(const BUFFER *b, int y);
int e_syntax_row_leaves(const BUFFER *b, int y, int state);
int e_sc_all(FENSTER *f, int sw);
int e_program_opt(FENSTER *f);
int e_sc_nw_txt(int y, BUFFER *b, int sw);
int *e_sc_txt(int *c_sw, BUFFER *b);
void e_pr_c_line(int y, FENSTER *f);
int e_add_synt_tl(char *filename, FENSTER *f);
E_AFILE *e_aopen(char *name, char *path, int mode, char **found);
int e_aclose(E_AFILE *ep);
char *e_agets(char *str, int n, E_AFILE *ep);
/*   we_shtok.c  */
/* What e_sh_walk looks for (the index into the table of we_shtok.c). */
#define E_SH_NEXT_TOKEN      0  /* the next token; a backslash joins a line */
#define E_SH_NEXT_TOKEN_EOL  1  /* the same; the end of a line joins too */
#define E_SH_CLOSE_BRACE     2  /* the matching close brace */
#define E_SH_LIST_END        3  /* the next top-level comma, ; or ( */
#define E_SH_CLOSE_PAREN     4  /* the matching close parenthesis */
char *e_sh_walk(int kind, char *sp, char *str, E_AFILE *fp, int *n);
/* A preprocessor line that opens the other side of an #if, and the one that
   closes it; the walkers skip the unused side. */
#define ispelse(s) ( !strncmp(s, "#else", 5) || !strncmp(s, "#elif", 5) )
#define ispendif(s) ( !strncmp(s, "#endif", 6) )
/* A preprocessor line that opens an #if block. */
#define ispif(s) ( !strncmp(s, "#if", 3) || !strncmp(s, "#ifdef", 6)  \
			|| !strncmp(s, "#ifndef", 7) )
/*   we_brkmatch.c  */
/* The partner of the bracket at b->b.  Return: 0 and its row and column, or
   -1 when there is none. */
int e_brk_match(BUFFER *b, int *my, int *mx);
struct dirfile *e_c_add_df(char *str, struct dirfile *df);
/* @file receives the path of the file the definition is in and must hold
   WPE_PATH_SIZE bytes. */
int e_find_def(char *name, char *startfile, int mode, char *file,
  int *num, int *xn, int nold, char *oldfile, struct dirfile **df,
  int *first);
int e_show_nm_f(char *name, FENSTER *f, int oldn, char **oldname);
int e_sh_def(FENSTER *f);
int e_sh_nxt_def(FENSTER *f);
int e_nxt_brk(FENSTER *f);
int e_mk_beauty(int sw, int ndif, FENSTER *f);
int e_p_beautify(FENSTER *f);

/*   we_fl_unix.c  */

int e_funct(FENSTER *f);
int e_funct_in(FENSTER *f);
int e_data_first(int sw, ECNT *cn, char *nstr);
int e_data_schirm(FENSTER *f);
int e_data_eingabe(ECNT *cn);

#endif


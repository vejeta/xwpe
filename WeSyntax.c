/*-------------------------------------------------------------------------*\
  <WeSyntax.c> -- Xwpe routines for syntax highlighting support

  Date      Programmer  Description
  05/24/97  Dennis      Created based on functions from "we_progn.c".
\*-------------------------------------------------------------------------*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Original header of "we_progn.c"
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* we_progn.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Includes
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "Xwpe.h"
#include "WeExpArr.h"
#include "WeProg.h"
#include "WeString.h"
#include "we_places.h"
#include "we_fmt.h"

unsigned char *WpeCReservedWord[] = {
 "auto", "break", "case", "char", "const", "continue", "default", "do",
 "double", "else", "enum", "extern", "float", "for", "goto", "if", "int",
 "long", "register", "return", "short", "signed", "sizeof", "static",
 "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
 "while", NULL
};

#define WpeCLongOperator NULL

unsigned char *WpeCxxReservedWord[] = {
 "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
 "catch", "char", "class", "compl", "const", "const_cast", "continue",
 "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
 "explicit", "extern", "false", "float", "for", "friend", "goto", "if",
 "inline", "int", "long", "mutable", "namespace", "new", "not", "not_eq",
 "operator", "or", "or_eq", "private", "protected", "public", "register",
 "reinterpret_cast", "return", "short", "signed", "sizeof", "static",
 "static_cast", "struct", "switch", "template", "this", "throw", "true",
 "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
 "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", NULL
};

#define WpeCxxLongOperator NULL

WpeSyntaxRule WpeCSyntaxRule = {
 WpeCReservedWord, WpeCLongOperator, "~^()[]{}<>+-/*%=|&!.?:,;", "/*", "*/",
 "", '\"', '\'', '#', '\\', '\\', '\0', -1, 1000, -1, ""
};

WpeSyntaxRule WpeCxxSyntaxRule = {
 WpeCxxReservedWord, WpeCxxLongOperator, "~^()[]{}<>+-/*%=|&!.?:,;", "/*",
 "*/", "//", '\"', '\'', '#', '\\', '\\', '\0', -1, 1000, -1, ""
};

/*WpeSyntaxExt WpeCSyntaxExt   = { ".c", &WpeCSyntaxRule };
WpeSyntaxExt WpeCxxSyntaxExt = { ".C", &WpeCxxSyntaxRule };
WpeSyntaxExt WpeHSyntaxExt   = { ".h", &WpeCxxSyntaxRule };*/

/* Definition of all programming language syntax (Note: This is an expandable
  array and must be destroyed with WpeExpArrayDestroy()) */
WpeSyntaxExt **WpeSyntaxDef = NULL;


int WpeReservedWordCompare(const void *x, const void *y)
{
 return (strcmp(*((char **)x), *((char **)y)));
}

/**
 * WpeSyntaxGetPersonal - The user's own syntax file, "~/.xwpe/syntax_def".
 * @filename/@n: receive the path.
 *
 * The user's file is read in preference to the system's, so a home folder
 * the system cannot hold (a long temporary folder, say) gives no personal
 * file rather than the file of a cut folder.  With no home folder known
 * there is no personal file: never one in the root folder, and never one in
 * the folder the editor was started in, which may be a cloned project's.
 * Return: 0, or -1 with @filename empty when there is no home or the path
 * does not fit.
 */
static int WpeSyntaxGetPersonal(char *filename, size_t n)
{
#ifdef DJGPP
 return wpe_path_copy(filename, n, "./" SYNTAX_FILE);
#else
 char home[WPE_PATH_SIZE];

 if (wpe_user_home(home, sizeof home) != 0)
 {
  if (n)
   filename[0] = '\0';
  return -1;
 }
 if (wpe_path_join(filename, n, home, XWPE_HOME) ||
     wpe_path_cat(filename, n, "/" SYNTAX_FILE))
 {
  if (n)
   filename[0] = '\0';
  return -1;
 }
 return 0;
#endif
}

/**
 * WpeSyntaxGetSystem - The syntax file installed with the editor.
 * @filename/@n: receive the path.
 * Return: 0, or -1 with @filename empty when the path does not fit.
 */
static int WpeSyntaxGetSystem(char *filename, size_t n)
{
 return wpe_path_join(filename, n, e_lib_dir(), SYNTAX_FILE);
}

/**
 * WpeSyntaxOpenPersonal - Open the user's own syntax file, if there is one.
 * @buf/@n: scratch for its path.
 * Return: the open file, or NULL.
 */
static FILE *WpeSyntaxOpenPersonal(char *buf, size_t n)
{
 if (WpeSyntaxGetPersonal(buf, n))
  return NULL;
 return fopen(buf, "r");
}

/**
 * WpeSyntaxOpenSystem - Open the syntax file installed with the editor.
 * @buf/@n: scratch for its path.
 * Return: the open file, or NULL.
 */
static FILE *WpeSyntaxOpenSystem(char *buf, size_t n)
{
 if (WpeSyntaxGetSystem(buf, n))
  return NULL;
 return fopen(buf, "r");
}

/**
 * WpeSyntaxAddBuiltinRules - The C and C++ rules the editor carries, for a
 * machine with no syntax_def file at all, so a source file is still
 * highlighted.
 */
static void WpeSyntaxAddBuiltinRules(void)
{
 WpeSyntaxExt *new_syntax;

 /* C Syntax (".c" extension) */
 new_syntax = WpeMalloc(sizeof(WpeSyntaxExt));
 new_syntax->extension = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 new_syntax->extension[0] = strdup(".c");
 new_syntax->syntax_rule = &WpeCSyntaxRule;
 WpeExpArrayAdd((void **)&WpeSyntaxDef, &new_syntax);
 /* C++ Syntax (".C", ".cpp", ".cxx", ".cc", ".h", and ".hpp" extensions) */
 new_syntax = WpeMalloc(sizeof(WpeSyntaxExt));
 new_syntax->extension = (char **)WpeExpArrayCreate(6, sizeof(char *), 1);
 new_syntax->extension[0] = strdup(".C");
 new_syntax->extension[1] = strdup(".cpp");
 new_syntax->extension[2] = strdup(".cxx");
 new_syntax->extension[3] = strdup(".cc");
 new_syntax->extension[4] = strdup(".h");
 new_syntax->extension[5] = strdup(".hpp");
 new_syntax->syntax_rule = &WpeCxxSyntaxRule;
 WpeExpArrayAdd((void **)&WpeSyntaxDef, &new_syntax);
 /*WpeSyntaxDef[2] = &WpeHSyntaxExt;*/
}

/** How many bytes one word of a syntax_def may take, with its terminator. */
#define WPE_SYNTAX_WORD_SIZE 4096

/** What the user reads when a word of the syntax_def does not fit; the
 *  first %s says which file ("personal" or "installed"), the second names it. */
#define WPE_SYNTAX_WORD_TOO_LONG_FMT "A word of the %s syntax_def is too long: "

/** Room for that sentence with the file's path in it. */
#define WPE_SYNTAX_MSG_SIZE 1024

/** Columns an error box keeps for its frame and the margin of the screen. */
#define WPE_SYNTAX_BOX_MARGIN 12

/** The least of the path that is worth showing when the screen is narrow. */
#define WPE_SYNTAX_PATH_MIN_SHOWN 8

/** The file being read: the open file, which of the two it is, and its path. */
typedef struct
{
 FILE *file;
 const char *which;
 const char *path;
} WpeSyntaxSource;

/** The answers of WpeSyntaxReadWord. */
#define WPE_SYNTAX_WORD_OK 1
#define WPE_SYNTAX_WORD_END 0
#define WPE_SYNTAX_WORD_LONG (-1)

/**
 * WpeSyntaxReadWord - Read the next blank separated word of a syntax_def.
 * @syntax_file: the open file; @word/@n: the buffer and its size.
 *
 * A word longer than the buffer is read to its end and refused, never cut
 * and never written past the buffer, so a damaged or hostile personal
 * syntax_def cannot end the editor when it starts.
 * Return: WPE_SYNTAX_WORD_OK with the word in @word, WPE_SYNTAX_WORD_END at
 * the end of the file, WPE_SYNTAX_WORD_LONG with @word empty.
 */
static int WpeSyntaxReadWord(FILE *syntax_file, char *word, size_t n)
{
 size_t len = 0;
 int c, too_long = 0;

 word[0] = '\0';
 do
  c = getc(syntax_file);
 while (c != EOF && isspace(c));
 if (c == EOF)
  return WPE_SYNTAX_WORD_END;
 while (c != EOF && !isspace(c))
 {
  if (len + 1 < n)
   word[len++] = (char)c;
  else
   too_long = 1;
  c = getc(syntax_file);
 }
 if (c != EOF)
  ungetc(c, syntax_file);
 word[len] = '\0';
 if (too_long)
 {
  word[0] = '\0';
  return WPE_SYNTAX_WORD_LONG;
 }
 return WPE_SYNTAX_WORD_OK;
}

/**
 * WpeSyntaxSayWordTooLong - Tell the user which syntax_def has a word the
 * editor cannot hold.
 * @cn: part of the old code e_error needs; @src: the file being read.
 *
 * A user with a personal file and the installed one needs to know which to
 * edit: the box names the kind and the path, shortened at its front so the
 * box stays within the screen.
 */
static void WpeSyntaxSayWordTooLong(ECNT *cn, const WpeSyntaxSource *src)
{
 char head[WPE_SYNTAX_MSG_SIZE];
 char msg[WPE_SYNTAX_MSG_SIZE];
 char shown[WPE_SYNTAX_MSG_SIZE];
 int room;

 snprintf(head, sizeof head, WPE_SYNTAX_WORD_TOO_LONG_FMT, src->which);
 room = MAXSCOL - WPE_SYNTAX_BOX_MARGIN - (int)strlen(head);
 if (room < WPE_SYNTAX_PATH_MIN_SHOWN)
  room = WPE_SYNTAX_PATH_MIN_SHOWN;
 /* The path loses its front, never the file's own name, and the cut is
    marked with "..." where a character starts. */
 wpe_fmt_shown_tail(shown, (size_t)room + 1, src->path);
 snprintf(msg, sizeof msg, "%s%s", head, shown);
 e_error(msg, 0, cn->fb);
}

/**
 * WpeSyntaxNeedWord - Read a word the entry in the file cannot do without.
 * @cn: part of the old code e_error needs; @src: the file being read;
 * @word, @n: as WpeSyntaxReadWord.
 *
 * Says in a box why the entry is given up: the file ends early, or a word
 * is longer than the editor holds.
 * Return: 1 with the word, 0 after the box.
 */
static int WpeSyntaxNeedWord(ECNT *cn, const WpeSyntaxSource *src, char *word, size_t n)
{
 int state = WpeSyntaxReadWord(src->file, word, n);

 if (state == WPE_SYNTAX_WORD_OK)
  return 1;
 if (state == WPE_SYNTAX_WORD_LONG)
  WpeSyntaxSayWordTooLong(cn, src);
 else
  e_error("Error reading syntax_def", 0, cn->fb);
 return 0;
}

/**
 * WpeSyntaxParseFile - Read every language of an open syntax_def into
 * WpeSyntaxDef.
 * @cn: part of the old code e_error needs; @src: the open file with its
 * kind and path.
 *
 * Stops at the first entry that is cut short and says so; the languages
 * read before it stay.  Does not close
 * the file: whoever opened it does, once, so no return path can leave the
 * editor holding the file for as long as it runs (and handing it to every
 * program it starts).
 */
static void WpeSyntaxParseFile(ECNT *cn, const WpeSyntaxSource *src)
{
 FILE *syntax_file = src->file;
 WpeSyntaxExt *new_syntax;
 char tmp[WPE_SYNTAX_WORD_SIZE];
 int reserved_num, long_op_num;
 int i, k, state;

 while ((state = WpeSyntaxReadWord(syntax_file, tmp, sizeof tmp)) ==
        WPE_SYNTAX_WORD_OK)
 {
  new_syntax = WpeMalloc(sizeof(WpeSyntaxExt));
  i = atoi(tmp);
  if (i > 0)
  {
   new_syntax->extension = (char **)WpeExpArrayCreate(i, sizeof(char *), 1);
   for (k = 0; k < i; k++)
   {
    if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
     return ;
    new_syntax->extension[k] = WpeStrdup(tmp);
   }
  }
  else
  {
   new_syntax->extension = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
   new_syntax->extension[0] = WpeStrdup(tmp);
  }
  new_syntax->syntax_rule = WpeMalloc(sizeof(WpeSyntaxRule));
  if (fscanf(syntax_file, "%d", &reserved_num) != 1)
  {
   e_error("Error reading syntax_def", 0, cn->fb);
   return ;
  }
  new_syntax->syntax_rule->reserved_word = WpeMalloc((reserved_num + 1) *
                                                     sizeof(char *));
  for (i = 0; i < reserved_num; i++)
  {
   if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
    return ;
   new_syntax->syntax_rule->reserved_word[i] = WpeStrdup(tmp);
  }
  new_syntax->syntax_rule->reserved_word[i] = NULL;
  if (fscanf(syntax_file, "%d", &long_op_num) != 1)
  {
   e_error("Error reading syntax_def", 0, cn->fb);
   return ;
  }
  new_syntax->syntax_rule->long_operator = WpeMalloc((long_op_num + 1) *
                                                     sizeof(char *));
  for (i = 0; i < long_op_num; i++)
  {
   if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
    return ;
   new_syntax->syntax_rule->long_operator[i] = WpeStrdup(tmp);
  }
  new_syntax->syntax_rule->long_operator[i] = NULL;
  if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
   return ;
  new_syntax->syntax_rule->single_operator = WpeStrdup(strcmp(tmp, "NULL") ? tmp : "");
  if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
   return ;
  new_syntax->syntax_rule->begin_comment = WpeStrdup(strcmp(tmp, "NULL") ? tmp : "");
  if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
   return ;
  new_syntax->syntax_rule->end_comment = WpeStrdup(strcmp(tmp, "NULL") ? tmp : "");
  if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
   return ;
  new_syntax->syntax_rule->line_comment = WpeStrdup(strcmp(tmp, "NULL") ? tmp : "");
  if (!WpeSyntaxNeedWord(cn, src, tmp, sizeof tmp))
   return ;
  new_syntax->syntax_rule->special_comment = WpeStrdup(strcmp(tmp, "NULL") ? tmp : "");
  if (fscanf(syntax_file, " %c%c%c%c%c%c",
             &new_syntax->syntax_rule->string_constant,
             &new_syntax->syntax_rule->char_constant,
             &new_syntax->syntax_rule->preproc_cmd,
             &new_syntax->syntax_rule->quoting_char,
             &new_syntax->syntax_rule->continue_char,
             &new_syntax->syntax_rule->insensitive) != 6)
  {
   e_error("Error reading syntax_def", 0, cn->fb);
   return ;
  }
  /* This is currently a mess but this format of syntax_def is unlikely to
    stay - Dennis */
  if (new_syntax->syntax_rule->string_constant == ' ')
  {
   new_syntax->syntax_rule->string_constant = '\0';
  }
  if (new_syntax->syntax_rule->char_constant == ' ')
  {
   new_syntax->syntax_rule->char_constant = '\0';
  }
  if (new_syntax->syntax_rule->preproc_cmd == ' ')
  {
   new_syntax->syntax_rule->preproc_cmd = '\0';
  }
  if (new_syntax->syntax_rule->quoting_char == ' ')
  {
   new_syntax->syntax_rule->quoting_char = '\0';
  }
  if (new_syntax->syntax_rule->continue_char == ' ')
  {
   new_syntax->syntax_rule->continue_char = '\0';
  }
  if (new_syntax->syntax_rule->insensitive == ' ')
  {
   new_syntax->syntax_rule->insensitive = '\0';
  }
  else
  {
   /* Convert all reserved words to upper case for case insensitive syntax */
   for (i = 0; new_syntax->syntax_rule->reserved_word[i]; i++)
   {
    WpeStringToUpper(new_syntax->syntax_rule->reserved_word[i]);
   }
   /* Convert all long operators to upper case for case insensitive syntax */
   for (i = 0; new_syntax->syntax_rule->long_operator[i]; i++)
   {
    WpeStringToUpper(new_syntax->syntax_rule->long_operator[i]);
   }
  }
  if (fscanf(syntax_file, "%d%d%d", &new_syntax->syntax_rule->special_column,
             &new_syntax->syntax_rule->continue_column,
             &new_syntax->syntax_rule->comment_column) != 3)
  {
   e_error("Error reading syntax_def", 0, cn->fb);
   return ;
  }
  qsort(new_syntax->syntax_rule->reserved_word, reserved_num, sizeof(char *),
        WpeReservedWordCompare);
  qsort(new_syntax->syntax_rule->long_operator, long_op_num, sizeof(char *),
        WpeReservedWordCompare);
  WpeExpArrayAdd((void **)&WpeSyntaxDef, &new_syntax);
 }
 if (state == WPE_SYNTAX_WORD_LONG)
  WpeSyntaxSayWordTooLong(cn, src);
}

/**
 * WpeSyntaxReadFile - Read the programming language syntax file at start-up:
 * the user's own, else the one installed, else the built-in C and C++ rules.
 * @cn: part of the old code e_error needs.
 *
 * The file is closed here, once, however the parser ends, so the editor does
 * not keep it open while it runs.
 */
void WpeSyntaxReadFile(ECNT *cn)
{
 WpeSyntaxSource src;
 char tmp[WPE_PATH_SIZE];

 WpeSyntaxDef = (WpeSyntaxExt **)WpeExpArrayCreate(0, sizeof(WpeSyntaxExt *), 4);
 src.which = "personal";
 src.file = WpeSyntaxOpenPersonal(tmp, sizeof tmp);
 if (src.file == NULL)
 {
  src.which = "installed";
  src.file = WpeSyntaxOpenSystem(tmp, sizeof tmp);
 }
 if (src.file == NULL)
 {
  WpeSyntaxAddBuiltinRules();
  return ;
 }
 src.path = tmp;
 WpeSyntaxParseFile(cn, &src);
 fclose(src.file);
}


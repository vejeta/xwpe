/* test_fl_sel_len.c                                      */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* Regression guard for the file-manager selected-name width helper.
   The file manager sizes its horizontal scrollbar from the length of the
   highlighted entry. On an empty directory the entry array has no valid
   element, so measuring name[nf] reads past its end -- clicking the
   scrollbar of an empty panel used to do exactly that. e_fl_sel_len() must
   return 0 for an empty or out-of-range selection WITHOUT dereferencing the
   array. The name array here is heap-allocated to the exact entry count, so
   under AddressSanitizer a removed guard turns into a caught heap overflow
   rather than silent undefined behaviour. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ(got, expect, msg) do { \
    tests_run++; \
    if ((got) == (expect)) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s (got %d, expected %d)\n", msg, \
                  (int)(got), (int)(expect)); } \
} while (0)

/* Minimal stand-ins for the two production structs e_fl_sel_len touches.
   Field names and meaning mirror `struct dirfile` and `FLWND` in edit.h. */
struct dirfile {
 int anz;       /* number of elements in the list */
 char **name;   /* the list elements */
};

typedef struct fl_wnd {
 int nf;              /* selected field in the dirfile */
 struct dirfile *df;  /* the file/directory list */
} FLWND;

/* Reference copy of the production helper (we_fl_fkt.c). Kept in sync by
   hand, exactly like test_checkheader.c embeds e_check_header_test: the
   logic is small enough that the copy is the specification. */
static int e_fl_sel_len(FLWND *fw)
{
   if(fw->df->anz <= 0) return(0);
   if(fw->nf < 0 || fw->nf >= fw->df->anz) return(0);
   return(strlen(*(fw->df->name + fw->nf)));
}

/* Build a dirfile whose name array is sized to exactly `count` entries, so a
   read at index `count` (or beyond) lands outside the allocation. */
static struct dirfile *make_dirfile(int count, char **entries)
{
 struct dirfile *df = malloc(sizeof(*df));
 int i;
 df->anz = count;
 df->name = count > 0 ? malloc(count * sizeof(char *)) : malloc(1);
 for (i = 0; i < count; i++)
  df->name[i] = entries[i];
 return df;
}

static void free_dirfile(struct dirfile *df)
{
 free(df->name);
 free(df);
}

static void test_empty_panel_returns_zero(void)
{
 FLWND fw;
 printf("test_empty_panel_returns_zero:\n");
 fw.nf = 0;
 fw.df = make_dirfile(0, NULL);
 ASSERT_EQ(e_fl_sel_len(&fw), 0, "empty directory -> 0, no name[] read");
 free_dirfile(fw.df);
}

static void test_selected_name_length(void)
{
 char *entries[3];
 FLWND fw;
 printf("test_selected_name_length:\n");
 entries[0] = "a.c";
 entries[1] = "readme.txt";
 entries[2] = "x";
 fw.df = make_dirfile(3, entries);
 fw.nf = 0;
 ASSERT_EQ(e_fl_sel_len(&fw), 3, "first entry 'a.c' -> 3");
 fw.nf = 1;
 ASSERT_EQ(e_fl_sel_len(&fw), 10, "second entry 'readme.txt' -> 10");
 fw.nf = 2;
 ASSERT_EQ(e_fl_sel_len(&fw), 1, "third entry 'x' -> 1");
 free_dirfile(fw.df);
}

static void test_stale_index_past_end(void)
{
 char *entries[1];
 FLWND fw;
 printf("test_stale_index_past_end:\n");
 entries[0] = "only.c";
 fw.df = make_dirfile(1, entries);
 fw.nf = 1;  /* list shrank to 1 entry but nf still points at the old row */
 ASSERT_EQ(e_fl_sel_len(&fw), 0, "nf == anz -> 0, no read past the array");
 fw.nf = 99;
 ASSERT_EQ(e_fl_sel_len(&fw), 0, "nf far past end -> 0");
 free_dirfile(fw.df);
}

static void test_negative_index(void)
{
 char *entries[1];
 FLWND fw;
 printf("test_negative_index:\n");
 entries[0] = "only.c";
 fw.df = make_dirfile(1, entries);
 fw.nf = -1;
 ASSERT_EQ(e_fl_sel_len(&fw), 0, "negative nf -> 0, no name[-1] read");
 free_dirfile(fw.df);
}

int main(void)
{
 test_empty_panel_returns_zero();
 test_selected_name_length();
 test_stale_index_past_end();
 test_negative_index();

 printf("\n%d/%d tests passed\n", tests_passed, tests_run);
 return tests_passed == tests_run ? 0 : 1;
}

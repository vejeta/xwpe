/*
 * test_syntax_def.c -- Verify syntax_def parses without errors.
 *
 * Mimics the fscanf loop in WeSyntax.c:WpeSyntaxReadFile() to ensure
 * every entry in syntax_def is well-formed: correct keyword counts,
 * long-operator counts, 5 string fields, 6 config chars, and 3 ints.
 *
 * Returns 0 (pass) if all entries parse, 1 (fail) on any error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_syntax_file(const char *path)
{
    FILE *f;
    char tmp[256];
    int entry = 0;
    int i, k, ext_count, kw_count, lo_count;
    char c1, c2, c3, c4, c5, c6;
    int i1, i2, i3;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return 1;
    }

    while (fscanf(f, "%255s", tmp) == 1) {
        entry++;
        i = atoi(tmp);

        if (i > 0) {
            ext_count = i;
            for (k = 0; k < ext_count; k++) {
                if (fscanf(f, "%255s", tmp) != 1) {
                    fprintf(stderr, "FAIL: entry %d: cannot read extension %d\n", entry, k);
                    fclose(f);
                    return 1;
                }
            }
        }

        if (fscanf(f, "%d", &kw_count) != 1) {
            fprintf(stderr, "FAIL: entry %d: cannot read keyword count\n", entry);
            fclose(f);
            return 1;
        }
        for (k = 0; k < kw_count; k++) {
            if (fscanf(f, "%255s", tmp) != 1) {
                fprintf(stderr, "FAIL: entry %d: cannot read keyword %d/%d\n", entry, k, kw_count);
                fclose(f);
                return 1;
            }
        }

        if (fscanf(f, "%d", &lo_count) != 1) {
            fprintf(stderr, "FAIL: entry %d: cannot read long-operator count\n", entry);
            fclose(f);
            return 1;
        }
        for (k = 0; k < lo_count; k++) {
            if (fscanf(f, "%255s", tmp) != 1) {
                fprintf(stderr, "FAIL: entry %d: cannot read long-op %d/%d\n", entry, k, lo_count);
                fclose(f);
                return 1;
            }
        }

        for (k = 0; k < 5; k++) {
            if (fscanf(f, "%255s", tmp) != 1) {
                fprintf(stderr, "FAIL: entry %d: cannot read string field %d\n", entry, k);
                fclose(f);
                return 1;
            }
        }

        if (fscanf(f, " %c%c%c%c%c%c", &c1, &c2, &c3, &c4, &c5, &c6) != 6) {
            fprintf(stderr, "FAIL: entry %d: cannot read 6 config chars\n", entry);
            fclose(f);
            return 1;
        }

        if (fscanf(f, "%d%d%d", &i1, &i2, &i3) != 3) {
            fprintf(stderr, "FAIL: entry %d: cannot read 3 config ints\n", entry);
            fclose(f);
            return 1;
        }
    }

    fclose(f);

    if (entry == 0) {
        fprintf(stderr, "FAIL: no entries found in %s\n", path);
        return 1;
    }

    printf("PASS: %d entries parsed from %s\n", entry, path);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *path;

    if (argc > 1) {
        path = argv[1];
    } else {
        path = "syntax_def";
    }

    return parse_syntax_file(path);
}

/* we_features.c -- what this build contains, said once, from config.h.
   See we_features.h. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include "we_features.h"

/* The words are configure's options; the order is the order configure.ac
   states the facts in (WPE_BUILT lines).  tests/check-build-config.sh fails
   when this table and config.h disagree. */
static const WpeFeature wpe_feature_table[] = {
 { "enable-ai",            WPE_BUILT_AI },
 { "enable-ai-agent-host", WPE_BUILT_AI_HOST },
 { "enable-ai-tls",        WPE_BUILT_AI_TLS },
 { "with-x",               WPE_BUILT_X11 },
 { "with-wayland",         WPE_BUILT_WAYLAND },
 { "with-gpm",             WPE_BUILT_GPM },
 { "with-xdb",             WPE_BUILT_XDB },
 { "enable-trace",         WPE_BUILT_TRACE },
 { "libvterm",             WPE_BUILT_VTERM },
 { "xft",                  WPE_BUILT_XFT },
 { "cairo",                WPE_BUILT_CAIRO },
 { "pango",                WPE_BUILT_PANGO }
};

#define WPE_FEATURE_COUNT (sizeof wpe_feature_table / sizeof wpe_feature_table[0])
#define WPE_FEATURES_PREFIX "built with:"
#define WPE_FEATURES_LINE_MAX 512

/**
 * wpe_features - The table of what configure was asked for and found.
 * @n: receives the number of entries.
 *
 * One entry per WPE_BUILT_* fact, for --version and for anything that must
 * say which build this is.
 *
 * Return: the table.
 */
const WpeFeature *wpe_features(size_t *n)
{
 if (n != NULL)
  *n = WPE_FEATURE_COUNT;
 return(wpe_feature_table);
}

/**
 * wpe_features_line - "built with: word=yes word=no ...", on one line.
 *
 * What `wpe --version` prints after the version, so that a test suite or a
 * bug report learns what the build contains from the binary itself.
 *
 * Return: a string owned by this file, valid for the life of the process.
 */
const char *wpe_features_line(void)
{
 static char line[WPE_FEATURES_LINE_MAX];
 size_t i, used;
 int len;

 if (line[0] != '\0')
  return(line);
 used = strlen(WPE_FEATURES_PREFIX);
 memcpy(line, WPE_FEATURES_PREFIX, used + 1);
 for (i = 0; i < WPE_FEATURE_COUNT; i++)
 {
  len = snprintf(line + used, sizeof line - used, " %s=%s",
                 wpe_feature_table[i].word,
                 wpe_feature_table[i].built ? "yes" : "no");
  if (len < 0 || (size_t)len >= sizeof line - used)
   break;
  used += (size_t)len;
 }
 return(line);
}

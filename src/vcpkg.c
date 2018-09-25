/**\file    vcpkg.c
 * \ingroup Misc
 *
 * \brief
 *   An interface for Microsoft's Package Manager VCPKG.
 *   https://github.com/Microsoft/vcpkg
 */
#include "envtool.h"
#include "smartlist.h"
#include "color.h"
#include "dirlist.h"
#include "regex.h"
#include "vcpkg.h"

/**
 * `CONTROL` file keywords we look for:
 *
 * \def CONTROL_DESCRIPTION
 *      the descriptions of a package follows this.
 *
 * \def CONTROL_SOURCE
 *      the source-name is the name of a package following this.
 *
 * \def CONTROL_VERSION
 *      the version-info of a package follows this.
 *
 * \def CONTROL_BUILD_DEPENDS
 *      the list of packages this package depends on.
 */
#define CONTROL_DESCRIPTION   "Description:"
#define CONTROL_SOURCE        "Source:"
#define CONTROL_VERSION       "Version:"
#define CONTROL_BUILD_DEPENDS "Build-Depends:"

/** The list of `CONTROL` and `portfile.cmake` file entries.
 */
static smartlist_t *vcpkg_nodes;

/** The list of packages found in `CONTROL` files.
 * This is a list of `struct vcpkg_depend`.
 */
static smartlist_t *vcpkg_packages;

/** Save nodes releative to this directory to save memory.
 */
static char vcpkg_base_dir [_MAX_PATH];

/** Save last error-text here.
 */
static char vcpkg_err_str [200];

/**
 * regex stuff
 */
static regex_t    re_hnd;
static regmatch_t re_matches[3];  /* regex sub-expressions */
static int        re_err;         /* last regex error-code */
static char       re_errbuf[10]; /* regex error-buffer */

static void regex_print (const regex_t *re, const regmatch_t *rm, const char *str)
{
  size_t i, j;

  C_puts ("sub-expr: ");
  for (i = 0; i < re->re_nsub; i++, rm++)
  {
    for (j = 0; j < strlen(str); j++)
    {
      if (j >= (size_t)rm->rm_so && j <= (size_t)rm->rm_eo)
           C_printf ("~5%c", str[j]);
      else C_printf ("~0%c", str[j]);
    }
  }
  if (i == 0)
     C_puts ("None");
  C_putc ('\n');
}

static void regex_free (void)
{
  if (re_hnd.buffer)
     regfree (&re_hnd);
  re_hnd.buffer = NULL;
}

/*
 * Try to match 'str' against the regular expression in 'pattern'.
 */
static BOOL regex_match (const char *str, const char *pattern)
{
  memset (&re_matches, '\0', sizeof(re_matches));
  if (!re_hnd.buffer)
  {
    re_err = regcomp (&re_hnd, pattern, REG_EXTENDED | REG_ICASE);
    if (re_err)
    {
      regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
      WARN ("Invalid regular expression \"%s\": %s (%d)\n", pattern, re_errbuf, re_err);
      regex_free();
      return (FALSE);
    }
  }

  re_err = regexec (&re_hnd, str, DIM(re_matches), re_matches, 0);
  DEBUGF (1, "regex() pattern '%s' against '%s'. re_err: %d\n", pattern, str, re_err);

  if (re_err == REG_NOMATCH)
     return (FALSE);

  if (re_err == REG_NOERROR)
     return (TRUE);

  regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
  DEBUGF (0, "Error while matching \"%s\": %s (%d)\n", str, re_errbuf, re_err);
  return (FALSE);
}

static void regex_test (const char *str, const char *pattern)
{
  if (regex_match(str, pattern))
     regex_print (&re_hnd, re_matches, str);
}

/**
 * Return the parent base directory of file `fname`.
 * This should be the same as `Source: x` in a `CONTROL` file.
 */
static char *get_parent_dir (const char *full_path, char *fname)
{
  while (*fname && fname > full_path)
  {
    if (IS_SLASH(*fname))
       break;
    fname--;
  }
  return (fname+1);
}

/**
 * Print the package top-dependencies for a `CONTROL` node.
 */
static void print_top_dependencies (const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *dep;
  size_t len, longest_package = 0;
  int    i, max_i;

  C_printf ("  %-*s", indent, "dependants:");
  if (!node->deps)
  {
    C_puts ("Nothing\n");
    return;
  }

  max_i = smartlist_len (node->deps);

  /* First, get the value for 'longest_package'
   */
  for (i = 0; i < max_i; i++)
  {
    dep = smartlist_get (node->deps, i);
    len = strlen (dep->package);
    if (len > longest_package)
       longest_package = len;
  }
  for (i = 0; i < max_i; i++)
  {
    BOOL Not;
    int  p_val;

    dep   = smartlist_get (node->deps, i);
    p_val = vcpkg_get_dep_platform (dep, &Not);

    if (i > 0)
       C_printf ("  %-*s", indent, "");
    C_printf ("%-*s  platform: %s%s (0x%04X)\n",
              (int)longest_package, dep->package,
              Not ? "not " : "",
              vcpkg_get_dep_name(dep), dep->platform);
  }
}

static int sub_level  = 0;
static int sub_indent = 0;

/**
 * Print the package sub-dependencies for a `CONTROL` node.
 */
static void print_sub_dependencies (const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *dep1, *dep2;
  int   i, i_max, j, j_max, found;

  C_setraw (0);
  if (!node->deps || smartlist_len(node->deps) == 0)
  {
    C_printf ("%-*sNo sub-deps\n", 2*(indent+sub_indent), "");
    sub_indent--;
    return;
  }

  i_max = smartlist_len (node->deps);
  j_max = smartlist_len (vcpkg_packages);

  for (i = found = 0; i < i_max; i++)
  {
    dep1 = smartlist_get (node->deps, i);

    for (j = 0; j < j_max; j++)
    {
      dep2 = smartlist_get (vcpkg_packages, j);
      if (dep1->package == dep2->package)
      {
        found++;
        sub_level++;
        vcpkg_dump_control (dep1->package);
//      sub_level--;
      }
    }
  }
  if (found == 0)
     C_puts ("None found\n");
}

/**
 * Get the `deps->platform` name(s).
 */
static const struct search_list platforms[] = {
                              { VCPKG_plat_WINDOWS, "windows" },
                              { VCPKG_plat_LINUX,   "linux"   },
                              { VCPKG_plat_UWP,     "uwp"     },
                              { VCPKG_plat_ARM,     "arm"     },
                              { VCPKG_plat_ANDROID, "android" },
                              { VCPKG_plat_OSX,     "osx"     },
                              { VCPKG_plat_x64,     "x64"     },
                            };

const char *vcpkg_get_dep_name (const struct vcpkg_depend *dep)
{
  if (dep->platform == VCPKG_plat_ALL)
     return ("all");
  return flags_decode (dep->platform & ~0x8000, platforms, DIM(platforms));
}

/**
 * Get the `deps->platform` and the inverse of it.
 */
int vcpkg_get_dep_platform (const struct vcpkg_depend *dep, BOOL *Not)
{
  unsigned val = dep->platform;

  if (Not)
     *Not = FALSE;

  if (val == VCPKG_plat_ALL)
     return (VCPKG_plat_ALL);

  if (val & 0x8000) /* Sign bit set */
  {
    if (Not)
       *Not = TRUE;
    val &= ~0x8000;
  }
  return (val);
}

/**
 * Split a line like "!uwp&!windows" and recursively set
 * fill `dep` list for it.
 */
static void make_dep_platform (struct vcpkg_depend *dep, char *platform, BOOL recurse)
{
  BOOL     Not = FALSE;
  unsigned val;

  if (*platform == '!')
  {
    platform++;
    Not = TRUE;
  }

  val = list_lookup_value (platform, platforms, DIM(platforms));
  if (val != UINT_MAX)
  {
    /* Set the `deps->platform` or the inverse of it.
     */
    if (Not)
         dep->platform |= (0x8000 | (VCPKG_platform)val);
    else dep->platform |= (VCPKG_platform)val;
  }
  else if (recurse)
  {
    char *tok_end, *tok = strtok_s (platform, "&", &tok_end);

    while (tok)
    {
      make_dep_platform (dep, tok, FALSE);
      tok = strtok_s (NULL, "&", &tok_end);
    }
  }
}

/**
 * Search the global `vcpkg_packages` for a matching `dep->package`.
 * If found return a pointer to it.
 * If not found, create a new `struct vcpkg_depend` entry to the `vcpkg_packages`
 * list and return a pointer to it.
 *
 * This is to save memory; no need to call `CALLOC()` for every `vcpkg_node::deps`
 * entry in the list. Hence many `vcpkg_node::deps` entries contains pointer to the
 * same area.
 */
static void *find_or_alloc_dependency (const struct vcpkg_depend *dep1)
{
  struct vcpkg_depend *dep2;
  int    i, max = smartlist_len (vcpkg_packages);

  for (i = 0; i < max; i++)
  {
    dep2 = smartlist_get (vcpkg_packages, i);
    if (!memcmp(dep2, dep1, sizeof(*dep2)))
       return (dep2);
  }
  dep2 = CALLOC (sizeof(*dep2), 1);
  memcpy (dep2, dep1, sizeof(*dep2));
  smartlist_add (vcpkg_packages, dep2);
  return (dep2);
}

/**
 * Split a line like:
 *   "openssl (!uwp&!windows), curl (!uwp&!windows)"
 *
 * first into tokens of:
 *   "openssl (!uwp&!windows)" and "curl (!uwp&!windows)".
 *
 * If a token contains a "(xx)" part, pass that to make_dep_platform()
 * which recursively figures out the platforms for the package.
 *
 * Add a package-dependency  to `node` as long as there are more ","
 * tokens in `str` to parse.
 */
static void make_dependencies (struct vcpkg_node *node, char *str)
{
  char *tok, *tok_end, *p;

  if (strchr(str, ')') > strchr(str, '('))
     DEBUGF (2, "str: '%s'\n", str);

  ASSERT (node->deps == NULL);
  node->deps = smartlist_new();

  tok = strtok_s (str, ",", &tok_end);

  while (tok)
  {
    struct vcpkg_depend dep;
    char   package [2*VCPKG_MAX_NAME];
    char   platform [51];
    char  *l_paren;

    memset (&dep, '\0', sizeof(dep));

    p = str_trim (tok);
    p = _strlcpy (package, p, sizeof(package));

    l_paren = strchr (package, '(');
    if (l_paren && sscanf(l_paren+1, "%50[^)])", platform) == 1)
    {
      *l_paren = '\0';
      p = str_trim (package);
      DEBUGF (2, "platform: '%s', tok: '%s', tok_end: '%s'\n", platform, tok, tok_end);
      make_dep_platform (&dep, platform, TRUE);
    }

    _strlcpy (dep.package, p, sizeof(dep.package));
    smartlist_add (node->deps, find_or_alloc_dependency(&dep));

    tok = strtok_s (NULL, ",", &tok_end);
  }
}

/**
 * Parse the content of a `CONTROL` file and add it's contents to `node`.
 */
static void CONTROL_parse (struct vcpkg_node *node, const char *file)
{
  FILE *f = fopen (file, "r");

  if (!f)
     return;

  while (1)
  {
    char *p, buf [5000];

    if (!fgets(buf,sizeof(buf)-1,f))   /* EOF */
       break;

    strip_nl (buf);
    p = str_ltrim (buf);

    if (!node->description && !strnicmp(p,CONTROL_DESCRIPTION,sizeof(CONTROL_DESCRIPTION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_DESCRIPTION) - 1);
      node->description = STRDUP (p);
    }
    else if (!node->package[0] && !strnicmp(p,CONTROL_SOURCE,sizeof(CONTROL_SOURCE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_SOURCE) - 1);
      _strlcpy (node->package, p, sizeof(node->package));
    }
    else if (!node->version[0] && !strnicmp(p,CONTROL_VERSION,sizeof(CONTROL_VERSION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_VERSION) - 1);
      _strlcpy (node->version, p, sizeof(node->version));
    }
    else if (!node->deps && !strnicmp(p,CONTROL_BUILD_DEPENDS,sizeof(CONTROL_BUILD_DEPENDS)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_BUILD_DEPENDS) - 1);
      if (opt.debug >= 2)
         regex_test (p, "[[:alnum:]_-]+");
      make_dependencies (node, p);
    }

    /* Quit when we've got all we need
     */
    if (node->description && node->package[0] && node->version[0])
       break;
  }
  fclose (f);
}

/**
 * Parse `file` for LOCAL package location or REMOTE package URL
 */
static void portfile_cmake_parse (struct vcpkg_node *node, const char *file)
{
  ARGSUSED (node);
  ARGSUSED (file);
}

/**
 * Recursively traverse the `%VCPKG_ROOT%/ports` directory looking for `CONTROL` and
 * `portfile.cmake` files. Parse these files and add needed information in those to
 * the `vcpkg_nodes` smartlist.
 */
static void _vcpkg_get_list (const char *dir, const struct od2x_options *opts)
{
  struct dirent2 *de;
  DIR2  *dp = opendir2x (dir, opts);

  if (!dp)
     return;

  while ((de = readdir2(dp)) != NULL)
  {
    struct vcpkg_node *node;
    char  *this_file, *this_dir, *p;
    char   file [_MAX_PATH];

    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      /* The recursion level should be limited to 2 since VCPKG seems to
       * support "CONTROL" and "portfile.cmake" files at one level below
       * "%VCPKG_ROOT%\\ports" only.
       */
      _vcpkg_get_list (de->d_name, opts);
    }
    else
    {
      this_file = basename (de->d_name);
      p = this_file-1;
      *p = '\0';
      this_dir = get_parent_dir (de->d_name, p-1);

      if (!stricmp(this_file,"CONTROL"))
      {
        node = CALLOC (sizeof(*node), 1);
        node->have_CONTROL = TRUE;
        snprintf (file, sizeof(file), "%s\\%s\\%s", vcpkg_base_dir, this_dir, this_file);
        CONTROL_parse (node, file);
        smartlist_add (vcpkg_nodes, node);
      }
      else if (!stricmp(this_file,"portfile.cmake"))
      {
        node = CALLOC (sizeof(*node), 1);
        snprintf (file, sizeof(file), "%s\\%s\\%s", vcpkg_base_dir, this_dir, this_file);
        _strlcpy (node->package, this_file, sizeof(node->package));
        portfile_cmake_parse (node, file);
        smartlist_add (vcpkg_nodes, node);
      }
    }
  }
  closedir2 (dp);
}

/**
 * Return a pointer to `vcpkg_err_str`.
 */
const char *vcpkg_last_error (void)
{
  return (vcpkg_err_str);
}

static BOOL vcpkg_get_base_env (void)
{
  const char *env = getenv ("VCPKG_ROOT");
  char       *end;

  if (!env)
  {
    _strlcpy (vcpkg_err_str, "Env-var ~5VCPKG_ROOT~0 not defined", sizeof(vcpkg_err_str));
    return (FALSE);
  }
  end = strchr (env, '\0') - 1;
  if (IS_SLASH(*end))
       snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%sports", env);
  else snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%s\\ports", env);
  return (TRUE);
}

static BOOL vcpkg_get_base_exe (void)
{
  const char *exe = searchpath ("vcpkg.exe", "PATH");
  char       *dir;

  if (!exe)
  {
    _strlcpy (vcpkg_err_str, "vcpkg.exe not on %%PATH.\n", sizeof(vcpkg_err_str));
    return (FALSE);
  }
  dir = dirname (exe);
  snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%s\\ports", dir);
  FREE (dir);
  return (TRUE);
}

/**
 * Get the VCPKG root-directory. Either based on:
 *  \li an existing directory `%VCPKG_ROOT\ports` or
 *  \li `dirname (searchpath("vcpkg.exe")) + \\ports`.
 */
static BOOL vcpkg_get_basedir (void)
{
  if (!vcpkg_get_base_env() && !vcpkg_get_base_exe())
     return (FALSE);

  if (!is_directory(vcpkg_base_dir))
  {
    snprintf (vcpkg_err_str, sizeof(vcpkg_err_str),
              "~6%s~0 points to a non-existing directory", vcpkg_base_dir);
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Build the smartlist `vcpkg_nodes`.
 */
int vcpkg_get_list (void)
{
  struct od2x_options opts;
  const char *env = getenv ("VCPKG_ROOT");
  int   len;

  if (!vcpkg_get_basedir())
     return (0);

  memset (&opts, '\0', sizeof(opts));
  opts.pattern = "*";

  ASSERT (vcpkg_nodes == NULL);
  ASSERT (vcpkg_packages == NULL);

  vcpkg_nodes = smartlist_new();
  vcpkg_packages = smartlist_new();

  _vcpkg_get_list (vcpkg_base_dir, &opts);
  len = smartlist_len (vcpkg_nodes);

  if (len == 0)
  {
    _strlcpy (vcpkg_err_str, "No ~5VCPKG~0 packages found", sizeof(vcpkg_err_str));
    vcpkg_free();
  }
  return (len);
}

/**
 * Free the memory allocated for `vcpkg_packages`.
 */
static void vcpkg_free_packages (void)
{
  int i, max;

  if (!vcpkg_packages)
     return;

  max = smartlist_len (vcpkg_packages);
  for (i = 0; i < max; i++)
  {
    struct vcpkg_depend *dep = smartlist_get (vcpkg_packages, i);

    FREE (dep);
  }
  smartlist_free (vcpkg_packages);
  vcpkg_packages = NULL;
}

/**
 * Free the memory allocated for `vcpkg_nodes`.
 */
static void vcpkg_free_nodes (void)
{
  int i, max;

  if (!vcpkg_nodes)
     return;

  max = smartlist_len (vcpkg_nodes);
  for (i = 0; i < max; i++)
  {
    struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

    if (node->deps)
       smartlist_free (node->deps);
    FREE (node->description);
    FREE (node);
  }
  smartlist_free (vcpkg_nodes);
  vcpkg_nodes = NULL;
}

/**
 * Free the memory allocated for both smartlists.
 */
void vcpkg_free (void)
{
  vcpkg_free_packages();
  vcpkg_free_nodes();
  regex_free();
}

int vcpkg_get_control (const struct vcpkg_node **node_p, const char *packages)
{
  const struct vcpkg_node *node;
  static int i, max;

  if (*node_p == NULL)
  {
    max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;
    i = 0;
  }

  while (i < max)
  {
    node = smartlist_get (vcpkg_nodes, i++);

    if (node->have_CONTROL &&
        fnmatch(packages, node->package, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      *node_p = node;
      break;
    }
  }
  return (i < max ? 1 : 0);
}

int vcpkg_dump_control (const char *packages)
{
  const struct vcpkg_node *node;
  int   old, indent, padding, matches = 0;

  if (sub_level == 0)
     C_printf ("Dumping CONTROL for packages matching ~6%s~0.\n", packages);

  for (node = NULL; vcpkg_get_control(&node, packages); matches++)
  {
    const char *package = node->package;

    padding = VCPKG_MAX_NAME - strlen (package);
    padding = max (0, padding);

    if (sub_level + sub_indent > 0)
         indent  = C_printf ("%*s  ~6%s~0: ", 2*(sub_level-sub_indent), "", package);
    else indent  = C_printf ("  ~6%s~0: %*s", package, padding, "");

    indent -= 4;

    /* In case some other fields contains a `~'
     */
    old = C_setraw (1);

    if (sub_level == 0)
    {
      if (node->description)
           print_long_line (node->description, indent+2);
      else C_puts ("<none>\n");

      C_printf ("  %-*s%s\n", indent, "version:", node->version[0] ? node->version : "<none>");

      print_top_dependencies (node, indent);
    }

    if (opt.verbose >= 1 && sub_level <= 10)
    {
      if (sub_level == 0)
         C_printf ("  %-*s\n", indent+2, "sub-dependants:");
      print_sub_dependencies (node, indent);
    }
    C_setraw (old);
  }
  return (matches);
}

/*
 * The output of 'vcpkg search' is similar to this:
 *
 *  3fd                  2.6.2            C++ Framework For Fast Development
 *  abseil               2018-09-18       an open-source collection designed to augment the C++ standard library. Abseil...
 *  ace                  6.5.2            The ADAPTIVE Communication Environment
 *  alac                 2017-11-03-c3... The Apple Lossless Audio Codec (ALAC) is a lossless audio codec developed by A...
 *  alac-decoder         0.2              ALAC C implementation of a decoder, written from reverse engineering the file ...
 *  alembic              1.7.9            Alembic is an open framework for storing and sharing scene data that includes ...
 *  allegro5             5.2.4.0          Allegro is a cross-platform library mainly aimed at video game and multimedia ...
 *  anax                 2.1.0-3          An open source C++ entity system. <https://github.com/miguelmartin75/anax>
 *  angle                2017-06-14-8d... A conformant OpenGL ES implementation for Windows, Mac and Linux. The goal of ...
 *
 * \note The truncated descriptions and no dependency information.
 *       Try to do better in verbose mode as with 'envtool -v --vcpkg 3f*':
 *
 *       3fd:                  C++ Framework For Fast Development
 *       version:              2.6.2
 *       dependants:           boost-lockfree (windows), boost-regex (windows), poco (windows), sqlite3, rapidxml
 *         boost-lockfree: <reqursively show info for all sub-packages>
 *         boost-regex:
 *         poco:
 *         sqlite3:
 *         radidxml:
 */
int vcpkg_dump_control2 (const char *packages)
{
  return (0);
}

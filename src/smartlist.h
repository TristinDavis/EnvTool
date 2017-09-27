#ifndef _SMARTLIST_H
#define _SMARTLIST_H

#if defined(INSIDE_SMARTLIST_C)
  typedef struct smartlist_t smartlist_t;        /* Forward */
#else
  typedef struct smartlist_internal smartlist_t; /* Opaque struct */
#endif

typedef int  (*smartlist_compare_func) (const void *key, const void **member);
typedef void (*smartlist_parse_func) (smartlist_t *sl, const char *line);


int          smartlist_len (const smartlist_t *sl);
void        *smartlist_get (const smartlist_t *sl, int idx);
smartlist_t *smartlist_new (void);
smartlist_t *smartlist_init (smartlist_t *sl);

void  smartlist_free (smartlist_t *sl);
void  smartlist_free_all (smartlist_t *sl);
void  smartlist_ensure_capacity (smartlist_t *sl, size_t num);
void  smartlist_add (smartlist_t *sl, void *element);
void  smartlist_del (smartlist_t *sl, int idx);
void  smartlist_append (smartlist_t *sl1, const smartlist_t *sl2);

void  smartlist_sort (smartlist_t *sl, smartlist_compare_func compare);

int   smartlist_bsearch_idx (const smartlist_t *sl, const void *key,
                             smartlist_compare_func compare, int *found_out);

void *smartlist_bsearch (const smartlist_t *sl, const void *key,
                         smartlist_compare_func compare);

smartlist_t *smartlist_read_file (const char *file,
                                  smartlist_parse_func parse);

int smartlist_write_file (smartlist_t *sl, const char *file);

#endif

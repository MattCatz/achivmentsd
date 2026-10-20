#ifndef ACHD_PARAMS_H
#define ACHD_PARAMS_H

#include <stddef.h>

/* application/x-www-form-urlencoded helpers. All results are malloc'd. */
char *form_get(const char *form, const char *name);
char *form_set(const char *form, const char *name, const char *value);
char *url_encode(const char *s);

#endif

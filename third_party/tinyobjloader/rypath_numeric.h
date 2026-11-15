/* SPDX-License-Identifier: GPL-3.0-only
 * Local adaptation: use the project's strict, locale-independent decoder.
 * The upstream scanner override is used only after lexical validation.
 */
#ifndef RYPATH_OBJ_NUMERIC_H
#define RYPATH_OBJ_NUMERIC_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool rypath_obj_scan_double(const char **cursor, const char *end, double *value);
#ifdef __cplusplus
}
#endif
#define TOBJ_SCAN_DOUBLE_OVERRIDE rypath_obj_scan_double
#endif

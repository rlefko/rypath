/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright Contributors to the OpenEXR Project.
 * rypath: standalone static Core configuration; no Imath runtime. */
#pragma once
#define EXR_EXPORT
#define EXR_PRINTF_FUNC_ATTRIBUTE __attribute__((format(printf,3,4)))
#define ILMTHREAD_THREADING_ENABLED 1
#define IMATH_VERSION_MAJOR 0
#define IMATH_VERSION_MINOR 0

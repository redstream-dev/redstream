/* Redstream source-compatibility header for applications written against DeepStream.
 *
 *   #ifdef USE_REDSTREAM
 *   #include <rds/rds_compat.h>
 *   #else
 *   #include "gstnvdsmeta.h"
 *   #endif
 *
 * Define RDS_COMPAT_NO_MACROS before including to keep the GStreamer functions unredirected
 * and call the rds_compat_* functions explicitly instead.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_COMPAT_H
#define RDS_COMPAT_H

#include <gst/gst.h>
#include <rds/rds-compat-remap.h>

#ifndef RDS_COMPAT_NO_MACROS
/* Object-like macros with identical signatures, so taking the function's address keeps working. */
#define gst_element_factory_make rds_compat_element_factory_make
#define gst_parse_launch rds_compat_parse_launch
#endif

#endif /* RDS_COMPAT_H */

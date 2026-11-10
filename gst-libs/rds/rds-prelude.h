/* Redstream: symbol export macro for libgstrds and libgstrds-analytics.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_PRELUDE_H
#define RDS_PRELUDE_H

/* The libraries are built with hidden visibility; RDS_API marks the public symbols.
 * BUILDING_RDS is defined only while compiling libgstrds or libgstrds-analytics themselves. */
#if defined(BUILDING_RDS)
#define RDS_API __attribute__ ((visibility ("default")))
#else
#define RDS_API
#endif

#endif /* RDS_PRELUDE_H */

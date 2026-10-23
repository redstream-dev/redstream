/* Redstream: symbol export macro for libgstrds.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_PRELUDE_H
#define RDS_PRELUDE_H

/* The library is built with hidden visibility; RDS_API marks the public symbols.
 * BUILDING_RDS is defined only while compiling libgstrds itself. */
#if defined(BUILDING_RDS)
#define RDS_API __attribute__ ((visibility ("default")))
#else
#define RDS_API
#endif

#endif /* RDS_PRELUDE_H */

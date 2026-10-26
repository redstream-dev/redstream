/* Redstream: rdslogger, a passthrough element that prints each stream's frame rate to stdout (the counterpart of
 * DeepStream's nvdslogger, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_LOGGER_H
#define GST_RDS_LOGGER_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_LOGGER (gst_rds_logger_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsLogger, gst_rds_logger, GST, RDS_LOGGER, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdslogger);

G_END_DECLS

#endif /* GST_RDS_LOGGER_H */

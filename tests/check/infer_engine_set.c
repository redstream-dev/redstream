/* rdsinfer's engine set: the batch sizes of its programs (dynamic-batch).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-engine-set.h"

/* @batch_size's sizes must be the @n_want in @want, in order. */
static void
assert_sizes (guint batch_size, gboolean dynamic, const guint * want, guint n_want)
{
  guint sizes[RDS_INFER_ENGINE_SET_MAX_SIZES], n;

  g_test_message ("batch-size %u, dynamic-batch %d", batch_size, dynamic);
  n = rds_infer_engine_set_sizes (batch_size, dynamic, sizes, G_N_ELEMENTS (sizes));
  g_assert_cmpmem (sizes, n * sizeof (guint), want, n_want * sizeof (guint));
}

G_STATIC_ASSERT (RDS_INFER_MAX_BATCH_SIZE == 1024);    /* the table below */

#define ASSERT_SIZES(batch_size, dynamic, ...) G_STMT_START { \
    static const guint want_[] = { __VA_ARGS__ }; \
    assert_sizes (batch_size, dynamic, want_, G_N_ELEMENTS (want_)); } G_STMT_END

/* The powers of two below batch-size, then batch-size; only batch-size without dynamic-batch. */
static void
test_sizes (void)
{
  ASSERT_SIZES (1, TRUE, 1);
  ASSERT_SIZES (2, TRUE, 1, 2);
  ASSERT_SIZES (3, TRUE, 1, 2, 3);
  ASSERT_SIZES (4, TRUE, 1, 2, 4);
  ASSERT_SIZES (6, TRUE, 1, 2, 4, 6);
  ASSERT_SIZES (16, TRUE, 1, 2, 4, 8, 16);
  ASSERT_SIZES (32, TRUE, 1, 2, 4, 8, 16, 32);
  ASSERT_SIZES (1023, TRUE, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1023);
  /* the largest batch-size fills the array */
  ASSERT_SIZES (RDS_INFER_MAX_BATCH_SIZE, TRUE, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024);

  ASSERT_SIZES (1, FALSE, 1);
  ASSERT_SIZES (16, FALSE, 16);
  ASSERT_SIZES (RDS_INFER_MAX_BATCH_SIZE, FALSE, RDS_INFER_MAX_BATCH_SIZE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer-engine-set/sizes", test_sizes);
  return g_test_run ();
}

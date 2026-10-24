/* Memory probes for soak tests: VmRSS and this process's GPU memory, plus a small ASan quarantine for the soak
 * builds (RDS_TEST_SMALL_QUARANTINE). Header-only; include it from one source file per executable.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_SOAK_H
#define RDS_TEST_SOAK_H

/* GLib/GStreamer first: HIP's headers define __noinline__ */
#include <gst/gst.h>
#include <hip/hip_runtime_api.h>
#include <string.h>

/* Under ASan, freed memory sits in a quarantine (256 MiB by default) that fills up over the first loops of the soak
 * and reads as VmRSS growth (+170 MiB). A soak test's own build (RDS_TEST_SMALL_QUARANTINE, see tests/meson.build)
 * uses a small quarantine, which keeps use-after-free detection and makes VmRSS meaningful
 * again; LSan still reports real leaks at exit. The other tests keep ASan's defaults. ASAN_OPTIONS from the
 * environment override this. */
#if defined(__SANITIZE_ADDRESS__)
#define RDS_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RDS_TEST_ASAN 1
#endif
#endif
#if defined (RDS_TEST_ASAN) && defined (RDS_TEST_SMALL_QUARANTINE)
const char *__asan_default_options (void);

__attribute__((used, visibility ("default"))) const char *
__asan_default_options (void)
{
  return "quarantine_size_mb=4:thread_local_quarantine_size_kb=64";
}
#endif

static gsize
vm_rss_bytes (void)
{
  gchar *status = NULL, *line;
  gsize kib = 0;

  g_assert_true (g_file_get_contents ("/proc/self/status", &status, NULL, NULL));
  line = strstr (status, "\nVmRSS:");
  g_assert_nonnull (line);
  kib = g_ascii_strtoull (line + strlen ("\nVmRSS:"), NULL, 10);
  g_free (status);
  return kib * 1024;
}

/* Sums the drm-total-vram and drm-total-gtt keys of one DRM fdinfo file into @sum (bytes). FALSE if it has neither. */
static gboolean
add_fdinfo_gpu_bytes (const gchar * path, guint64 * sum)
{
  static const gchar *keys[] = { "drm-total-vram:", "drm-total-gtt:" };
  gchar *text = NULL, **lines;
  gboolean found = FALSE;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return FALSE;
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
      if (g_str_has_prefix (lines[i], keys[k])) {
        gchar *end = NULL;
        guint64 v = g_ascii_strtoull (lines[i] + strlen (keys[k]), &end, 10);

        while (end != NULL && g_ascii_isspace (*end))
          end++;
        if (end != NULL && g_str_has_prefix (end, "KiB"))
          v *= 1024;
        else if (end != NULL && g_str_has_prefix (end, "MiB"))
          v *= 1024 * 1024;
        *sum += v;
        found = TRUE;
      }
    }
  }
  g_strfreev (lines);
  g_free (text);
  return found;
}

/* GPU memory held by this process: drm-total-vram + drm-total-gtt from the DRM fdinfo of every render node it has
 * open. HIP (KFD) allocations are accounted on the render node's DRM client (a 64 MiB hipMalloc moves it by 64 MiB;
 * on APUs device memory shows up as GTT), and so are VA surfaces. Unlike hipMemGetInfo, which is device-wide, other
 * processes on the GPU don't move it. Kernels without these fdinfo keys fall back to hipMemGetInfo, where any other
 * process allocating on the device during the soak skews the delta. */
static gsize
gpu_used_bytes (void)
{
  GDir *dir = g_dir_open ("/proc/self/fd", 0, NULL);
  const gchar *name;
  guint64 sum = 0;
  gboolean found = FALSE;
  size_t free_b = 0, total = 0;

  g_assert_nonnull (dir);
  while ((name = g_dir_read_name (dir)) != NULL) {
    gchar *fd_path = g_build_filename ("/proc/self/fd", name, NULL);
    gchar *target = g_file_read_link (fd_path, NULL);

    if (target != NULL && g_str_has_prefix (target, "/dev/dri/renderD")) {
      gchar *info_path = g_build_filename ("/proc/self/fdinfo", name, NULL);

      found = add_fdinfo_gpu_bytes (info_path, &sum) || found;
      g_free (info_path);
    }
    g_free (target);
    g_free (fd_path);
  }
  g_dir_close (dir);
  if (found)
    return sum;

  g_assert_cmpint (hipMemGetInfo (&free_b, &total), ==, hipSuccess);
  return total - free_b;
}

#define MIB(b) ((gdouble) (b) / (1024.0 * 1024.0))

#endif /* RDS_TEST_SOAK_H */

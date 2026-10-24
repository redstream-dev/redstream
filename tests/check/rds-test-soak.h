/* Memory probes for soak tests: host memory (VmRSS, or ASan's live heap under ASan) and this process's GPU memory.
 * Header-only; include it from one source file per executable.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_SOAK_H
#define RDS_TEST_SOAK_H

/* GLib/GStreamer first: HIP's headers define __noinline__ */
#include <gst/gst.h>
#include <hip/hip_runtime_api.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define RDS_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RDS_TEST_ASAN 1
#endif
#endif

G_GNUC_UNUSED static gsize
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

/* Host memory the soak checks for growth: VmRSS in normal builds. Under ASan, VmRSS also moves with the allocator's
 * own caches and its quarantine of freed chunks (256 MiB by default: +170 MiB over the first loops, and noisy under
 * parallel load), so the soak reads ASan's count of live heap bytes instead. A free counts as freed when the chunk
 * enters the quarantine, so quarantined memory is not in that count and the soaks run with ASan's default options.
 * The count leaves out memory not allocated through malloc (HIP pinned host memory, driver mappings): the normal
 * build's VmRSS check covers those. */
#ifdef RDS_TEST_ASAN
/* from <sanitizer/allocator_interface.h>, which the dev image's GCC does not install */
size_t __sanitizer_get_current_allocated_bytes (void);
#define RDS_TEST_HOST_MEMORY_NAME "ASan live heap"
static gsize
host_memory_bytes (void)
{
  return __sanitizer_get_current_allocated_bytes ();
}
#else
#define RDS_TEST_HOST_MEMORY_NAME "VmRSS"
static gsize
host_memory_bytes (void)
{
  return vm_rss_bytes ();
}
#endif

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

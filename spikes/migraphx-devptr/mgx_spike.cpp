// SPDX-License-Identifier: LGPL-2.1-or-later
// Spike: MIGraphX with offload_copy=false on caller-owned HIP buffers, run_async on our stream,
// partial-batch padding, .mxr save/load, FP16. Usage: mgx_spike model.onnx
#include <migraphx/migraphx.hpp>
#include <hip/hip_fp16.h>
#include <hip/hip_runtime.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define HIP_CHECK(x)                                                                      \
  do {                                                                                    \
    hipError_t e_ = (x);                                                                  \
    if (e_ != hipSuccess) {                                                               \
      fprintf(stderr, "%s:%d %s -> %s\n", __FILE__, __LINE__, #x, hipGetErrorString(e_)); \
      exit(2);                                                                            \
    }                                                                                     \
  } while (0)

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

static bool is_output(const std::string &name) { return name.find("#output") != std::string::npos; }

static migraphx::program build(const char *onnx, unsigned batch, bool offload, bool fp16) {
  migraphx::onnx_options o;
  o.set_default_dim_value(batch);
  auto p = migraphx::parse_onnx(onnx, o);
  if (fp16)
    migraphx::quantize_fp16(p);
  migraphx::compile_options co;
  co.set_offload_copy(offload);
  auto t = clk::now();
  p.compile(migraphx::target("gpu"), co);
  fprintf(stderr, "compile batch=%u offload=%d fp16=%d: %.0f ms\n", batch, offload, fp16, ms_since(t));
  return p;
}

// Deterministic per-image input; slot >= valid gets zeros (the padding policy under test).
static std::vector<float> make_input(size_t per_image, unsigned batch, unsigned valid) {
  std::vector<float> v(per_image * batch, 0.0f);
  for (unsigned b = 0; b < valid; b++)
    for (size_t i = 0; i < per_image; i++)
      v[b * per_image + i] = std::sin(0.001f * (float)i);
  return v;
}

// Reference: offload_copy=true, host buffers, eval().
static std::vector<float> run_host(migraphx::program &p, const std::vector<float> &in) {
  migraphx::program_parameters params;
  auto shapes = p.get_parameter_shapes();
  for (auto name : shapes.names())
    params.add(name, migraphx::argument(shapes[name], (void *)in.data()));
  auto outs = p.eval(params);
  auto o = outs[0];
  const float *d = reinterpret_cast<const float *>(o.data());
  return std::vector<float>(d, d + o.get_shape().bytes() / sizeof(float));
}

// Under test: offload_copy=false, caller-owned device buffers, run_async on our stream.
// The input is assumed float; the output may be float or half (fp16 program) and is returned as float.
// The output buffer is pre-filled with 0xFF bytes (NaN) so a stale or unwritten output can never pass.
// *ptr_ok reports whether the arguments returned by run_async alias our output buffer.
static std::vector<float> run_device(migraphx::program &p, const std::vector<float> &in, hipStream_t s,
                                     bool *ptr_ok) {
  migraphx::program_parameters params;
  auto shapes = p.get_parameter_shapes();
  std::vector<void *> bufs;
  void *out_dev = nullptr;
  size_t out_bytes = 0;
  bool out_half = false;
  for (auto name : shapes.names()) {
    auto sh = shapes[name];
    void *d = nullptr;
    HIP_CHECK(hipMalloc(&d, sh.bytes()));
    bufs.push_back(d);
    if (is_output(name)) {
      out_dev = d;
      out_bytes = sh.bytes();
      out_half = sh.type() == migraphx_shape_half_type;
      HIP_CHECK(hipMemsetAsync(d, 0xFF, sh.bytes(), s));
      if (!out_half && sh.type() != migraphx_shape_float_type) {
        fprintf(stderr, "unexpected output dtype %d\n", (int)sh.type());
        exit(5);
      }
    } else {
      if (sh.type() != migraphx_shape_float_type) {
        fprintf(stderr, "unexpected input dtype %d for '%s'\n", (int)sh.type(), name);
        exit(5);
      }
      HIP_CHECK(hipMemcpyAsync(d, in.data(), sh.bytes(), hipMemcpyHostToDevice, s));
    }
    params.add(name, migraphx::argument(sh, d));
  }
  if (!out_dev) {
    fprintf(stderr, "no '#output' parameter: offload_copy=false contract differs, inspect names\n");
    exit(4);
  }
  // Drain the input upload first so the stream-query probe below only sees the inference work.
  HIP_CHECK(hipStreamSynchronize(s));
  auto t = clk::now();
  auto outs = p.run_async(params, s);
  double enqueue_ms = ms_since(t);
  *ptr_ok = outs.size() == 1 && (void *)outs[0].data() == out_dev;
  fprintf(stderr, "  run_async returned %zu argument(s), outs[0].data() %s our output buffer\n", outs.size(),
          *ptr_ok ? "==" : "!=");
  // Diagnostic only: if run_async enqueued on OUR stream, the stream is usually still busy here.
  hipError_t q = hipStreamQuery(s);
  t = clk::now();
  HIP_CHECK(hipStreamSynchronize(s));
  fprintf(stderr, "  run_async returned in %.2f ms; our stream right after: %s; our stream sync waited %.2f ms\n",
          enqueue_ms, q == hipErrorNotReady ? "busy" : (q == hipSuccess ? "idle" : hipGetErrorString(q)),
          ms_since(t));
  std::vector<float> out;
  if (out_half) {
    std::vector<__half> h(out_bytes / sizeof(__half));
    HIP_CHECK(hipMemcpyAsync(h.data(), out_dev, out_bytes, hipMemcpyDeviceToHost, s));
    HIP_CHECK(hipStreamSynchronize(s));
    out.resize(h.size());
    for (size_t i = 0; i < h.size(); i++)
      out[i] = __half2float(h[i]);
    fprintf(stderr, "  output dtype half, converted to float on host\n");
  } else {
    out.resize(out_bytes / sizeof(float));
    HIP_CHECK(hipMemcpyAsync(out.data(), out_dev, out_bytes, hipMemcpyDeviceToHost, s));
    HIP_CHECK(hipStreamSynchronize(s));
  }
  for (void *d : bufs)
    HIP_CHECK(hipFree(d));
  return out;
}

// Device-side busy wait used to make the stream's prior work slow (see run_ordered).
__global__ void spin_kernel(unsigned long long ticks) {
  unsigned long long start = wall_clock64();
  while (wall_clock64() - start < ticks) {
  }
}

// [5]: the whole sequence is enqueued on `s` with no host syncs in between; one sync at the end.
// A slow spin kernel sits between zeroing the input and uploading the real input, so if run_async did
// not wait for prior work on `s` it would read zeros, and if the D2H copy did not wait for run_async it
// would read the NaN sentinel. `exec` is the stream given to run_async (== s for the real test).
static std::vector<float> run_ordered(migraphx::program &p, const std::vector<float> &in, hipStream_t s,
                                      hipStream_t exec, unsigned spin_ms, bool *ptr_ok) {
  auto shapes = p.get_parameter_shapes();
  migraphx::program_parameters params;
  void *in_dev = nullptr, *out_dev = nullptr;
  size_t in_bytes = 0, out_bytes = 0;
  for (auto name : shapes.names()) {
    auto sh = shapes[name];
    void *d = nullptr;
    HIP_CHECK(hipMalloc(&d, sh.bytes()));
    if (is_output(name)) {
      out_dev = d;
      out_bytes = sh.bytes();
    } else {
      in_dev = d;
      in_bytes = sh.bytes();
    }
    params.add(name, migraphx::argument(sh, d));
  }
  if (in_bytes != in.size() * sizeof(float) || shapes[shapes.names()[0]].type() == migraphx_shape_half_type) {
    fprintf(stderr, "run_ordered: unexpected input size\n");
    exit(5);
  }
  float *in_host = nullptr, *out_host = nullptr;
  HIP_CHECK(hipHostMalloc((void **)&in_host, in_bytes, hipHostMallocDefault));
  HIP_CHECK(hipHostMalloc((void **)&out_host, out_bytes, hipHostMallocDefault));
  std::memcpy(in_host, in.data(), in_bytes);
  int rate_khz = 0;
  HIP_CHECK(hipDeviceGetAttribute(&rate_khz, hipDeviceAttributeWallClockRate, 0));
  HIP_CHECK(hipDeviceSynchronize()); // start from an idle device; nothing below syncs until the end

  auto t = clk::now();
  HIP_CHECK(hipMemsetAsync(out_dev, 0xFF, out_bytes, s));
  HIP_CHECK(hipMemsetAsync(in_dev, 0, in_bytes, s));
  hipLaunchKernelGGL(spin_kernel, dim3(1), dim3(1), 0, s, (unsigned long long)rate_khz * spin_ms);
  HIP_CHECK(hipGetLastError());
  HIP_CHECK(hipMemcpyAsync(in_dev, in_host, in_bytes, hipMemcpyHostToDevice, s));
  auto outs = p.run_async(params, exec);
  HIP_CHECK(hipMemcpyAsync(out_host, out_dev, out_bytes, hipMemcpyDeviceToHost, s));
  double enqueue_ms = ms_since(t);
  hipError_t q = hipStreamQuery(s);
  HIP_CHECK(hipStreamSynchronize(s));
  double total_ms = ms_since(t);
  *ptr_ok = outs.size() == 1 && (void *)outs[0].data() == out_dev;
  fprintf(stderr, "  ordered: spin %u ms (wall clock %d kHz), enqueue %.2f ms, stream after enqueue: %s, total %.1f ms, "
          "outs[0].data() %s our output buffer\n", spin_ms, rate_khz, enqueue_ms,
          q == hipErrorNotReady ? "busy" : (q == hipSuccess ? "idle" : hipGetErrorString(q)), total_ms,
          *ptr_ok ? "==" : "!=");
  std::vector<float> out(out_host, out_host + out_bytes / sizeof(float));
  HIP_CHECK(hipDeviceSynchronize()); // the negative control may still be running on `exec`
  HIP_CHECK(hipHostFree(in_host));
  HIP_CHECK(hipHostFree(out_host));
  HIP_CHECK(hipFree(in_dev));
  HIP_CHECK(hipFree(out_dev));
  return out;
}

static float max_abs_diff(const float *a, const float *b, size_t n) {
  float m = 0;
  for (size_t i = 0; i < n; i++) {
    float d = std::fabs(a[i] - b[i]);
    if (std::isnan(d))
      return INFINITY; // std::fmax would silently drop NaN (e.g. an unwritten sentinel output)
    m = std::fmax(m, d);
  }
  return m;
}

static size_t argmax(const float *a, size_t n) {
  size_t k = 0;
  for (size_t i = 1; i < n; i++)
    if (a[i] > a[k])
      k = i;
  return k;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s model.onnx\n", argv[0]);
    return 1;
  }
  const char *onnx = argv[1];
  hipStream_t s;
  HIP_CHECK(hipStreamCreate(&s));
  int failures = 0;

  auto ref_p = build(onnx, 1, true, false);
  size_t per_image = 0;
  {
    auto shapes = ref_p.get_parameter_shapes();
    for (auto name : shapes.names())
      fprintf(stderr, "  ref param '%s' %zu bytes\n", name, shapes[name].bytes());
    per_image = shapes[shapes.names()[0]].bytes() / sizeof(float);
  }
  auto in1 = make_input(per_image, 1, 1);
  auto ref = run_host(ref_p, in1);
  size_t classes = ref.size();

  auto dev_p = build(onnx, 1, false, false);
  {
    auto shapes = dev_p.get_parameter_shapes();
    for (auto name : shapes.names())
      fprintf(stderr, "  param '%s' %zu bytes\n", name, shapes[name].bytes());
  }
  bool p1 = false;
  auto dev = run_device(dev_p, in1, s, &p1);
  float d1 = max_abs_diff(ref.data(), dev.data(), classes);
  bool ok1 = d1 < 1e-3f && p1;
  printf("[1] device ptrs + run_async vs reference: max|diff| = %g, output ptr %s, %s\n", d1,
         p1 ? "ours" : "NOT ours", ok1 ? "PASS" : "FAIL");
  failures += !ok1;

  auto b4_p = build(onnx, 4, false, false);
  bool shapes_ok = true;
  {
    auto shapes = b4_p.get_parameter_shapes();
    for (auto name : shapes.names()) {
      auto sh = shapes[name];
      std::string lens, strides;
      for (auto l : sh.lengths())
        lens += std::to_string(l) + " ";
      for (auto l : sh.strides())
        strides += std::to_string(l) + " ";
      fprintf(stderr, "  b4 param '%s' type %d lengths [ %s] strides [ %s] elements %zu bytes %zu standard %d\n", name,
              (int)sh.type(), lens.c_str(), strides.c_str(), sh.elements(), sh.bytes(), (int)sh.standard());
      if (!sh.standard())
        shapes_ok = false;
      if (is_output(name)) {
        if (sh.elements() != 4 * classes)
          shapes_ok = false;
      } else if (sh.bytes() != 4 * per_image * sizeof(float)) {
        shapes_ok = false;
      }
    }
  }
  bool p2 = false;
  auto b4 = run_device(b4_p, make_input(per_image, 4, 1), s, &p2);
  float d2 = max_abs_diff(ref.data(), b4.data(), classes);
  bool ok2 = d2 < 1e-3f && p2 && shapes_ok && b4.size() == 4 * classes;
  printf("[2] batch-4 slot 0 (slots 1-3 zero) vs batch-1: max|diff| = %g, shapes %s, output ptr %s, %s\n", d2,
         shapes_ok ? "ok" : "UNEXPECTED", p2 ? "ours" : "NOT ours", ok2 ? "PASS" : "FAIL");
  failures += !ok2;

  const char *mxr = ".cache/models/mobilenetv2-12_b1_spike.mxr";
  migraphx::save(dev_p, mxr);
  auto t = clk::now();
  auto loaded = migraphx::load(mxr);
  double load_ms = ms_since(t);
  bool p3 = false;
  auto again = run_device(loaded, in1, s, &p3);
  bool same = again.size() == dev.size() && std::memcmp(again.data(), dev.data(), classes * sizeof(float)) == 0;
  bool ok3 = same && p3;
  printf("[3] .mxr save/load: load %.0f ms, output %s, output ptr %s, %s\n", load_ms, same ? "identical" : "differs",
         p3 ? "ours" : "NOT ours", ok3 ? "PASS" : "FAIL");
  failures += !ok3;

  auto h_p = build(onnx, 1, false, true);
  {
    auto shapes = h_p.get_parameter_shapes();
    for (auto name : shapes.names())
      fprintf(stderr, "  fp16 param '%s' %zu bytes type %d\n", name, shapes[name].bytes(), (int)shapes[name].type());
  }
  bool p4 = false;
  auto half = run_device(h_p, in1, s, &p4);
  bool same_class = argmax(half.data(), classes) == argmax(ref.data(), classes);
  bool ok4 = same_class && p4;
  printf("[4] fp16: max|diff| = %g, argmax %s (ref class %zu), output ptr %s, %s\n",
         max_abs_diff(ref.data(), half.data(), classes), same_class ? "same" : "differs", argmax(ref.data(), classes),
         p4 ? "ours" : "NOT ours", ok4 ? "PASS" : "FAIL");
  failures += !ok4;

  // [5] sync-free ordering, with a 300 ms spin before the input upload. Both streams are non-blocking so
  // that no implicit null-stream synchronization can supply the ordering: the only edge between the
  // upload and the inference is the stream handed to run_async.
  hipStream_t ns, other;
  HIP_CHECK(hipStreamCreateWithFlags(&ns, hipStreamNonBlocking));
  HIP_CHECK(hipStreamCreateWithFlags(&other, hipStreamNonBlocking));
  bool p5 = false;
  auto ord = run_ordered(dev_p, in1, ns, ns, 300, &p5);
  float d5 = max_abs_diff(ref.data(), ord.data(), classes);
  bool ok5 = d5 < 1e-3f && p5;
  printf("[5] stream ordering (no host syncs): max|diff| = %g, output ptr %s, %s\n", d5, p5 ? "ours" : "NOT ours",
         ok5 ? "PASS" : "FAIL");
  failures += !ok5;
  // Negative controls (informational, not counted): run_async on a different stream has no dependency on
  // the upload, so [5] is only meaningful if this shows a large diff or NaN.
  bool pc = false;
  auto ctl = run_ordered(dev_p, in1, ns, other, 300, &pc);
  float dc = max_abs_diff(ref.data(), ctl.data(), classes);
  printf("[5c] negative control (upload on non-blocking stream, run_async on another): max|diff| = %g (%s)\n", dc,
         dc < 1e-3f ? "NOT detected" : "ordering violation detected");
  // Same control with the blocking stream `s` as the upload stream. A control that is NOT detected was seen
  // once (fresh stream, cause unconfirmed; possibly HIP mapping both streams to one hardware queue), so
  // [5] is only conclusive in runs where [5c] detects the violation.
  auto ctl2 = run_ordered(dev_p, in1, s, other, 300, &pc);
  float dc2 = max_abs_diff(ref.data(), ctl2.data(), classes);
  printf("[5d] control with blocking upload stream: max|diff| = %g (%s)\n", dc2,
         dc2 < 1e-3f ? "NOT detected" : "ordering violation detected");
  if (dc < 1e-3f)
    printf("    warning: [5c] did not detect the violation, so [5] is inconclusive in this run\n");
  HIP_CHECK(hipStreamDestroy(other));
  HIP_CHECK(hipStreamDestroy(ns));

  HIP_CHECK(hipStreamDestroy(s));
  printf("%s (%d failures)\n", failures ? "SPIKE FAIL" : (dc < 1e-3f ? "SPIKE PASS ([5] inconclusive)" : "SPIKE PASS"),
         failures);
  return failures ? 3 : 0;
}

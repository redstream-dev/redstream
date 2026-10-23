// Spike 0b: MIGraphX with offload_copy=false on caller-owned HIP buffers, run_async on our stream,
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
static std::vector<float> run_device(migraphx::program &p, const std::vector<float> &in, hipStream_t s) {
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
  p.run_async(params, s);
  double enqueue_ms = ms_since(t);
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

static float max_abs_diff(const float *a, const float *b, size_t n) {
  float m = 0;
  for (size_t i = 0; i < n; i++)
    m = std::fmax(m, std::fabs(a[i] - b[i]));
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
  auto dev = run_device(dev_p, in1, s);
  float d1 = max_abs_diff(ref.data(), dev.data(), classes);
  printf("[1] device ptrs + run_async vs reference: max|diff| = %g %s\n", d1, d1 < 1e-3f ? "PASS" : "FAIL");
  failures += d1 >= 1e-3f;

  auto b4_p = build(onnx, 4, false, false);
  auto b4 = run_device(b4_p, make_input(per_image, 4, 1), s);
  float d2 = max_abs_diff(ref.data(), b4.data(), classes);
  printf("[2] batch-4 slot 0 (slots 1-3 zero) vs batch-1: max|diff| = %g %s\n", d2, d2 < 1e-3f ? "PASS" : "FAIL");
  failures += d2 >= 1e-3f;

  const char *mxr = ".cache/models/mobilenetv2-12_b1_spike.mxr";
  migraphx::save(dev_p, mxr);
  auto t = clk::now();
  auto loaded = migraphx::load(mxr);
  double load_ms = ms_since(t);
  auto again = run_device(loaded, in1, s);
  bool same = std::memcmp(again.data(), dev.data(), classes * sizeof(float)) == 0;
  printf("[3] .mxr save/load: load %.0f ms, output %s\n", load_ms, same ? "identical PASS" : "differs FAIL");
  failures += !same;

  auto h_p = build(onnx, 1, false, true);
  {
    auto shapes = h_p.get_parameter_shapes();
    for (auto name : shapes.names())
      fprintf(stderr, "  fp16 param '%s' %zu bytes type %d\n", name, shapes[name].bytes(), (int)shapes[name].type());
  }
  auto half = run_device(h_p, in1, s);
  bool same_class = argmax(half.data(), classes) == argmax(ref.data(), classes);
  printf("[4] fp16: max|diff| = %g, argmax %s (ref class %zu)\n", max_abs_diff(ref.data(), half.data(), classes),
         same_class ? "same PASS" : "differs FAIL", argmax(ref.data(), classes));
  failures += !same_class;

  HIP_CHECK(hipStreamDestroy(s));
  printf("%s (%d failures)\n", failures ? "SPIKE 0b FAIL" : "SPIKE 0b PASS", failures);
  return failures ? 3 : 0;
}

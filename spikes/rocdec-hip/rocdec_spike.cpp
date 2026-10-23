// Spike 0a: rocDecode H.264 decode -> HIP device frame -> copy into an owned pitch-linear NV12 surface.
// Prints "frame <n> y=<crc> uv=<crc>" (same format as ref_crc) unless --no-crc.
// Usage: rocdec_spike file.h264 [--loops N] [--no-crc]
#include <rocdecode/rocdecode.h>
#include <rocdecode/rocparser.h>
#include <hip/hip_runtime.h>
#include <zlib.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#define HIP_CHECK(x)                                                                          \
  do {                                                                                        \
    hipError_t e_ = (x);                                                                      \
    if (e_ != hipSuccess) {                                                                   \
      fprintf(stderr, "%s:%d %s -> %s\n", __FILE__, __LINE__, #x, hipGetErrorString(e_));     \
      exit(2);                                                                                \
    }                                                                                         \
  } while (0)
#define RD_CHECK(x)                                                                           \
  do {                                                                                        \
    rocDecStatus s_ = (x);                                                                    \
    if (s_ != ROCDEC_SUCCESS) {                                                               \
      fprintf(stderr, "%s:%d %s -> %s\n", __FILE__, __LINE__, #x, rocDecGetErrorName(s_));    \
      exit(2);                                                                                \
    }                                                                                         \
  } while (0)

using clk = std::chrono::steady_clock;

struct Ctx {
  rocDecDecoderHandle dec = nullptr;
  uint32_t width = 0, height = 0;  // display (cropped) size
  uint8_t *owned = nullptr;        // owned NV12 surface: Y rows then UV rows, same pitch
  size_t owned_pitch = 0;
  std::vector<uint8_t> host;
  hipStream_t stream = nullptr;
  long frames = 0;
  bool print_crc = true;
  double copy_ms = 0;
};

static int ROCDECAPI on_sequence(void *ud, RocdecVideoFormat *fmt) {
  Ctx *c = static_cast<Ctx *>(ud);
  uint32_t surfaces = fmt->min_num_decode_surfaces + 4;  // headroom for display delay
  if (c->dec)
    return surfaces;  // spike: same stream repeated, no reconfigure needed

  RocDecoderCreateInfo ci = {};
  ci.device_id = 0;
  ci.width = ci.max_width = fmt->coded_width;
  ci.height = ci.max_height = fmt->coded_height;
  ci.num_decode_surfaces = surfaces;
  ci.codec_type = fmt->codec;
  ci.chroma_format = fmt->chroma_format;
  ci.bit_depth_minus_8 = fmt->bit_depth_luma_minus8;
  ci.display_rect.left = (int16_t)fmt->display_area.left;
  ci.display_rect.top = (int16_t)fmt->display_area.top;
  ci.display_rect.right = (int16_t)fmt->display_area.right;
  ci.display_rect.bottom = (int16_t)fmt->display_area.bottom;
  ci.output_format = rocDecVideoSurfaceFormat_NV12;
  c->width = fmt->display_area.right - fmt->display_area.left;
  c->height = fmt->display_area.bottom - fmt->display_area.top;
  ci.target_width = c->width;
  ci.target_height = c->height;
  ci.num_output_surfaces = 1;
  RD_CHECK(rocDecCreateDecoder(&c->dec, &ci));

  HIP_CHECK(hipMallocPitch((void **)&c->owned, &c->owned_pitch, c->width, c->height * 3 / 2));
  c->host.resize((size_t)c->width * c->height * 3 / 2);
  fprintf(stderr, "sequence: coded %ux%u display %ux%u surfaces %u owned pitch %zu\n", fmt->coded_width,
          fmt->coded_height, c->width, c->height, surfaces, c->owned_pitch);
  return surfaces;
}

static int ROCDECAPI on_decode(void *ud, RocdecPicParams *pp) {
  RD_CHECK(rocDecDecodeFrame(static_cast<Ctx *>(ud)->dec, pp));
  return 1;
}

static int ROCDECAPI on_display(void *ud, RocdecParserDispInfo *di) {
  Ctx *c = static_cast<Ctx *>(ud);
  void *planes[3] = {};
  uint32_t pitch[3] = {};
  RocdecProcParams pp = {};
  pp.progressive_frame = di->progressive_frame;
  pp.top_field_first = di->top_field_first;
  RD_CHECK(rocDecGetVideoFrame(c->dec, di->picture_index, planes, pitch, &pp));
  uint32_t uv_pitch = pitch[1] ? pitch[1] : pitch[0];

  auto t0 = clk::now();
  HIP_CHECK(hipMemcpy2DAsync(c->owned, c->owned_pitch, planes[0], pitch[0], c->width, c->height,
                             hipMemcpyDeviceToDevice, c->stream));
  HIP_CHECK(hipMemcpy2DAsync(c->owned + c->owned_pitch * c->height, c->owned_pitch, planes[1], uv_pitch,
                             c->width, c->height / 2, hipMemcpyDeviceToDevice, c->stream));
  HIP_CHECK(hipStreamSynchronize(c->stream));
  c->copy_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();

  if (c->print_crc) {
    HIP_CHECK(hipMemcpy2D(c->host.data(), c->width, c->owned, c->owned_pitch, c->width, c->height * 3 / 2,
                          hipMemcpyDeviceToHost));
    size_t ysz = (size_t)c->width * c->height;
    uLong y = crc32(crc32(0, Z_NULL, 0), c->host.data(), ysz);
    uLong uv = crc32(crc32(0, Z_NULL, 0), c->host.data() + ysz, ysz / 2);
    printf("frame %ld y=%08lx uv=%08lx\n", c->frames, y, uv);
  }
  c->frames++;
  return 1;
}

static long rss_kb() {
  long pages = 0, resident = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f && fscanf(f, "%ld %ld", &pages, &resident) != 2)
    resident = 0;
  if (f)
    fclose(f);
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

static size_t gpu_free() {
  size_t free_b = 0, total_b = 0;
  HIP_CHECK(hipMemGetInfo(&free_b, &total_b));
  return free_b;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.h264 [--loops N] [--no-crc]\n", argv[0]);
    return 1;
  }
  int loops = 1;
  Ctx c;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--loops") && i + 1 < argc)
      loops = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-crc"))
      c.print_crc = false;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.empty()) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  HIP_CHECK(hipStreamCreate(&c.stream));

  RocdecParserParams pp = {};
  pp.codec_type = rocDecVideoCodec_AVC;
  pp.max_num_decode_surfaces = 1;  // updated by the sequence callback's return value
  pp.max_display_delay = 0;
  pp.user_data = &c;
  pp.pfn_sequence_callback = on_sequence;
  pp.pfn_decode_picture = on_decode;
  pp.pfn_display_picture = on_display;
  RocdecVideoParser parser = nullptr;
  RD_CHECK(rocDecCreateVideoParser(&parser, &pp));

  long rss_first = 0;
  size_t gpu_first = 0;
  auto t0 = clk::now();
  // rocDecode's parser mis-decodes NAL units split across packets (fixed-size chunks give wrong frames and
  // ROCDEC_RUNTIME_ERROR), so feed whole NAL units: cut at every Annex-B start code (incl. 4-byte ones).
  std::vector<size_t> cuts;
  for (size_t i = 0; i + 2 < data.size(); i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      cuts.push_back((i > 0 && data[i - 1] == 0) ? i - 1 : i);
      i += 2;
    }
  }
  if (cuts.empty() || cuts[0] != 0)
    cuts.insert(cuts.begin(), 0);
  cuts.push_back(data.size());
  for (int l = 0; l < loops; l++) {
    for (size_t k = 0; k + 1 < cuts.size(); k++) {
      RocdecSourceDataPacket pkt = {};
      pkt.payload = data.data() + cuts[k];
      pkt.payload_size = (uint32_t)(cuts[k + 1] - cuts[k]);
      RD_CHECK(rocDecParseVideoData(parser, &pkt));
    }
    if (l == 0) {
      rss_first = rss_kb();
      gpu_first = gpu_free();
    }
  }
  RocdecSourceDataPacket eos = {};
  eos.flags = ROCDEC_PKT_ENDOFSTREAM;
  RD_CHECK(rocDecParseVideoData(parser, &eos));
  double secs = std::chrono::duration<double>(clk::now() - t0).count();

  long rss_last = rss_kb();
  size_t gpu_last = gpu_free();
  fprintf(stderr, "frames %ld in %.2fs = %.1f fps, avg D2D copy %.3f ms, rss %+ld kB, gpu free %+lld MB\n", c.frames,
          secs, c.frames / secs, c.copy_ms / (c.frames ? c.frames : 1), rss_last - rss_first,
          ((long long)gpu_last - (long long)gpu_first) / (1024 * 1024));

  RD_CHECK(rocDecDestroyVideoParser(parser));
  RD_CHECK(rocDecDestroyDecoder(c.dec));
  HIP_CHECK(hipFree(c.owned));
  HIP_CHECK(hipStreamDestroy(c.stream));
  bool soak_ok = loops == 1 || (rss_last - rss_first < 5 * 1024 &&
                                llabs((long long)gpu_last - (long long)gpu_first) < 16LL * 1024 * 1024);
  return (c.frames > 0 && soak_ok) ? 0 : 3;
}

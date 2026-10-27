# SPDX-License-Identifier: LGPL-2.1-or-later
"""Decodes the tracker test clip (VP8 WebM) and writes its frames to stdout as raw I420 at the clip's own size, for
fetch-tracker-clip.sh to scale and encode. Run by that script in a throwaway Python container with PyAV pinned in
tracker-clip-requirements.txt; the clip's sha256 is checked before this runs.

Usage: decode-tracker-clip.py CLIP WIDTH HEIGHT
"""
import sys

import av


def main():
    path, width, height = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    out = sys.stdout.buffer
    n = 0
    with av.open(path) as container:
        stream = container.streams.video[0]
        for frame in container.decode(stream):
            if frame.width != width or frame.height != height:
                sys.exit(f"frame {n}: {frame.width}x{frame.height}, expected {width}x{height}")
            frame = frame.reformat(format="yuv420p")
            # planes are padded to their line size: write only the visible bytes of each row
            for plane, w, h in zip(frame.planes, (width, width // 2, width // 2),
                                   (height, height // 2, height // 2)):
                data = memoryview(plane)
                for y in range(h):
                    out.write(data[y * plane.line_size:y * plane.line_size + w])
            n += 1
    out.flush()
    print(f"decoded {n} frames", file=sys.stderr)


if __name__ == "__main__":
    main()

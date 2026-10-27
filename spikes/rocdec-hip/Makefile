# SPDX-License-Identifier: LGPL-2.1-or-later
ROCM_INC := /opt/rocm/core-10.1/include
ROCM_LIB := /opt/rocm/core-10.1/lib
GST      := $(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

all: ref_crc rocdec_spike

ref_crc: ref_crc.c
	cc -O2 -Wall -o $@ $< $(GST) -lz

rocdec_spike: rocdec_spike.cpp
	hipcc -O2 -Wall -I$(ROCM_INC) -o $@ $< -L$(ROCM_LIB) -lrocdecode -lz

clean:
	rm -f ref_crc rocdec_spike

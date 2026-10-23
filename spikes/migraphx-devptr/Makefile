MGX_INC := /opt/rocm/extras-10/include
MGX_LIB := /opt/rocm/extras-10/lib

mgx_spike: mgx_spike.cpp
	hipcc -O2 -Wall -I$(MGX_INC) -o $@ $< -L$(MGX_LIB) -lmigraphx_c

clean:
	rm -f mgx_spike

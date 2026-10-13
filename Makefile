# link8b10b - build and run everything:  make test
VERILATOR ?= verilator
CXX       ?= g++
RTL := rtl/link_pkg.sv rtl/scrambler.sv rtl/enc8b10b.sv rtl/dec8b10b.sv rtl/link_top.sv
TB  := $(CURDIR)/tb/tb_main.cpp $(CURDIR)/model/model.cpp
VFLAGS := --cc --exe --build -j 0 -Wall --top-module link_top -CFLAGS "-std=c++20 -O2 -I$(CURDIR)/model" $(VEXTRA)
PACKETS ?= 20000
SEED    ?= 1
FLIP    ?= 1

.PHONY: test lint model-test sim wave clean

test: lint model-test sim

lint:
	$(VERILATOR) --lint-only -Wall --top-module link_top $(RTL)
	@echo "lint: clean"

build/model_tests: tests/model_tests.cpp model/model.cpp model/model.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Imodel tests/model_tests.cpp model/model.cpp -o $@

model-test: build/model_tests
	./build/model_tests

build/obj/Vlink_top: $(RTL) tb/tb_main.cpp model/model.cpp model/model.hpp
	$(VERILATOR) $(VFLAGS) -Mdir build/obj $(RTL) $(TB) -o Vlink_top > build/verilator.log

sim: build/obj/Vlink_top
	./build/obj/Vlink_top --packets $(PACKETS) --seed $(SEED) --flip-bits $(FLIP)

# Same testbench with VCD tracing of the first 20k cycles -> build/wave.vcd (open in GTKWave).
build/obj_trace/Vlink_top: $(RTL) tb/tb_main.cpp model/model.cpp model/model.hpp
	$(VERILATOR) $(VFLAGS) --trace -Mdir build/obj_trace $(RTL) $(TB) -o Vlink_top > build/verilator_trace.log

wave: build/obj_trace/Vlink_top
	./build/obj_trace/Vlink_top --packets 200 --seed $(SEED)
	@echo "waveform: build/wave.vcd"

clean:
	rm -rf build

# Builds one emulated module: the unmodified firmware sketch + the shim in shim/.
CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable -pthread
BUILD    ?= build
FW       := ../vendor/firmware/SplitFlapUniversalFirmware.cpp

all: $(BUILD)/sfmodule

$(BUILD)/sfmodule: $(FW) shim/emu.cpp shim/Arduino.h ../common/vclock.h ../common/buslink.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I shim -D_SS_MAX_RX_BUFF=128 -o $@ $(FW) shim/emu.cpp

clean:
	rm -rf $(BUILD)
.PHONY: all clean

# Builds the emulated gateway: the unmodified SplitFlapGateway sources + the ESP32 core pieces
# vendored verbatim (String/Print/Stream, WebServer) + the shim in shim/.
CXX      ?= c++
CC       ?= cc
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation -Wno-deprecated-declarations -pthread
BUILD    ?= build
V        := ../vendor
INC      := -I shim -I $(V)/esp32core -I $(V)/WebServer -I $(V)/ArduinoJson -I $(V)/PubSubClient -I $(V)/gateway/src
DEFS     := -DARDUINOJSON_ENABLE_PROGMEM=0 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=1 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=1 -DHTTP_UPLOAD_BUFLEN=1436

GW_SRC   := $(wildcard $(V)/gateway/src/*.cpp)
CORE_SRC := $(V)/esp32core/WString.cpp $(V)/esp32core/Print.cpp $(V)/esp32core/Stream.cpp
WEB_SRC  := $(V)/WebServer/WebServer.cpp $(V)/WebServer/Parsing.cpp $(V)/WebServer/detail/mimetable.cpp $(V)/WebServer/middleware/MiddlewareChain.cpp
LIB_SRC  := $(V)/PubSubClient/PubSubClient.cpp
SHIM_SRC := $(wildcard shim/*.cpp)
C_SRC    := $(V)/esp32core/stdlib_noniso.c

OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(GW_SRC) $(CORE_SRC) $(WEB_SRC) $(LIB_SRC) $(SHIM_SRC))) $(BUILD)/stdlib_noniso.o
VPATH := $(V)/gateway/src:$(V)/esp32core:$(V)/WebServer:$(V)/WebServer/detail:$(V)/WebServer/middleware:$(V)/PubSubClient:shim

all: $(BUILD)/sfgateway

$(BUILD)/sfgateway: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(DEFS) -c -o $@ $<

$(BUILD)/stdlib_noniso.o: $(C_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall $(INC) -c -o $@ $<

clean:
	rm -rf $(BUILD)
.PHONY: all clean

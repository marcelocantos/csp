# Standalone consumer of CSP's published drop-in files. No development headers.
ROOT := ../..
BUILD := $(ROOT)/build/lifeboat
CXX := c++
CC := cc
WSLAY := $(ROOT)/vendor/github.com/tatsuhiro-t/wslay/lib
CPPFLAGS := -I$(ROOT)/dist -I$(ROOT)/vendor/github.com/nodejs/llhttp/include \
	-I$(WSLAY)/includes -I$(WSLAY)
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -MMD -MP
ifeq ($(shell uname),Darwin)
CXXFLAGS += -stdlib=libc++
endif
OBJECTS := $(BUILD)/csp.o $(BUILD)/csp_globals.o $(BUILD)/csp_http.o $(BUILD)/csp_ws.o \
	$(BUILD)/llhttp.o $(BUILD)/api.o $(BUILD)/http.o $(BUILD)/lifeboat.o \
	$(BUILD)/wslay_frame.o $(BUILD)/wslay_event.o $(BUILD)/wslay_queue.o $(BUILD)/wslay_net.o
.PHONY: all test
all: $(BUILD)/lifeboat
$(BUILD)/lifeboat: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -pthread
$(BUILD)/lifeboat.o: lifeboat.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(ROOT)/dist/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(ROOT)/vendor/github.com/nodejs/llhttp/src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -O2 -c $< -o $@
$(BUILD)/wslay_%.o: $(WSLAY)/wslay_%.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -O2 -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -c $< -o $@
test: all
	$(BUILD)/lifeboat --self-test --workers 1
	$(BUILD)/lifeboat --self-test --workers 4
	cd $(ROOT) && ./formal/tlc Lifeboat
-include $(OBJECTS:.o=.d)

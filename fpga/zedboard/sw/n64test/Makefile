# Cart test ROM for the N64, built with libdragon (see fpga/zedboard/README.md)

all: n64test.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

# 62 MB of test pattern: with the code, the ROM fills most of the cart's 64 MB
PATTERN_SIZE = 65011712

OBJS = $(BUILD_DIR)/n64test.o

n64test.z64: N64_ROM_TITLE = "ZedBoard cart test"
n64test.z64: $(BUILD_DIR)/n64test.dfs

$(BUILD_DIR)/n64test.dfs: filesystem/pattern.bin
$(BUILD_DIR)/n64test.elf: $(OBJS)

# Runs on the PC: the N64 compiler settings of n64.mk don't apply
$(BUILD_DIR)/mkpattern: mkpattern.c pattern.h
	@mkdir -p $(dir $@)
	cc -O2 -Wall -o $@ $<

filesystem/pattern.bin: $(BUILD_DIR)/mkpattern
	@mkdir -p $(dir $@)
	$< $@ $(PATTERN_SIZE)

clean:
	rm -rf $(BUILD_DIR) filesystem n64test.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)

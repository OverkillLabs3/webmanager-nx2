# SPDX-License-Identifier: MIT
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TARGET := webfm
BUILD := build
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -D__SWITCH__ -D__SWITCH2__ \
    -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib \
    -lkaihen_plugin -lounce_core

all: $(TARGET).nso

$(BUILD):
	mkdir -p $@

source/ui_html.h: source/ui.html tools/gen_ui.py
	python3 tools/gen_ui.py

$(BUILD)/main.o: source/main.c source/webcore.h source/ui_html.h | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD)/webcore.o: source/webcore.c source/webcore.h | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(BUILD)/main.o $(BUILD)/webcore.o
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).nso: $(BUILD)/$(TARGET).elf
	$(DEVKITPRO)/tools/bin/elf2nso $< $@

test:
	gcc -std=gnu11 -Wall -Wextra -Werror source/webcore.c tests/test_webcore.c -o $(BUILD)/test_webcore
	$(BUILD)/test_webcore

clean:
	rm -rf $(BUILD) $(TARGET).nso source/ui_html.h

.PHONY: all clean test

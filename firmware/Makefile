TARGETS := whisker_breeze eeprom_font_writer
MINICHLINK ?= ../ch32fun/minichlink

.PHONY: all flash clean attach flash-whisker_breeze flash-eeprom_font_writer

ifeq ($(TARGET),)

all:
	@for t in $(TARGETS); do \
		$(MAKE) TARGET=$$t build; \
	done

flash: flash-whisker_breeze

flash-whisker_breeze:
	$(MAKE) TARGET=whisker_breeze flash

flash-eeprom_font_writer:
	$(MAKE) TARGET=eeprom_font_writer flash

clean:
	@for t in $(TARGETS); do \
		$(MAKE) TARGET=$$t cv_clean; \
	done

attach:
	$(MINICHLINK)/minichlink -T

else

TARGET_MCU?=CH32V003
TARGET_EXT?=c
ADDITIONAL_C_FILES:=i2c_lowlevel.c eeprom_font_storage.c eeprom_config.c

include ../ch32fun/ch32fun/ch32fun.mk

all: build

flash: cv_flash

clean: cv_clean

endif

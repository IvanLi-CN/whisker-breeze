.PHONY: all flash clean attach

TARGET:=whisker_breeze
TARGET_MCU?=CH32V003
TARGET_EXT?=c

include ../ch32fun/ch32fun/ch32fun.mk

all: flash
flash: cv_flash
clean: cv_clean

attach:
	$(MINICHLINK)/minichlink -T

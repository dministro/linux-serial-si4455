FW_ROOT_DIR = $(shell pwd)
FW_BUILD_DIR = $(FW_ROOT_DIR)/build/fw

FW_PREFIX ?= /lib/firmware
define FW_RCMAIN
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "radio.h"
#include "radio_config.h"
uint8_t Radio_Configuration_Data_Array[] = RADIO_CONFIGURATION_DATA_ARRAY;
int main (int argc, char ** argv) { write(1, Radio_Configuration_Data_Array, sizeof(Radio_Configuration_Data_Array)); }
endef
export FW_RCMAIN

FW_RC = $(shell cat ${RADIO_CONFIG})

$(FW_NAME):
	mkdir -p $(FW_BUILD_DIR)/src
	echo "$$FW_RCMAIN" > $(FW_BUILD_DIR)/src/fwc.c
	mkdir -p $(FW_BUILD_DIR)/src/radio_config
	cat '$(RADIO_CONFIG)' | sed --expression='s/..\\drivers\\radio\\Si4455\\si4455_patch.h/si4455_patch.h/g' > $(FW_BUILD_DIR)/src/radio_config/radio_config.h
	cat '$(FW_ROOT_DIR)/resources/si4455_patch.h' > $(FW_BUILD_DIR)/src/si4455_patch.h
	cat '$(FW_ROOT_DIR)/resources/radio.h' > $(FW_BUILD_DIR)/src/radio.h
	gcc $(FW_BUILD_DIR)/src/fwc.c -o $(FW_BUILD_DIR)/fwc -I '$(FW_BUILD_DIR)/src' -I '$(FW_BUILD_DIR)/src/radio_config'
	chmod +x $(FW_BUILD_DIR)/fwc
	$(FW_BUILD_DIR)/fwc > $(FW_BUILD_DIR)/$(FW_NAME)


firmware: $(FW_NAME)

firmware-install: $(FW_NAME)
	cp $(FW_BUILD_DIR)/$(FW_NAME) $(FW_PREFIX)/

all: firmware-install
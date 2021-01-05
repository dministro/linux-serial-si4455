ROOT_DIR = $(shell pwd)
DL_DIR := $(ROOT_DIR)/dl
O := $(ROOT_DIR)/output

OL_BUILD = $(O)/overlay/build

DTC = dtc

OL_BUS ?= 0
OL_DEV ?= 1
OL_SDN ?= 21
OL_IRQ ?= 20
OL_PACKAGE_SIZE ?= 15
OL_TX_CHANNEL ?= 3
OL_RX_CHANNEL ?= 3
OL_TX_TIMEOUT ?= 100

ifeq ($(OL_FW_NAME),)
	OL_FW_NAME = si4455_spi$(OL_BUS)_$(OL_DEV).ez.bin
	FW_NAME = $(OL_FW_NAME)
endif

OL_DTS_NAME = si4455_spi$(OL_BUS)_$(OL_DEV)-overlay.dts
OL_DTBO_NAME = si4455_spi$(OL_BUS)_$(OL_DEV)
OL_DTBO_FILE = $(OL_DTBO_NAME).dtbo

define boot_change_line
	sed -i /boot/config.txt -i -e 's/^${1}/${2}/'
endef

define boot_add_line
	if [ $(shell egrep -c '^${1}' /boot/config.txt) -eq 0 ]; then \
	echo ${1} >> /boot/config.txt; \
	fi
endef

driver:
	make -C /lib/modules/$(shell uname -r)/build M=$(ROOT_DIR)/../src/linux/drivers/tty/serial modules

firmware:
	make -C ../fw FW_NAME=$(FW_NAME) RADIO_CONFIG=$(RADIO_CONFIG) firmware

overlay:
	mkdir -p $(OL_BUILD)
	cat '$(ROOT_DIR)/resources/overlay.dts.tmpl' | \
	sed --expression='s/_OL_BUS_/$(OL_BUS)/g' | \
	sed --expression='s/_OL_DEV_/$(OL_DEV)/g' | \
	sed --expression='s/_OL_IRQ_/$(OL_IRQ)/g' | \
	sed --expression='s/_OL_SDN_/$(OL_SDN)/g' | \
	sed --expression='s/_OL_PACKAGE_SIZE_/$(OL_PACKAGE_SIZE)/g' | \
	sed --expression='s/_OL_TX_CHANNEL_/$(OL_TX_CHANNEL)/g' | \
	sed --expression='s/_OL_RX_CHANNEL_/$(OL_RX_CHANNEL)/g' | \
	sed --expression='s/_OL_TX_TIMEOUT_/$(OL_TX_TIMEOUT)/g' | \
	sed --expression='s/_OL_FW_NAME_/$(OL_FW_NAME)/g' > \
	$(OL_BUILD)/$(OL_DTS_NAME)
	$(DTC) -W no-unit_address_vs_reg -@ -I dts -O dtb -o $(OL_BUILD)/$(OL_DTBO_FILE) $(OL_BUILD)/$(OL_DTS_NAME)

driver-install: driver
	cp $(ROOT_DIR)/../src/linux/drivers/tty/serial/si4455.ko /lib/modules/$(shell uname -r)/extra
	depmod

firmware-install: firmware
	cp $(ROOT_DIR)/../fw/build/fw/$(FW_NAME) /lib/firmware

overlay-install: overlay
	cp $(OL_BUILD)/$(OL_DTBO_FILE) /boot/overlays

boot-configure:
	$(call boot_change_line,dtparam=spi=off,dtparam=spi=on)
	$(call boot_add_line,dtparam=spi=on)
	$(call boot_add_line,dtoverlay=$(OL_DTBO_NAME))

install: driver-install firmware-install overlay-install

all: install boot-configure
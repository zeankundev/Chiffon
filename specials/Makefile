.PHONY: all help esp32 clean

all: help

help:
	@echo "Special MCU Targets:"
	@echo "  make -C specials/esp32        Build ESP32 (Xtensa LX6)"
	@echo "  make -C specials/esp32 run    Run in ESP32 QEMU"
	@echo "  make -C specials/esp32 flash  Flash to connected board"
	@echo "  make -C specials/esp32 clean  Clean ESP32 build artifacts"

esp32:
	$(MAKE) -C esp32

clean:
	$(MAKE) -C esp32 clean

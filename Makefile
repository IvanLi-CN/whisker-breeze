.PHONY: all flash flash-font clean attach

all:
	$(MAKE) -C firmware all

flash:
	$(MAKE) -C firmware flash

flash-font:
	$(MAKE) -C firmware flash-eeprom_font_writer

clean:
	$(MAKE) -C firmware clean

attach:
	$(MAKE) -C firmware attach

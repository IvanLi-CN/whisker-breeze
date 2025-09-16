.PHONY: all flash clean attach

all: flash

flash:
	$(MAKE) -C firmware flash

clean:
	$(MAKE) -C firmware clean

attach:
	$(MAKE) -C firmware attach

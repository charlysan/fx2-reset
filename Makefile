SDCC    ?= sdcc
PACKIHX ?= packihx

all: reenum.hex

reenum.ihx: reenum.c
	$(SDCC) -mmcs51 --code-size 0x3e00 $<

reenum.hex: reenum.ihx
	$(PACKIHX) $< > $@

clean:
	rm -f *.ihx *.hex *.asm *.lst *.rel *.rst *.sym *.map *.mem *.lk

.PHONY: all clean

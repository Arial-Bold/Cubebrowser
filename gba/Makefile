#---------------------------------------------------------------------------------
# CubeBrowser's Game Boy Advance music player. Builds gbaplayer_mb.gba, a
# program the GameCube sends over the link cable (multiboot, runs from RAM).
# Needs devkitARM + libgba (the GitHub build uses the devkitpro/devkitarm image).
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Use the GitHub build (see README.md)")
endif

PREFIX	:=	$(DEVKITARM)/bin/arm-none-eabi-
CC	:=	$(PREFIX)gcc
OBJCOPY	:=	$(PREFIX)objcopy
LIBGBA	:=	$(DEVKITPRO)/libgba
GBAFIX	:=	$(DEVKITPRO)/tools/bin/gbafix

TARGET	:=	gbaplayer_mb
SRC	:=	main.c link.c player.c gfx.c gfxdata.c ../source/gbaproto.c
CFLAGS	:=	-O2 -Wall -std=gnu11 -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb -mthumb-interwork \
		-ffunction-sections -fdata-sections -I$(LIBGBA)/include -I../source -I.

$(TARGET).gba: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -tCUBEPLAYER -cCBMP -mCB

$(TARGET).elf: $(SRC) $(wildcard *.h) ../source/gbaproto.h
	$(CC) $(CFLAGS) -specs=gba_mb.specs -Wl,--gc-sections -o $@ $(SRC) -L$(LIBGBA)/lib -lgba

clean:
	rm -f $(TARGET).elf $(TARGET).gba

.PHONY: clean

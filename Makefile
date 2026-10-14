#---------------------------------------------------------------------------------
# CubeBrowser - GameCube web browser. Builds cubebrowser.dol
# Requires devkitPPC + libogc + libfat (devkitPro), and mbedTLS 3.6 sources in
# third_party/mbedtls (run ./get-mbedtls.sh once).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "DEVKITPPC is not set. Install devkitPro (gamecube-dev) or use the Docker command in README.md")
endif

include $(DEVKITPPC)/gamecube_rules

MBEDTLS		:=	third_party/mbedtls

# Only check from the top-level folder; the second pass runs inside build/
ifneq (build,$(notdir $(CURDIR)))
ifeq ($(wildcard $(MBEDTLS)/library/ssl_tls.c),)
ifneq ($(MAKECMDGOALS),clean)
$(error "mbedTLS not found in $(MBEDTLS). Run ./get-mbedtls.sh first (see README.md)")
endif
endif
endif

TARGET		:=	cubebrowser
BUILD		:=	build
SOURCES		:=	source $(MBEDTLS)/library
DATA		:=
INCLUDES	:=	source

CFLAGS	=	-g -O2 -Wall -std=gnu11 $(MACHDEP) $(INCLUDE) \
		-DMBEDTLS_USER_CONFIG_FILE='"gc_mbedtls_config.h"'
CXXFLAGS	=	$(CFLAGS)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map

# libfat must come before libogc
LIBS	:=	-lfat -lasnd -lbba -logc -lm
LIBDIRS	:=	$(PORTLIBS)
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CC)
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES	:=	$(CFILES:.c=.o)
export OFILES	:=	$(OFILES_BIN) $(OFILES_SOURCES)
export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
			-I$(CURDIR)/source \
			-I$(CURDIR)/$(MBEDTLS)/include \
			-I$(CURDIR)/$(MBEDTLS)/library \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD) \
			-I$(LIBOGC_INC)

export LIBPATHS	:=	-L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

-include $(DEPENDS)

endif

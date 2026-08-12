#---------------------------------------------------------------------------------
# SwitchSaveSync — overlay (libtesla).
#
# Sai um SwitchSaveSync.ovl, que vai pro cartao em:
#   /switch/.overlays/SwitchSaveSync.ovl
#
# O Ultrahand le essa pasta — overlay de libtesla roda nele igual.
#
# So compila core/syncstate.c e core/titles.c: o overlay nao fala com o
# Google (nada de curl/mbedtls aqui), quem faz rede e o sysmodule.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := SwitchSaveSync

# Um .ovl e um NRO, nao um NSO — foi o motivo de ele nao aparecer na lista do
# Ultrahand (conferido abrindo um .ovl que funciona: magic "HOMEBREW"/"NRO0").
# O nome que aparece no menu vem do NACP, dai o APP_TITLE.
APP_TITLE   := SwitchSaveSync
APP_AUTHOR  := miguel
APP_VERSION := 0.4.0
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES := source lib/libtesla/include ../core

# Do ../core so esses dois entram. O resto (drive/oauth/http) puxaria curl.
CORE_SRC := ../core/syncstate.c ../core/titles.c ../core/lang.c

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections

CFLAGS := -g -Wall -O2 -ffast-math $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fexceptions -frtti -std=gnu++20

ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(CURDIR)/../core \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

# Overlay nao tem icone (o menu nao mostra), mas tem NACP: e de la que sai o
# nome na lista.
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

CFILES   := $(notdir $(CORE_SRC)) $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD := $(CXX)

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).nso $(TARGET).elf

#---------------------------------------------------------------------------------
else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

# .ovl e um NRO com outro nome — e o que o nx-ovlloader/Ultrahand carrega.
$(OUTPUT).ovl : $(OUTPUT).nro
	@cp $< $@
	@echo built ... $(notdir $@)

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
# SwitchSaveSync — sysmodule de autosync.
#
# Sai um exefs.nsp, que vai pro cartao em:
#   /atmosphere/contents/00FF0000535953FF/exefs.nsp
#   /atmosphere/contents/00FF0000535953FF/toolbox.json
#
# NAO existe boot2.flag nessa pasta, e nao deve existir: o sysmodule so sobe
# quando o overlay do Ultrahand manda. Ver ANALISE.md, passo 2.
#
# Compartilha ../core com o app grafico (http/oauth/drive/titles/savemount/
# syncjob/syncstate) — e o mesmo codigo de backup dos dois lados.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

# Antes do include: switch_rules monta a regra do .npdm com $(APP_JSON) como
# pre-requisito na hora em que e lido. Definir depois deixa a regra sem
# pre-requisito nenhum e o npdmtool roda sem argumento.
export APP_JSON := $(TOPDIR)/SwitchSaveSync.json

include $(DEVKITPRO)/libnx/switch_rules

TARGET   := SwitchSaveSync
BUILD    := build
SOURCES  := source ../core
DATA     := data
INCLUDES := source ../core

#---------------------------------------------------------------------------------
# opcoes de geracao de codigo
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections

CFLAGS := -g -Wall -O2 $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# boilerplate padrao do devkitPro
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

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
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf

#---------------------------------------------------------------------------------
else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp

$(OUTPUT).nsp : $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------

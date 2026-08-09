#---------------------------------------------------------------------------------
# SwitchSaveSync — spike de validacao (login Google Drive + upload/download).
# Makefile no formato padrao devkitPro/libnx. Baseado no template usado por
# praticamente todo homebrew de Switch (switch-examples). Se algo aqui não
# bater com a versão do seu devkitPro, compare com um projeto gerado por
# `dkp-pacman -S switch-examples` — a estrutura abaixo é a mesma.
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET   nome do .nro final
# BUILD    diretório de build (objetos intermediários)
# SOURCES  diretórios com código fonte
# DATA     diretórios com arquivos de dados soltos (não usamos)
# INCLUDES diretórios com headers
# ROMFS    conteúdo embutido no romfs (aqui: cacert.pem)
#---------------------------------------------------------------------------------
TARGET      :=  SwitchSaveSync
BUILD       :=  build
SOURCES     :=  source ../core
DATA        :=  data
INCLUDES    :=  source ../core
ROMFS       :=  romfs

APP_TITLE   :=  SwitchSaveSync
APP_AUTHOR  :=  Miguel
APP_VERSION :=  0.1.0

#---------------------------------------------------------------------------------
# opções de geração de código
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  :=  -g -Wall -O2 -ffunction-sections \
            $(ARCH) $(DEFINES) -D__SWITCH__

CFLAGS  +=  $(INCLUDE)

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl + mbedtls (TLS) + z (zlib, dependência do curl). Instale com:
#   sudo dkp-pacman -S switch-curl switch-mbedtls switch-zlib
LIBS    :=  -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx

#---------------------------------------------------------------------------------
# diretórios de libs (portlibs + libnx) — topo contendo include/ e lib/
#---------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# não deveria precisar editar nada daqui pra baixo
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                     $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD   :=  $(CC)
else
    export LD   :=  $(CXX)
endif

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC    :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     :=  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ROMFS)),)
    export NROFLAGS :=
else
    export NROFLAGS :=  --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS +=  --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS +=  --nacp=$(CURDIR)/$(TARGET).nacp
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nso $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else

.PHONY: all

DEPENDS :=  $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all   :   $(OUTPUT).nro

# NROFLAGS (--romfsdir/--icon/--nacp) e' consumido pela regra %.nro do
# switch_rules; NAO e' pre-requisito. Listar aqui fazia o make procurar um
# arquivo chamado "--romfsdir=..." e quebrar o build.
ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro   :   $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro   :   $(OUTPUT).elf
endif

$(OUTPUT).elf   :   $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

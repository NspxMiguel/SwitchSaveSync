#---------------------------------------------------------------------------------
# SwitchSaveSync — versao com interface grafica (borealis).
#
# Baseado no Makefile de exemplo da borealis (lib/borealis/Makefile), com
# duas mudancas:
#   - SOURCES inclui ../core, que e o codigo compartilhado com o app de
#     console em ../app (http/oauth/drive/titles/savemount).
#   - LIBS ganha curl+mbedtls+z, que o ../core precisa. A borealis adiciona
#     as dela (glfw/EGL/glapi/drm_nouveau) via borealis.mk.
#
# Precisa dos pacotes:
#   sudo dkp-pacman -S switch-curl switch-mbedtls switch-zlib \
#                      switch-glfw switch-mesa switch-glm
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      :=  SwitchSaveSync
BUILD       :=  build
SOURCES     :=  source ../core ../core/qrcodegen
DATA        :=  data
INCLUDES    :=  source ../core ../core/qrcodegen
# icone proprio (256x256 JPEG) — gerado por ../assets/make_icon.py
ICON        :=  ../assets/icon.jpg

APP_TITLE   :=  SwitchSaveSync
APP_AUTHOR  :=  Miguel
APP_VERSION :=  0.3.2

# O romfs carrega os recursos da borealis (fontes, icones material, i18n) e
# o cacert.pem que o http.c usa pra validar o TLS do Google.
ROMFS               :=  resources
BOREALIS_PATH       :=  lib/borealis
BOREALIS_RESOURCES  :=  romfs:/

#---------------------------------------------------------------------------------
# opcoes de geracao de codigo
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  :=  -g -Wall -O2 -ffunction-sections \
            $(ARCH) $(DEFINES)

CFLAGS  +=  $(INCLUDE) -D__SWITCH__ \
            -DBOREALIS_RESOURCES="\"$(BOREALIS_RESOURCES)\"" \
            -DAPP_VERSION_STRING="\"$(APP_VERSION)\""

# a borealis exige excecoes e rtti ligados (nada de -fno-exceptions/-fno-rtti)
CXXFLAGS    := $(CFLAGS) -std=c++1z -O2 -Wno-volatile

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

include $(TOPDIR)/$(BOREALIS_PATH)/library/borealis.mk

#---------------------------------------------------------------------------------
# daqui pra baixo e o boilerplate padrao do devkitPro
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

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
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
    export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
    export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD): $(ROMFS)/icon/app.jpg
	@[ -d $@ ] || mkdir -p $@
	@MSYS2_ARG_CONV_EXCL="-D;$(MSYS2_ARG_CONV_EXCL)" $(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# O icone do canto de dentro do app é o MESMO arquivo do icone do .nro. Sai
# copiado pro romfs em vez de estar commitado duas vezes: o icone do app tem que
# ter um dono só, senão um dia trocam um e esquecem o outro — que foi exatamente
# o que aconteceu (dentro do app ficou o icone da borealis).
$(ROMFS)/icon/app.jpg: $(ICON)
	@cp $< $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(ROMFS)/icon/app.jpg

#---------------------------------------------------------------------------------
else

.PHONY: all

DEPENDS :=  $(OFILES:.o=.d)

all :   $(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro   :   $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro   :   $(OUTPUT).elf
endif

$(OUTPUT).elf   :   $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing header files
# ICON is the filename of the icon (.jpg), relative to the project folder,
#   embedded into the .nro via nacptool - this is NOT the catalog-listing
#   icon served by the backend (that's server/public/icons/<id>.jpg).
#---------------------------------------------------------------------------------
TARGET      :=  freeshop-client
BUILD       :=  build
SOURCES     :=  source source/net source/ui source/catalog source/install source/update
INCLUDES    :=  source
ICON        :=  icon.jpg

APP_TITLE   := FreeShop
APP_AUTHOR  := jhon1466
APP_VERSION := 0.1.0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  :=  -g -Wall -O2 -ffunction-sections \
            $(ARCH) $(DEFINES)

CFLAGS  +=  $(INCLUDE) -D__SWITCH__

# SDL2/SDL2_ttf headers live in subdirectories of the generic portlibs
# include path that $(INCLUDE) above doesn't add on its own.
CFLAGS  +=  -I$(DEVKITPRO)/portlibs/switch/include/SDL2 \
            -I$(DEVKITPRO)/portlibs/switch/include/freetype2 \
            -I$(DEVKITPRO)/portlibs/switch/include/harfbuzz \
            -I$(DEVKITPRO)/portlibs/switch/include/libpng16

CXXFLAGS    :=  $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# switch-sdl2/switch-sdl2_ttf and their transitive deps (freetype, harfbuzz,
# libpng, bz2, zlib, mesa's EGL/glapi/drm_nouveau) have circular-ish static
# link dependencies (e.g. freetype needs zlib's inflate, libpng needs zlib's
# crc32, harfbuzz needs libm's hypotf) that a plain left-to-right link order
# can't satisfy - wrapping them in --start-group/--end-group lets the linker
# re-scan until everything resolves. Verified working in this exact
# devkitpro/devkita64 image via a standalone smoke test before wiring this
# into the real project.
LIBS    :=  -Wl,--start-group -lSDL2_ttf -lSDL2 -lEGL -lstdc++ -lglapi -ldrm_nouveau \
            -lfreetype -lbz2 -lpng16 -lharfbuzz -lz -lm -lpthread \
            -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljansson -ljpeg \
            -Wl,--end-group -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS :=  $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD   :=  $(CC)
else
    export LD   :=  $(CXX)
endif

export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_SRC)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.jpg)
    ifneq (,$(findstring $(TARGET).jpg,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).jpg
    else
        ifneq (,$(findstring icon.jpg,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.jpg
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).pfs0 $(TARGET).nso $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS :=  $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all : $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf   :   $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

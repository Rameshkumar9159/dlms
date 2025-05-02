RELATIVE_PATH ?= ./
DLMS_PATH := $(RELATIVE_PATH)DLMS_LIB/development/
SRCS_PATH := $(RELATIVE_PATH)dlms_app/

safe_malloc ?= min

include $(SRCS_PATH)/gurux.defines
CFLAGS += $(GURUX_FLAGS)

ifeq ($(safe_malloc), yes)
# When safe malloc is enabled, gxmalloc, gxfree, ... are redefined as
# their safe malloc counterparts but we also need to use the -include directive
# to automatically inject sfmalloc.h header file in every source file that
# will be compiled

# We add unconditionnaly the safe malloc directory to the list of directories
# to be searched for header files as sfmalloc.h is included unconditionnaly
SFMALLOC_PATH := $(SRCS_PATH)/safemalloc
INCLUDES += -I$(SFMALLOC_PATH)
endif

# General compiler flags (Define it before specific makefile in order to allow app to overwrite it)
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=gnu99 -mthumb -nostartfiles -lgcc -lnosys -ggdb --specs=nano.specs
CFLAGS  += -Os -ffunction-sections -fdata-sections

CFLAGS += -DARM_MATH_ARMV8MML
CFLAGS += -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fms-extensions

# Mcu instruction set
ARCH=armv8-m.main
CFLAGS += -march=$(ARCH)

# Prefix for Arm tools
PREFIX := $(arm_toolchain)arm-none-eabi-

# Toolchain programs
CC          := $(PREFIX)gcc
AR          := $(PREFIX)ar
OBJCOPY     := $(PREFIX)objcopy
RM          := rm -f
MV          := mv
CP          := cp
MKDIR       := mkdir -p
LINKER      := $(AR) rvs

# Generate STATIC library.
TARGET   = libgurux_dlms_c.a

# linking flags here
LFLAGS   = 

# change these to set the proper directories where each files shoould be
SRCDIR   = $(DLMS_PATH)src
OBJDIR   = $(DLMS_PATH)obj
BINDIR   = $(DLMS_PATH)lib


# List of source files
SRCS +=  $(SRCDIR)apdu.c             \
         $(SRCDIR)bytebuffer.c       \
         $(SRCDIR)bitarray.c         \
         $(SRCDIR)ciphering.c        \
         $(SRCDIR)converters.c       \
         $(SRCDIR)cosem.c            \
         $(SRCDIR)datainfo.c         \
         $(SRCDIR)date.c             \
         $(SRCDIR)dlms.c             \
         $(SRCDIR)dlmsSettings.c     \
         $(SRCDIR)gxaes.c            \
         $(SRCDIR)gxarray.c          \
         $(SRCDIR)gxget.c            \
         $(SRCDIR)gxinvoke.c         \
         $(SRCDIR)gxkey.c            \
         $(SRCDIR)gxmd5.c            \
         $(SRCDIR)gxobjects.c        \
         $(SRCDIR)gxset.c            \
         $(SRCDIR)gxsha1.c           \
         $(SRCDIR)gxsha256.c         \
         $(SRCDIR)gxvalueeventargs.c \
         $(SRCDIR)helpers.c          \
         $(SRCDIR)message.c          \
         $(SRCDIR)objectarray.c      \
         $(SRCDIR)parameters.c       \
         $(SRCDIR)replydata.c        \
         $(SRCDIR)client.c           \
         $(SRCDIR)variant.c          \
         $(SRCDIR)gxsetmalloc.c      \
         $(SRCDIR)notify.c           \
         $(SRCDIR)server.c
#        $(SRCDIR)serverevents.c      ## warning due to bad allocation size - implemented in our code

INCLUDES += -I$(DLMS_PATH)

OBJECTS := $(SRCS:$(SRCDIR)%.c=$(OBJDIR)/%.o)

$(BINDIR)/$(TARGET): $(OBJECTS)
	@$(MKDIR) $(BINDIR) 
	@$(LINKER) $@ $(LFLAGS) $(OBJECTS)
	@echo "$(COLOR_DETAILS)CFLAGS are:\n $(CFLAGS)$(COLOR_END)"
	@echo "Linking complete!"

$(OBJECTS): $(OBJDIR)/%.o : $(SRCDIR)/%.c
	@$(MKDIR) $(OBJDIR) 
	@$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@
	@echo "Compiled "$<" successfully!"

.PHONY: clean
clean:
	@$(RM) $(OBJECTS)
	@echo "Cleanup complete!" 
	@echo $(OBJECTS)

.PHONY: remove
remove: clean
	@$(rm) $(BINDIR)/$(TARGET)
	@echo "Executable removed!"

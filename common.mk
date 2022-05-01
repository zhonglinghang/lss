MFNAME = $(word 1, $(MAKEFILE_LIST))

ifeq ($(DEBUG), 1)
NAME = debug_$(MFNAME)
CFLAGS += -O2 -g
else
NAME = release_$(MFNAME)
CFLAGS += -O2 -DNDEBUG
endif

# tool chain
CPP = $(CROSS)g++
CC = $(CROSS)gcc
LD = $(CROSS)ld
AR = $(CROSS)ar
RM = rm -f
STRIP = @echo " strip  $@"; $(CROSS)strip

SRCDIR := $(foreach dir, $(SRCSUBDIR), $(SRCPATH)/$(dir)) $(SRCPATH)

SRCS_C := $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.c))
SRCS_CPP := $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.cpp))

OBJPATH := .obj_$(NAME)
OBJSUBDIR := $(SRCSUBDIR)

BINDIR := $(OBJPATH)/.out

OBJS_C := $(patsubst %.c, %.o, ${SRCS_C})
OBJS_CPP := $(patsubst %.cpp, %.o, ${SRCS_CPP})
OBJS_ORIG := $(sort $(OBJS_C) $(OBJS_CPP))
OBJS := $(patsubst $(SRCPATH)/%, $(OBJPATH)/%, ${OBJS_ORIG})

OBJD_C := $(patsubst %.o, %.c.d, ${OBJS_C})
OBJD_CPP := $(patsubst %.o, %.cpp.d, ${OBJS_CPP})
OBJD_ORIG := $(OBJD_C) $(OBJD_CPP)
OBJD := $(patsubst $(SRCPATH)/%, $(OBJPATH)/%, ${OBJD_ORIG})

INCS := $(foreach inc, $(INCDIR), -I$(inc))

LDLIBS := $(foreach lp, $(LIBDIR), -L$(lp)) -L$(shell pwd) $(foreach ln, $(LIBS), -l$(ln))

ifneq ($(strip ${MAKECMDGOALS}), clean)
$(shell mkdir -p $(OBJPATH))
$(foreach dir, $(OBJSUBDIR), $(shell mkdir -p $(OBJPATH)/$(dir)))
$(shell mkdir -p $(BINDIR))
endif

all: start depends $(TARGET) install end

start:
	$(START_CMD)
	@echo [[[ START $(TARGET) $(NAME) ]]]

end:
	@echo [[[ THE END ]]]

# build
$(TARGET): $(OBJS) $(USER_LIBS)
	@echo [[[ OUTPUT ]]]
# static lib
ifeq ($(BINARY), static)
	@echo " ar $(TARGET)"
	@$(AR) -r $(BINDIR)/$@ $^
# shared lib
else ifeq ($(BINARY), shared)
	@echo " make $(TARGET)"
	@$(CPP) $^ -shared $(LDLIBS) $(LDFLAGS) -o $(BINDIR)/$@
# exec
else ifeq ($(BINARY), exec)
	@echo " make $(TARGET)"
	@$(CPP) $^ $(LDLIBS) $(LDFLAGS) -o $(BINDIR)/$@
else
	@echo NOT support build tyep, exit.
	@exit
endif

# compile
$(OBJPATH)/%.o: $(SRCPATH)/%.c
	@echo " $(CC) $(patsubst $(SRCPATH)/%, %, $<)"
	@$(CC) -c $(INCS) $(CFLAGS) $< -o $@
$(OBJPATH)/%.o: $(SRCPATH)/%.cpp
	@echo " $(CPP) $(patsubst $(SRCPATH)/%, %, $<)"
	@$(CPP) -c $(INCS) $(CFLAGS) $< -o $@

# depends
depends: $(OBJD)
	@echo " make depends"

$(OBJPATH)/%.c.d: $(SRCPATH)/%.c
	@$(CC) $(CFLAGS) $(INCS) -MM -E $^ > $@
	@sed 's/.*\.o/$(subst /,\/, $(dir $@))&/g' $@ >$@.tmp
	@mv $@.tmp $@
$(OBJPATH)/%.cpp.d: $(SRCPATH)/%.cpp
	@$(CPP) $(CFLAGS) $(INCS) -MM -E $^ > $@
	@sed 's/.*\.o/$(subst /,\/, $(dir $@))&/g' $@ >$@.tmp
	@mv $@.tmp $@

# include depends
ifneq ($(strip ${MAKECMDGOALS}), clean)
-include $(OBJD)
endif

# install
install:
	@echo " cp to $(INSTALL_PATH)/$(TARGET)"
	@cp $(BINDIR)/$(TARGET) $(INSTALL_PATH)/

# clean
clean:
	@echo [[[ CLEAN $(TARGET) $(NAME) ]]]
	rm -rf $(OBJPATH)
	rm -f $(INSTALL_PATH)/$(TARGET)
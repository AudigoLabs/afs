PROJECT_NAME := afs_test

PROJECT_DIR := .
AFS_ROOT := ..
BUILD_DIR := build

CC := gcc
CXX := g++

C_SOURCES := \
	$(AFS_ROOT)/src/afs.c \
	$(AFS_ROOT)/src/afs_debug.c \
	$(AFS_ROOT)/src/cache.c \
	$(AFS_ROOT)/src/compile_checks.c \
	$(AFS_ROOT)/src/lookup_table.c \
	$(AFS_ROOT)/src/object_read.c \
	$(AFS_ROOT)/src/object_seek.c \
	$(AFS_ROOT)/src/object_write.c \
	$(AFS_ROOT)/src/open_object_list.c \
	$(AFS_ROOT)/src/storage.c \
	$(AFS_ROOT)/src/util.c

INCLUDE_DIRS := \
	$(PROJECT_DIR) \
	$(AFS_ROOT)/include

C_DEFINES := \
	LOGGING_USE_DATETIME=1

CXX_SOURCES := \
	$(PROJECT_DIR)/main.cpp \
	$(PROJECT_DIR)/test_storage.cpp

MAKEFLAGS += -r
CURR_MAKEFILE := $(firstword $(MAKEFILE_LIST))
OBJS := $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:%=%.o))) $(addprefix $(BUILD_DIR)/,$(notdir $(CXX_SOURCES:%=%.o)))

CFLAGS := $(addprefix -I,$(INCLUDE_DIRS)) -g3 -Og -Werror $(addprefix -D,$(C_DEFINES))
CPP_FLAGS :=
LDFLAGS := -lgtest -lpthread

ifeq ($(OS),Windows_NT)
$(error "Windows is not currently supported")
else
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
# Likely using clang
CFLAGS += -Wno-extern-c-compat
CPP_FLAGS += -std=c++14
else
CPP_FLAGS += -D_Static_assert=static_assert
endif
endif

vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.cpp $(sort $(dir $(CXX_SOURCES)))
-include $(wildcard $(BUILD_DIR)/*.d)

run: $(BUILD_DIR)/$(PROJECT_NAME)
	@echo "Running test..."
	@$<

build: $(BUILD_DIR)/$(PROJECT_NAME)

clean:
	@echo "Deleting $(BUILD_DIR)"
	@rm -rf $(BUILD_DIR)

$(BUILD_DIR)/:
	@echo "Creating $(BUILD_DIR)"
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.c.o: %.c $(CURR_MAKEFILE) | $(BUILD_DIR)/
	@echo "Compiling $(notdir $@)"
	@$(CC) $(CFLAGS) -MMD -MP -MF"$(@:%.c.o=%.d)" -MT"$@" -MT"$@" -o $@ -c $<

$(BUILD_DIR)/%.cpp.o: %.cpp $(CURR_MAKEFILE) | $(BUILD_DIR)/
	@echo "Compiling $(notdir $@)"
	@$(CXX) $(CFLAGS) $(CPP_FLAGS) -MMD -MP -MF"$(@:%.c.o=%.d)" -MT"$@" -MT"$@" -o $@ -c $<

$(BUILD_DIR)/$(PROJECT_NAME): $(OBJS) $(LINKER_SCRIPT)
	@echo "Linking $(notdir $@)"
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)

.PHONY: build clean
.DEFAULT_GOAL := run

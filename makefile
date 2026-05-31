CC := gcc
CFLAGS := -Wall -O1
CYAN := [96m
PURPLE := [94m
GREEN := [92m
RED := [91m
RESET := [0m

RM := rm -fr
MV := mv

YAP_CFLAGS := $(shell yap --cflags)

debug ?= false
ifeq ($(debug),true)
    CFLAGS += -g -fno-omit-frame-pointer
endif

log := $(debug)
ifeq ($(log),true)
    CFLAGS += -DYAP_LOG
endif

YAP_SEM_FLAGS := -I./include $(YAP_CFLAGS) $(CFLAGS)
YAP_SEM_LIB := ./libyap_semantic.so

.PHONY: all clean

default: all

all:
	@echo $(PURPLE)Building yap-semantic component$(RESET)
	$(CC) -fPIC $(YAP_SEM_FLAGS) src/*.c -c
	$(CC) -shared -o $(YAP_SEM_LIB) ./*.o
	$(RM) ./*.o
	@echo $(GREEN)Done!$(RESET)

clean:
	$(RM) $(YAP_SEM_LIB) ./*.o

CC := gcc
CFLAGS := -Wall -Wextra -O1
CYAN := \033[96m
PURPLE := \033[94m
GREEN := \033[92m
RESET := \033[0m

RM := rm -fr

YAP_CFLAGS := $(shell yap --cflags)
YAP_SEMANTIC_FLAGS := $(YAP_CFLAGS) -I./include $(CFLAGS)
YAP_SEMANTIC_LIB := ./libyap_semantic.so

.PHONY: default all clean

default: all

all:
	@printf "$(PURPLE)Building yap-semantic module$(RESET)\n"
	@printf "$(CYAN)Building objects$(RESET)\n"
	$(CC) -fPIC $(YAP_SEMANTIC_FLAGS) src/*.c -c
	@printf "$(CYAN)Building shared library$(RESET)\n"
	$(CC) -shared -o $(YAP_SEMANTIC_LIB) ./*.o -lyap
	$(RM) ./*.o
	@printf "$(GREEN)Done!$(RESET)\n"

clean:
	$(RM) $(YAP_SEMANTIC_LIB) ./*.o

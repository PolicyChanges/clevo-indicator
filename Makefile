vpath %.c ../src

CC = gcc
CFLAGS = -c -g -Wall -std=gnu99
LDFLAGS = -g

DSTDIR := /usr/local
OBJDIR := obj
SRCDIR := src

SRC = clevo-indicator.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC)) 

TARGET = bin/clevo-indicator


NVML_LIB := /lib/
NVML_LIB_L := $(addprefix -L , $(NVML_LIB))
CFLAGS += `pkg-config --cflags ayatana-appindicator3-0.1`
LDFLAGS += `pkg-config --libs ayatana-appindicator3-0.1`
LDFLAGS += -lnvidia-ml $(NVML_LIB_L)
all: $(TARGET)

install: $(TARGET)
	@echo Install to ${DSTDIR}/bin/
	@sudo install -m 4750 -g adm $(TARGET) ${DSTDIR}/bin/

test: $(TARGET)
	@sudo chown root $(TARGET)
	@sudo chgrp adm  $(TARGET)
	@sudo chmod 4750 $(TARGET)

$(TARGET): $(OBJ) Makefile
	@mkdir -p bin
	@echo linking $(TARGET) from $(OBJ)
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS) -lm
	@sudo chown root $(TARGET)
	@sudo chmod u+s $(TARGET)

debug: $(TARGET)
	@mkdir -p bin
	@echo linking $(TARGET) from $(OBJ)
	@$(CC) -ggdb $(OBJ) -o $(TARGET) $(LDFLAGS) -lm
	@sudo chown root $(TARGET)
	@sudo chmod u+s $(TARGET)
	@sudo gdb $(TARGET)

clean:
	rm $(OBJ) $(TARGET)

$(OBJDIR)/%.o : $(SRCDIR)/%.c Makefile
	@echo compiling $< 
	@mkdir -p obj
	@$(CC) $(CFLAGS) -c $< -o $@

run:
	./bin/clevo-indicator
#$(OBJECTS): | obj

#obj:
#	@mkdir -p $@

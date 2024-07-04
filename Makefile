CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g

OUT ?= build

BIN = $(OUT)/kvm-cmd

all: $(BIN)

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
else
    Q := @
    VECHO = @printf
endif

OBJS := vm.o kvm-cmd.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^

$(OUT)/%.o: src/%.c
	@mkdir -p .$(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

clean:
	rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	rm -rf build

-include $(deps)

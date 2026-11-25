CC ?= cc
CLANG ?= clang
BPFTOOL ?= bpftool
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g

OUTPUT := build
LIBBPF_CFLAGS = $(shell $(PKG_CONFIG) --cflags libbpf)
LIBBPF_LIBS = $(shell $(PKG_CONFIG) --libs libbpf)

# Clang's BPF target still needs the native Linux asm/types.h headers.
# Debian/Ubuntu place these in the compiler's multiarch include directory.
MULTIARCH = $(shell $(CC) -dumpmachine)

.PHONY: all clean
.DELETE_ON_ERROR:

all: $(OUTPUT)/exec_trace

$(OUTPUT):
	mkdir -p $@

$(OUTPUT)/exec_trace.bpf.o: exec_trace.bpf.c | $(OUTPUT)
	$(PKG_CONFIG) --exists libbpf
	$(CLANG) -g -O2 -std=gnu11 -target bpf -Wall -Wextra -Werror \
		$(LIBBPF_CFLAGS) -idirafter /usr/include/$(MULTIARCH) \
		-c $< -o $@

$(OUTPUT)/exec_trace.skel.h: $(OUTPUT)/exec_trace.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/exec_trace: exec_trace.c $(OUTPUT)/exec_trace.skel.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(LIBBPF_CFLAGS) -I$(OUTPUT) exec_trace.c \
		-o $@ $(LDFLAGS) $(LIBBPF_LIBS)

clean:
	$(RM) $(OUTPUT)/exec_trace $(OUTPUT)/exec_trace.skel.h $(OUTPUT)/exec_trace.bpf.o

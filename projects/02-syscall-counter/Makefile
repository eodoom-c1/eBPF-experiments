CC ?= cc
CLANG ?= clang
BPFTOOL ?= bpftool
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g

OUTPUT := build
LIBBPF_CFLAGS = $(shell $(PKG_CONFIG) --cflags libbpf)
LIBBPF_LIBS = $(shell $(PKG_CONFIG) --libs libbpf)
MULTIARCH = $(shell $(CC) -dumpmachine)

.PHONY: all clean
.DELETE_ON_ERROR:

all: $(OUTPUT)/syscall_counts

$(OUTPUT):
	mkdir -p $@

$(OUTPUT)/syscall_counts.bpf.o: syscall_counts.bpf.c syscall_counts.h | $(OUTPUT)
	$(PKG_CONFIG) --exists libbpf
	$(CLANG) -g -O2 -std=gnu11 -target bpf -Wall -Wextra -Werror \
		$(LIBBPF_CFLAGS) -idirafter /usr/include/$(MULTIARCH) \
		-c $< -o $@

$(OUTPUT)/syscall_counts.skel.h: $(OUTPUT)/syscall_counts.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/syscall_counts: syscall_counts.c syscall_counts.h $(OUTPUT)/syscall_counts.skel.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(LIBBPF_CFLAGS) -I$(OUTPUT) syscall_counts.c \
		-o $@ $(LDFLAGS) $(LIBBPF_LIBS)

clean:
	$(RM) $(OUTPUT)/syscall_counts $(OUTPUT)/syscall_counts.skel.h $(OUTPUT)/syscall_counts.bpf.o

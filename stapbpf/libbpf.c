/* eBPF mini library */
#include <stdlib.h>
#include <stdio.h>
#include <linux/unistd.h>
#include <unistd.h>
#include <string.h>
#include <linux/netlink.h>
#include <linux/bpf.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>
#include "libbpf.h"

/* Older headers might not have this defined yet. */
#ifndef __NR_bpf
# if defined(__i386__)
#  define __NR_bpf 357
# elif defined(__x86_64__)
#  define __NR_bpf 321
# elif defined(__aarch64__)
#  define __NR_bpf 280
# else
#  error __NR_bpf not defined.
# endif
#endif

static __u64 ptr_to_u64(void *ptr)
{
	return (__u64) (unsigned long) ptr;
}

int bpf_create_map(enum bpf_map_type map_type, unsigned key_size,
		   unsigned value_size, unsigned max_entries,
		   unsigned flags __attribute__((unused)))
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.map_type = map_type;
	attr.key_size = key_size;
	attr.value_size = value_size;
	attr.max_entries = max_entries;

	return syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
}

int bpf_update_elem(int fd, void *key, void *value, unsigned long long flags)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.map_fd = fd;
	attr.key = ptr_to_u64(key);
	attr.value = ptr_to_u64(value);
	attr.flags = flags;

	return syscall(__NR_bpf, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
}

int bpf_lookup_elem(int fd, void *key, void *value)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.map_fd = fd;
	attr.key = ptr_to_u64(key);
	attr.value = ptr_to_u64(value);

	return syscall(__NR_bpf, BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

int bpf_delete_elem(int fd, void *key)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.map_fd = fd;
	attr.key = ptr_to_u64(key);

	return syscall(__NR_bpf, BPF_MAP_DELETE_ELEM, &attr, sizeof(attr));
}

int bpf_get_next_key(int fd, void *key, void *next_key)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.map_fd = fd;
	attr.key = ptr_to_u64(key);
	attr.next_key = ptr_to_u64(next_key);

	return syscall(__NR_bpf, BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr));
}

#define ROUND_UP(x, n) (((x) + (n) - 1u) & ~((n) - 1u))

char bpf_log_buf[LOG_BUF_SIZE];
extern int log_level; // set from stapbpf command line

int bpf_prog_load(enum bpf_prog_type prog_type,
		  const struct bpf_insn *insns, int prog_len,
		  const char *license, int kern_version)
{
	union bpf_attr attr;
        memset (&attr, 0, sizeof(attr)); // kernel asserts 0 pad values
	attr.prog_type = prog_type;
	attr.insns = ptr_to_u64((void *) insns);
	attr.insn_cnt = prog_len / sizeof(struct bpf_insn);
	attr.license = ptr_to_u64((void *) license);

        if (log_level)
          {
            attr.log_buf = ptr_to_u64(bpf_log_buf);
            attr.log_size = LOG_BUF_SIZE;
            attr.log_level = log_level;
            /* they hang together, or they hang separately with -EINVAL */
          }

	/* assign one field outside of struct init to make sure any
	 * padding is zero initialized
	 */
	attr.kern_version = kern_version;

	bpf_log_buf[0] = 0;

        if (log_level > 1)
          fprintf(stderr, "Loading probe type %d, size %d\n", prog_type, prog_len);

	return syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
}

int bpf_obj_pin(int fd, const char *pathname)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.pathname	= ptr_to_u64((void *)pathname);
	attr.bpf_fd	= fd;

	return syscall(__NR_bpf, BPF_OBJ_PIN, &attr, sizeof(attr));
}

int bpf_obj_get(const char *pathname)
{
	union bpf_attr attr;
        memset(&attr, 0, sizeof(union bpf_attr));
	attr.pathname = ptr_to_u64((void *)pathname);

	return syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr));
}

int open_raw_sock(const char *name)
{
	struct sockaddr_ll sll;
	int sock;

	sock = socket(PF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (sock < 0) {
		printf("cannot create raw socket\n");
		return -1;
	}

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = if_nametoindex(name);
	sll.sll_protocol = htons(ETH_P_ALL);
	if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		printf("bind to %s: %s\n", name, strerror(errno));
		close(sock);
		return -1;
	}

	return sock;
}

int perf_event_open(struct perf_event_attr *attr, int pid, int cpu,
		    int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu,
		       group_fd, flags);
}

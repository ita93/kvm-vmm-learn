#include <asm/bootparam.h>
#include <asm/kvm.h>
#include <asm/kvm_para.h>
#include <bits/types.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RAM_SIZE (1 << 30) // 1GB of ram

typedef struct vm {
  int kvm_fd, vm_fd, vcpu_fd; // file descriptors
  void *mem;
} vm_t;

static int throw_err(const char *fmt, ...) {
  // va_list, va_start, va_end are the handlers of variable list
  // it is the ...
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args); // vfprint : print on an variable list
  fprintf(stderr, ", errno: %d\n", errno);
  va_end(args);
  return -1;
}

// Registers initialization
static int vm_init_regs(vm_t *g) {
  struct kvm_sregs sregs;

  if (ioctl(g->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
    return throw_err("Failed to get registers");

// all segment selector are the same
#define X(R) sregs.R.base = 0, sregs.R.limit = ~0, sregs.R.g = 1
  X(cs), X(ds), X(fs), X(gs), X(es), X(ss);
#undef X

  sregs.cs.db = 1;
  sregs.ss.db = 1;
  sregs.cr0 |= 1; /* enable protected mode */

  if (ioctl(g->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
    return throw_err("Failed to set special registers");

  struct kvm_regs regs;
  if (ioctl(g->vcpu_fd, KVM_GET_REGS, &regs) < 0)
    return throw_err("Failed to get registers");

  regs.rflags = 2;
  // similar to qemu?
  regs.rip = 0x100000, regs.rsi = 0x10000;
  if (ioctl(g->vcpu_fd, KVM_SET_REGS, &regs) < 0)
    return throw_err("Failed to set registers");

  return 0;
}

#define N_ENTRIES 100
static void vm_init_cpu_id(vm_t *g) {
  struct {
    uint32_t nent;
    uint32_t padding;
    struct kvm_cpuid_entry2 entries[N_ENTRIES];
  } kvm_cpuid = {.nent = N_ENTRIES};
  ioctl(g->kvm_fd, KVM_GET_SUPPORTED_CPUID, &kvm_cpuid);

  for (unsigned int i = 0; i < N_ENTRIES; i++) {
    struct kvm_cpuid_entry2 *entry = &kvm_cpuid.entries[i];
    if (entry->function == KVM_CPUID_SIGNATURE) {
      entry->eax = KVM_CPUID_FEATURES;
      entry->ebx = 0x4b4d564b; // KVMK
      entry->ecx = 0x564b4d56; // VMKV
      entry->edx = 0x4d;       // M
    }
  }

  ioctl(g->vcpu_fd, KVM_SET_CPUID2, &kvm_cpuid);
}

int vm_init(vm_t *g) {
  printf("Initializing VM\n");

  if ((g->kvm_fd = open("/dev/kvm", O_RDWR)) < 0)
    return throw_err("Failed to open /dev/kvm");

  if ((g->vm_fd = ioctl(g->kvm_fd, KVM_CREATE_VM, 0)) < 0)
    return throw_err("Failed to create VM");

  if (ioctl(g->vm_fd, KVM_SET_TSS_ADDR, 0xffffd000) < 0)
    return throw_err("Failed to set TSS addres");

  __u64 map_addr = 0xffffc000;
  // This mapping allow intel to switch between different CPU Mode
  if (ioctl(g->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr) < 0)
    return throw_err("Failed to set identity map address");

  if (ioctl(g->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
    return throw_err("Failed to create interrupt controller model");
  }

  struct kvm_pit_config pit = {.flags = 0};
  if (ioctl(g->vm_fd, KVM_CREATE_PIT2, &pit) < 0)
    return throw_err("Failed to create i8254 interval timer");

  // Create memory for the VM
  g->mem = mmap(NULL, RAM_SIZE, PROT_WRITE | PROT_READ,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!g->mem)
    return throw_err("Failed to mmap vm memory");

  struct kvm_userspace_memory_region region = {
      .slot = 0,
      .flags = 0,
      .guest_phys_addr = 0,
      .memory_size = RAM_SIZE,
      .userspace_addr = (__u64)g->mem,
  };

  // Because the memory is smaller than 4G, it won't overlap with the MMIO
  if (ioctl(g->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    return throw_err("Failed to setup user memory region");

  if ((g->vcpu_fd = ioctl(g->vm_fd, KVM_CREATE_VCPU, 0)) < 0)
    return throw_err("Failed to create vcpu");

  vm_init_regs(g);
  vm_init_cpu_id(g);

  return 0;
}

int vm_load(vm_t *g, const char *image_path) {
  int fd = open(image_path, O_RDONLY);
  if (fd < 0)
    return 1;

  struct stat st;
  fstat(fd, &st);
  size_t datasz = st.st_size;
  void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);

  struct boot_params *boot =
      (struct boot_params *)((uint8_t *)g->mem + 0x10000);
  void *cmdline = ((uint8_t *)g->mem) + 0x20000;
  void *kernel = ((uint8_t *)g->mem) + 0x100000;

  memset(boot, 0, sizeof(struct boot_params));
  memmove(boot, data, sizeof(struct boot_params));

  size_t setup_sectors = boot->hdr.setup_sects;
  size_t setupsz = (setup_sectors + 1) * 512; // ech sector is 512 bytes

  boot->hdr.vid_mode = 0xFFFF; // VGA
  boot->hdr.type_of_loader = 0xFF;
  boot->hdr.ramdisk_size = 0x0; // NO initramfs at this moment
  boot->hdr.ramdisk_size = 0x0;
  boot->hdr.loadflags |= CAN_USE_HEAP | 0x01 | KEEP_SEGMENTS;
  boot->hdr.heap_end_ptr = 0xFE00;
  boot->hdr.ext_loader_ver = 0x0;
  boot->hdr.cmd_line_ptr = 0x20000;
  memset(cmdline, 0, boot->hdr.cmdline_size);
  memcpy(cmdline, "console=ttyS0", 14);
  memmove(kernel, (char *)data + setupsz, datasz - setupsz);

  return 0;
}

void vm_exit(vm_t *g) {
  close(g->kvm_fd);
  close(g->vm_fd);
  close(g->vcpu_fd);
  munmap(g->mem, RAM_SIZE);
}

int vm_run(vm_t *g) {
  int run_size = ioctl(g->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run =
      mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, g->vcpu_fd, 0);

  while (1) {
    if (ioctl(g->vcpu_fd, KVM_RUN, 0) < 0)
      return throw_err("Failed to execute kvm_run");

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
      if (run->io.port == 0x3f8 && run->io.direction == KVM_EXIT_IO_OUT) {
        uint32_t size = run->io.size;
        uint64_t offset = run->io.data_offset;
        printf("%.*s", size * run->io.count, (char *)run + offset);
      } else if ((run->io.port == 0x3f8 + 5) &&
                 (run->io.direction == KVM_EXIT_IO_IN)) {
        *((char *)run + run->io.data_offset) = 0x20;
      }
      break;
    case KVM_EXIT_SHUTDOWN:
      printf("shutdown \n");
      return 0;
    default:
      printf("reason: %d\n", run->exit_reason);
      return -1;
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2)
    return fprintf(stderr, "Usage: %s [filename]\n", argv[0]);

  printf("Start vm with bzimage: %s\n", argv[1]);
  vm_t vm;
  if (vm_init(&vm) < 0)
    return throw_err("Failed to initialize guest vm");

  if (vm_load(&vm, argv[1]) < 0)
    return throw_err("Failed to load guest image");

  printf("Running VM\n");
  vm_run(&vm);
  vm_exit(&vm);

  return 0;
}

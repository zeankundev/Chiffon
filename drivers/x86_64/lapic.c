/* Local APIC (Advanced Programmable Interrupt Controller) driver for x86_64 SMP.
 * Implements CPU discovery via MP table, INIT/SIPI/SIPI sequence for secondary CPU bringup,
 * IPI (inter-processor interrupt) delivery, and LAPIC timer configuration.
 * See: Intel SDM Volume 3, Chapter 10 "Advanced Programmable Interrupt Controller (APIC)". */

#include "kernel.h"
#include "drivers/x86io.h"

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_ENABLE   0x800

static uintptr_t lapic_base;        /* virtual address of LAPIC MMIO region */

/* LAPIC register offsets (from base, 4 bytes each, 16-byte aligned) */
#define LAPIC_ID            0x020   /* Local APIC ID */
#define LAPIC_VERSION       0x030   /* Local APIC Version */
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_LDR           0x0D0   /* Logical Destination Register */
#define LAPIC_DFR           0x0E0   /* Destination Format Register */
#define LAPIC_SPURIOUS      0x0F0   /* Spurious Interrupt Vector */
#define LAPIC_ISR_BASE      0x100   /* In-Service Register (0x100-0x170, 8 regs) */
#define LAPIC_TMR_BASE      0x180   /* Trigger Mode Register (0x180-0x1F0, 8 regs) */
#define LAPIC_IRR_BASE      0x200   /* Interrupt Request Register (0x200-0x270, 8 regs) */
#define LAPIC_ESR           0x280   /* Error Status Register */
#define LAPIC_CMCI          0x2F0   /* CMCI LVT */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register (Lo) */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register (Hi) */
#define LAPIC_LVT_TIMER     0x320   /* LVT Timer */
#define LAPIC_LVT_THERMAL   0x330   /* LVT Thermal Sensor */
#define LAPIC_LVT_PERF      0x340   /* LVT Performance Counter */
#define LAPIC_LVT_LINT0     0x350   /* LVT Local Interrupt 0 (LINT0) */
#define LAPIC_LVT_LINT1     0x360   /* LVT Local Interrupt 1 (LINT1) */
#define LAPIC_LVT_ERROR     0x370   /* LVT Error */
#define LAPIC_TIMER_ICR     0x380   /* Timer Initial Count Register */
#define LAPIC_TIMER_CCR     0x390   /* Timer Current Count Register */
#define LAPIC_TIMER_DCR     0x3E0   /* Timer Divide Configuration Register */

/* ICR command encoding */
#define ICR_VECTOR(v)       ((v) & 0xFF)
#define ICR_DELMODE_FIXED   0x000
#define ICR_DELMODE_LOWEST  0x100
#define ICR_DELMODE_SMI     0x200
#define ICR_DELMODE_REMOTE  0x300
#define ICR_DELMODE_INIT    0x500
#define ICR_DELMODE_STARTUP 0x600
#define ICR_DESTMODE_PHYSICAL 0
#define ICR_DESTMODE_LOGICAL  0x800
#define ICR_LEVEL_DEASSERT  0
#define ICR_LEVEL_ASSERT    0x4000
#define ICR_TRIGGER_EDGE    0
#define ICR_TRIGGER_LEVEL   0x8000
#define ICR_DEST_SELF       0x40000
#define ICR_DEST_ALL        0x80000
#define ICR_DEST_ALL_EXCEPT 0xC0000
#define ICR_DEST_FIELD(cpu) ((uint64_t)(cpu) << 56)

/* LVT delivery modes for LINT0/LINT1 */
#define LVT_EXTINT 0x700
#define LVT_NMI    0x400

static inline uint32_t lapic_read(uint32_t offset) {
	return *(volatile uint32_t *)(lapic_base + offset);
}

static inline void lapic_write(uint32_t offset, uint32_t value) {
	*(volatile uint32_t *)(lapic_base + offset) = value;
}

static inline uint64_t rdmsr(uint32_t m) {
	uint32_t l, h;
	__asm__ volatile("rdmsr" : "=a"(l), "=d"(h) : "c"(m));
	return (uint64_t)h << 32 | l;
}

static inline void wrmsr(uint32_t m, uint64_t v) {
	__asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* PIT channel 2 provides a boot-time clock before scheduler ticks are available. */
static void lapic_delay_us(unsigned us) {
	while (inb(0x61) & 1) outb(0x61, inb(0x61) & (uint8_t)~1);
	outb(0x61, (inb(0x61) & (uint8_t)~2) | 1);
	outb(0x43, 0xB0);                         /* channel 2, one-shot, low/high */
	uint32_t count = (1193182U * us + 999999U) / 1000000U;
	if (count > 65535) count = 65535;
	outb(0x42, count & 0xff); outb(0x42, count >> 8);
	while (!(inb(0x61) & 0x20)) __asm__ volatile("pause");
	outb(0x61, inb(0x61) & (uint8_t)~1);
}

static void lapic_wait_icr(void) {
	for (unsigned n = 0; n < 1000000; n++) {
		if (!(lapic_read(LAPIC_ICR_LO) & 0x1000)) return;
		__asm__ volatile("pause");
	}
}

/* ---- MP Table parsing ---- */

struct mp_floating {
	char sig[4];       /* "_MP_" */
	uint32_t phys;     /* physical address of MP Configuration Table */
	uint8_t len;
	uint8_t spec;
	uint8_t checksum;
	uint8_t features[5];
} __attribute__((packed));

struct mp_config_header {
	char sig[4];       /* "PCMP" */
	uint16_t len;
	uint8_t spec;
	uint8_t checksum;
	char oemid[8];
	char productid[12];
	uint32_t oem_table_ptr;
	uint16_t oem_table_size;
	uint16_t entry_count;
	uint32_t lapic_phys;
	uint16_t extended_len;
	uint8_t extended_checksum;
	uint8_t reserved;
} __attribute__((packed));

struct mp_processor {
	uint8_t type;          /* 0 = processor */
	uint8_t lapic_id;
	uint8_t lapic_ver;
	uint8_t cpu_flags;     /* bit 0 = usable, bit 1 = BSP */
	uint32_t cpu_sig;
	uint32_t cpu_feat;
} __attribute__((packed));

#define MP_CPU_USABLE   1
#define MP_CPU_BSP      2

static int cpu_count = 0;
static struct {
	uint8_t id;
	int usable;
} cpus_found[MAX_CPUS];

static struct mp_floating *find_mp_floating(void) {
	/* Search EBDA (Extended BIOS Data Area) at 0x400:0x0E */
	uint16_t ebda_seg = *(volatile uint16_t *)P2V(0x40E);
	uintptr_t ebda_addr = (uintptr_t)ebda_seg << 4;
	if (ebda_addr && ebda_addr < 0x100000) {
		int len = 1024;  /* up to 1 KiB from EBDA start */
		for (uintptr_t a = ebda_addr; a < ebda_addr + len; a += 16) {
			struct mp_floating *mp = (struct mp_floating *)P2V(a);
			if (mp->sig[0] == '_' && mp->sig[1] == 'M' && mp->sig[2] == 'P' && mp->sig[3] == '_') {
				return mp;
			}
		}
	}
	/* Fall back: search last 64 KiB of conventional memory */
	for (uintptr_t a = 0xE0000; a < 0x100000; a += 16) {
		struct mp_floating *mp = (struct mp_floating *)P2V(a);
		if (mp->sig[0] == '_' && mp->sig[1] == 'M' && mp->sig[2] == 'P' && mp->sig[3] == '_') {
			return mp;
		}
	}
	return 0;
}

static int parse_mp_table(void) {
	struct mp_floating *floating = find_mp_floating();
	if (!floating) {
		kprintf("lapic: no MP floating pointer found\n");
		return 0;
	}

	struct mp_config_header *config = (struct mp_config_header *)P2V(floating->phys);
	if (config->sig[0] != 'P' || config->sig[1] != 'C' || config->sig[2] != 'M' || config->sig[3] != 'P') {
		kprintf("lapic: invalid MP config signature\n");
		return 0;
	}

	uintptr_t addr = floating->phys + sizeof(struct mp_config_header);
	uint32_t entries = config->entry_count;
	cpu_count = 0;

	for (uint16_t i = 0; i < entries && cpu_count < MAX_CPUS; i++) {
		struct mp_processor *proc = (struct mp_processor *)P2V(addr);
		if (proc->type == 0) {  /* processor entry */
			cpus_found[cpu_count].id = proc->lapic_id;
			cpus_found[cpu_count].usable = (proc->cpu_flags & MP_CPU_USABLE) ? 1 : 0;
			cpu_count++;
			kprintf("lapic: found CPU %d (LAPIC ID %d, %s)\n",
					cpu_count - 1, proc->lapic_id,
					(proc->cpu_flags & MP_CPU_USABLE) ? "usable" : "disabled");
		}
		addr += 8;  /* each entry is 8 bytes */
	}

	return cpu_count;
}

/* ---- Initialization and IPI ---- */

int board_smp_start(uintptr_t entry_pa) {
	(void)entry_pa;
	if (cpu_count <= 1) return 0;  /* no secondary CPUs found */
	/* Secondary CPUs start in real mode at the SIPI vector. There is no x86 real-mode trampoline yet, so
	 * starting them now would run whatever bytes sit at that address. Stay on the boot CPU unless the
	 * kernel is booted with smp=on (for experiments only). */
	const char *smp = cmdline_get("smp");
	if (!smp || kstrcasecmp(smp, "on")) {
		kprintf("smp: %d CPUs found, running on 1 (x86 secondary startup needs a trampoline; boot with smp=on to try)\n", cpu_count);
		return 0;
	}

	int secondary_count = 0;
	int bsp_id = lapic_read(LAPIC_ID) >> 24;

	for (int i = 0; i < cpu_count; i++) {
		uint8_t lapic_id = cpus_found[i].id;
		if (lapic_id == bsp_id || !cpus_found[i].usable) continue;  /* skip BSP and disabled */

		/* Send INIT IPI to secondary CPU */
		uint64_t icr_val = ICR_DELMODE_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL | ICR_DEST_FIELD(lapic_id);
		lapic_write(LAPIC_ICR_HI, (uint32_t)(icr_val >> 32));
		lapic_write(LAPIC_ICR_LO, (uint32_t)icr_val);

		/* Wait for INIT to complete */
		lapic_wait_icr();

		/* Delay 10 ms */
		lapic_delay_us(10000);

		/* Send SIPI (Startup IPI) twice to secondary CPU */
		for (int sipi = 0; sipi < 2; sipi++) {
			uint32_t page = entry_pa >> 12;  /* vector = page number */
			icr_val = ICR_DELMODE_STARTUP | (page & 0xFF) | ICR_DEST_FIELD(lapic_id);
			lapic_write(LAPIC_ICR_HI, (uint32_t)(icr_val >> 32));
			lapic_write(LAPIC_ICR_LO, (uint32_t)icr_val);

			/* Wait for SIPI to complete */
			lapic_wait_icr();

			/* Delay 200 µs between SIPIs */
			if (!sipi) lapic_delay_us(200);
		}

		secondary_count++;
		kprintf("lapic: started secondary CPU %d (LAPIC ID %d)\n", secondary_count, lapic_id);
	}

	return secondary_count;
}

void board_ipi_send(int cpu) {
	if (cpu < 0 || cpu >= cpu_count) return;
	uint8_t lapic_id = cpus_found[cpu].id;
	if (!cpus_found[cpu].usable) return;

	/* Send RESCHED IPI (vector 0x20 + offset for reschedule, typically 0xF0 or similar) */
	uint64_t icr_val = 0xF0 | ICR_DELMODE_FIXED | ICR_DEST_FIELD(lapic_id);
	lapic_write(LAPIC_ICR_HI, (uint32_t)(icr_val >> 32));
	lapic_write(LAPIC_ICR_LO, (uint32_t)icr_val);
}

void lapic_init(void) {
	/* Enable APIC via MSR */
	uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
	lapic_base = (uintptr_t)P2V(apic_msr & 0xFFFFF000);
	wrmsr(IA32_APIC_BASE_MSR, apic_msr | IA32_APIC_ENABLE);

	/* Try to map the APIC base address as device memory (uncacheable) */
	arch_map_device(apic_msr & 0xFFFFF000, 4096);

	/* Initialize LAPIC registers */
	lapic_write(LAPIC_DFR, 0xFFFFFFFF);             /* flat mode */
	lapic_write(LAPIC_LDR, 1 << (lapic_read(LAPIC_ID) >> 24));
	lapic_write(LAPIC_SPURIOUS, 0x1FF);             /* spurious vector 0xFF, APIC enabled */
	lapic_write(LAPIC_TPR, 0);                      /* accept all interrupts */

	/* Virtual-wire mode: the 8259 PIC (and so the PIT timer on IRQ0) reaches the CPU through LINT0 as
	 * ExtINT. Masking LINT0 silently kills the timer, which stops the screen repainting, the keyboard
	 * being polled and the scheduler preempting anything. LINT1 is NMI; error is masked. */
	lapic_write(LAPIC_LVT_LINT0, LVT_EXTINT);
	lapic_write(LAPIC_LVT_LINT1, LVT_NMI);
	lapic_write(LAPIC_LVT_ERROR, 0x10000);          /* mask error */

	kprintf("lapic: initialized (base=%p, ID=%d)\n", (void *)lapic_base, lapic_read(LAPIC_ID) >> 24);

	/* Parse MP table and discover CPUs */
	parse_mp_table();
}

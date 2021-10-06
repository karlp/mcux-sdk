/*
 * Copyright 2019 Broadcom
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 *
 * Copyright (c) 2021 BayLibre, SAS
 * Copyright (c) 2021 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "arm_mmu.h"

/* TODO: remoe dependency on lib_helpers.h */
#include "lib_helpers.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

#define __ASSERT(op, fmt, ...) \
  do { \
    if (!(op)) { \
      while(1) \
        /* wait here */; \
    } \
  } while (0)

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define isb()                  __ISB()
#define dsb()                  __DSB()
#define dmb()                  __DMB()

#define MPIDR_AFFLVL(mpidr, aff_level) \
      (((mpidr) >> MPIDR_AFF##aff_level##_SHIFT) & MPIDR_AFFLVL_MASK)

#define GET_MPIDR()               read_sysreg(mpidr_el1)

#if (defined(FSL_RTOS_FREE_RTOS) && defined(GUEST))
  #define MPIDR_TO_CORE(mpidr)    MPIDR_AFFLVL(mpidr, 3)
#else
  #define MPIDR_TO_CORE(mpidr)    MPIDR_AFFLVL(mpidr, 0)
#endif
#define IS_PRIMARY_CORE()         (!MPIDR_TO_CORE(GET_MPIDR()))

#define CONFIG_MMU_PAGE_SIZE   4096
#define CONFIG_MAX_XLAT_TABLES 16
#define CONFIG_ARM64_PA_BITS   48
#define CONFIG_ARM64_VA_BITS   48

//#define LOG_ERR(fmt, ...)      PRINTF(fmt, ##__VA_ARGS__);
#define LOG_ERR(fmt, ...)      (void)(fmt)
#define ARG_UNUSED(x)          (void)(x)

#define BITS_PER_LONG          (__CHAR_BIT__ * __SIZEOF_LONG__)
#define GENMASK(h, l) \
  (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define        ENOMEM          12      /* Out of memory */
#define        EBUSY           16      /* Device or resource busy */
#define        ENOTSUP         35

/* TODO: Figure out spinlock, if needed */
struct k_spinlock {
	char dummy;
};
#define k_spinlock_key_t       void *
#define k_spin_lock(p)         (p)
#define k_spin_unlock(p, key)  (void)key

#define k_panic()              assert(false)

/*******************************************************************************
 * from zephyr:/arch/arm/core/aarch64/mmu/arm_mmu.h:
 ******************************************************************************/

/* Set below flag to get debug prints */
//#define MMU_DEBUG_PRINTS	0

#if defined (MMU_DEBUG_PRINTS) && (MMU_DEBUG_PRINTS == 1)
/* To dump page table entries while filling them, set DUMP_PTE macro */
#define DUMP_PTE		0
#define MMU_DEBUG(fmt, ...)	PRINTF(fmt, ##__VA_ARGS__)
#else
#define MMU_DEBUG(...)
#endif

/*
 * 48-bit address with 4KB granule size:
 *
 * +------------+------------+------------+------------+-----------+
 * | VA [47:39] | VA [38:30] | VA [29:21] | VA [20:12] | VA [11:0] |
 * +---------------------------------------------------------------+
 * |     L0     |     L1     |     L2     |     L3     | block off |
 * +------------+------------+------------+------------+-----------+
 */

/* Only 4K granule is supported */
#define PAGE_SIZE_SHIFT		12U

/* 48-bit VA address */
#define VA_SIZE_SHIFT_MAX	48U

/* Maximum 4 XLAT table levels (L0 - L3) */
#define XLAT_LAST_LEVEL		3U

/* The VA shift of L3 depends on the granule size */
#define L3_XLAT_VA_SIZE_SHIFT	PAGE_SIZE_SHIFT

/* Number of VA bits to assign to each table (9 bits) */
#define Ln_XLAT_VA_SIZE_SHIFT	(PAGE_SIZE_SHIFT - 3)

/* Starting bit in the VA address for each level */
#define L2_XLAT_VA_SIZE_SHIFT	(L3_XLAT_VA_SIZE_SHIFT + Ln_XLAT_VA_SIZE_SHIFT)
#define L1_XLAT_VA_SIZE_SHIFT	(L2_XLAT_VA_SIZE_SHIFT + Ln_XLAT_VA_SIZE_SHIFT)
#define L0_XLAT_VA_SIZE_SHIFT	(L1_XLAT_VA_SIZE_SHIFT + Ln_XLAT_VA_SIZE_SHIFT)

#define LEVEL_TO_VA_SIZE_SHIFT(level)			\
	(PAGE_SIZE_SHIFT + (Ln_XLAT_VA_SIZE_SHIFT *	\
	(XLAT_LAST_LEVEL - (level))))

/* Number of entries for each table (512) */
#define Ln_XLAT_NUM_ENTRIES	((1U << PAGE_SIZE_SHIFT) / 8U)

/* Virtual Address Index within a given translation table level */
#define XLAT_TABLE_VA_IDX(va_addr, level) \
	((va_addr >> LEVEL_TO_VA_SIZE_SHIFT(level)) & (Ln_XLAT_NUM_ENTRIES - 1))

/*
 * Calculate the initial translation table level from CONFIG_ARM64_VA_BITS
 * For a 4 KB page size:
 *
 * (va_bits <= 20)	 - base level 3
 * (21 <= va_bits <= 29) - base level 2
 * (30 <= va_bits <= 38) - base level 1
 * (39 <= va_bits <= 47) - base level 0
 */
#define GET_BASE_XLAT_LEVEL(va_bits)				\
	 ((va_bits > L0_XLAT_VA_SIZE_SHIFT) ? 0U		\
	: (va_bits > L1_XLAT_VA_SIZE_SHIFT) ? 1U		\
	: (va_bits > L2_XLAT_VA_SIZE_SHIFT) ? 2U : 3U)

/* Level for the base XLAT */
#define BASE_XLAT_LEVEL	GET_BASE_XLAT_LEVEL(CONFIG_ARM64_VA_BITS)

#if (CONFIG_ARM64_PA_BITS == 48)
#define TCR_PS_BITS TCR_PS_BITS_256TB
#elif (CONFIG_ARM64_PA_BITS == 44)
#define TCR_PS_BITS TCR_PS_BITS_16TB
#elif (CONFIG_ARM64_PA_BITS == 42)
#define TCR_PS_BITS TCR_PS_BITS_4TB
#elif (CONFIG_ARM64_PA_BITS == 40)
#define TCR_PS_BITS TCR_PS_BITS_1TB
#elif (CONFIG_ARM64_PA_BITS == 36)
#define TCR_PS_BITS TCR_PS_BITS_64GB
#else
#define TCR_PS_BITS TCR_PS_BITS_4GB
#endif

/* Upper and lower attributes mask for page/block descriptor */
#define DESC_ATTRS_UPPER_MASK	GENMASK(63, 51)
#define DESC_ATTRS_LOWER_MASK	GENMASK(11, 2)

#define DESC_ATTRS_MASK		(DESC_ATTRS_UPPER_MASK | DESC_ATTRS_LOWER_MASK)

/******************************************************************************/

static uint64_t xlat_tables[CONFIG_MAX_XLAT_TABLES * Ln_XLAT_NUM_ENTRIES]
		__aligned(Ln_XLAT_NUM_ENTRIES * sizeof(uint64_t));
static uint16_t xlat_use_count[CONFIG_MAX_XLAT_TABLES];
static struct k_spinlock xlat_lock;

/* Returns a reference to a free table */
static uint64_t *new_table(void)
{
	unsigned int i;

	/* Look for a free table. */
	for (i = 0; i < CONFIG_MAX_XLAT_TABLES; i++) {
		if (xlat_use_count[i] == 0) {
			xlat_use_count[i] = 1;
			return &xlat_tables[i * Ln_XLAT_NUM_ENTRIES];
		}
	}

	LOG_ERR("CONFIG_MAX_XLAT_TABLES, too small");
	return NULL;
}

static inline unsigned int table_index(uint64_t *pte)
{
	unsigned int i = (pte - xlat_tables) / Ln_XLAT_NUM_ENTRIES;

	__ASSERT(i < CONFIG_MAX_XLAT_TABLES, "table %p out of range", pte);
	return i;
}

/* Makes a table free for reuse. */
static void free_table(uint64_t *table)
{
	unsigned int i = table_index(table);

	MMU_DEBUG("freeing table [%d]%p\r\n", i, table);
	__ASSERT(xlat_use_count[i] == 1, "table still in use");
	xlat_use_count[i] = 0;
}

/* Adjusts usage count and returns current count. */
static int table_usage(uint64_t *table, int adjustment)
{
	unsigned int i = table_index(table);

	xlat_use_count[i] += adjustment;
	__ASSERT(xlat_use_count[i] > 0, "usage count underflow");
	return xlat_use_count[i];
}

static inline bool is_table_unused(uint64_t *table)
{
	return table_usage(table, 0) == 1;
}

static inline bool is_free_desc(uint64_t desc)
{
	return (desc & PTE_DESC_TYPE_MASK) == PTE_INVALID_DESC;
}

static inline bool is_table_desc(uint64_t desc, unsigned int level)
{
	return level != XLAT_LAST_LEVEL &&
	       (desc & PTE_DESC_TYPE_MASK) == PTE_TABLE_DESC;
}

static inline bool is_block_desc(uint64_t desc)
{
	return (desc & PTE_DESC_TYPE_MASK) == PTE_BLOCK_DESC;
}

static inline uint64_t *pte_desc_table(uint64_t desc)
{
	uint64_t address = desc & GENMASK(47, PAGE_SIZE_SHIFT);

	return (uint64_t *)address;
}

static inline bool is_desc_superset(uint64_t desc1, uint64_t desc2,
				    unsigned int level)
{
	uint64_t mask = DESC_ATTRS_MASK | GENMASK(47, LEVEL_TO_VA_SIZE_SHIFT(level));

	return (desc1 & mask) == (desc2 & mask);
}

#if DUMP_PTE
static void debug_show_pte(uint64_t *pte, unsigned int level)
{
	MMU_DEBUG("%.*s", level * 2, ". . . ");
	MMU_DEBUG("[%d]%p: ", table_index(pte), pte);

	if (is_free_desc(*pte)) {
		MMU_DEBUG("---\r\n");
		return;
	}

	if (is_table_desc(*pte, level)) {
		uint64_t *table = pte_desc_table(*pte);

		MMU_DEBUG("[Table] [%d]%p\r\n", table_index(table), table);
		return;
	}

	if (is_block_desc(*pte)) {
		MMU_DEBUG("[Block] ");
	} else {
		MMU_DEBUG("[Page] ");
	}

	uint8_t mem_type = (*pte >> 2) & MT_TYPE_MASK;

	MMU_DEBUG((mem_type == MT_NORMAL) ? "MEM" :
		  ((mem_type == MT_NORMAL_NC) ? "NC" : "DEV"));
	MMU_DEBUG((*pte & PTE_BLOCK_DESC_AP_RO) ? "-RO" : "-RW");
	MMU_DEBUG((*pte & PTE_BLOCK_DESC_NS) ? "-NS" : "-S");
	MMU_DEBUG((*pte & PTE_BLOCK_DESC_AP_ELx) ? "-ELx" : "-ELh");
	MMU_DEBUG((*pte & PTE_BLOCK_DESC_PXN) ? "-PXN" : "-PX");
	MMU_DEBUG((*pte & PTE_BLOCK_DESC_UXN) ? "-UXN" : "-UX");
	MMU_DEBUG("\r\n");
}
#else
static inline void debug_show_pte(uint64_t *pte, unsigned int level) { }
#endif

static void set_pte_table_desc(uint64_t *pte, uint64_t *table, unsigned int level)
{
	/* Point pte to new table */
	*pte = PTE_TABLE_DESC | (uint64_t)table;
	debug_show_pte(pte, level);
}

static void set_pte_block_desc(uint64_t *pte, uint64_t desc, unsigned int level)
{
	if (desc) {
		desc |= (level == XLAT_LAST_LEVEL) ? PTE_PAGE_DESC : PTE_BLOCK_DESC;
	}
	*pte = desc;
	debug_show_pte(pte, level);
}

static uint64_t *expand_to_table(uint64_t *pte, unsigned int level)
{
	uint64_t *table;

	__ASSERT(level < XLAT_LAST_LEVEL, "can't expand last level");

	table = new_table();
	if (!table) {
		return NULL;
	}

	if (!is_free_desc(*pte)) {
		/*
		 * If entry at current level was already populated
		 * then we need to reflect that in the new table.
		 */
		uint64_t desc = *pte;
		unsigned int i, stride_shift;

		MMU_DEBUG("expanding PTE 0x%016llx into table [%d]%p\r\n",
			  desc, table_index(table), table);
		__ASSERT(is_block_desc(desc), "");

		if (level + 1 == XLAT_LAST_LEVEL) {
			desc |= PTE_PAGE_DESC;
		}

		stride_shift = LEVEL_TO_VA_SIZE_SHIFT(level + 1);
		for (i = 0; i < Ln_XLAT_NUM_ENTRIES; i++) {
			table[i] = desc | (i << stride_shift);
		}
		table_usage(table, Ln_XLAT_NUM_ENTRIES);
	} else {
		/*
		 * Adjust usage count for parent table's entry
		 * that will no longer be free.
		 */
		table_usage(pte, 1);
	}

	/* Link the new table in place of the pte it replaces */
	set_pte_table_desc(pte, table, level);
	table_usage(table, 1);

	return table;
}

static int set_mapping(struct ARM_MMU_ptables *ptables,
		       uintptr_t virt, size_t size,
		       uint64_t desc, bool may_overwrite)
{
	uint64_t *pte, *ptes[XLAT_LAST_LEVEL + 1];
	uint64_t level_size;
	uint64_t *table = ptables->base_xlat_table;
	unsigned int level = BASE_XLAT_LEVEL;
	k_spinlock_key_t key;
	int ret = 0;

	key = k_spin_lock(&xlat_lock);

	while (size) {
		__ASSERT(level <= XLAT_LAST_LEVEL,
			 "max translation table level exceeded\r\n");

		/* Locate PTE for given virtual address and page table level */
		pte = &table[XLAT_TABLE_VA_IDX(virt, level)];
		ptes[level] = pte;

		if (is_table_desc(*pte, level)) {
			/* Move to the next translation table level */
			level++;
			table = pte_desc_table(*pte);
			continue;
		}

		if (!may_overwrite && !is_free_desc(*pte)) {
			/* the entry is already allocated */
			LOG_ERR("entry already in use: "
				"level %d pte %p *pte 0x%016llx",
				level, pte, *pte);
			ret = -EBUSY;
			break;
		}

		level_size = 1ULL << LEVEL_TO_VA_SIZE_SHIFT(level);

		if (is_desc_superset(*pte, desc, level)) {
			/* This block already covers our range */
			level_size -= (virt & (level_size - 1));
			if (level_size > size) {
				level_size = size;
			}
			goto move_on;
		}

		if ((size < level_size) || (virt & (level_size - 1))) {
			/* Range doesn't fit, create subtable */
			table = expand_to_table(pte, level);
			if (!table) {
				ret = -ENOMEM;
				break;
			}
			level++;
			continue;
		}

		/* Adjust usage count for corresponding table */
		if (is_free_desc(*pte)) {
			table_usage(pte, 1);
		}
		if (!desc) {
			table_usage(pte, -1);
		}
		/* Create (or erase) block/page descriptor */
		set_pte_block_desc(pte, desc, level);

		/* recursively free unused tables if any */
		while (level != BASE_XLAT_LEVEL &&
		       is_table_unused(pte)) {
			free_table(pte);
			pte = ptes[--level];
			set_pte_block_desc(pte, 0, level);
			table_usage(pte, -1);
		}

move_on:
		virt += level_size;
		desc += desc ? level_size : 0;
		size -= level_size;

		/* Range is mapped, start again for next range */
		table = ptables->base_xlat_table;
		level = BASE_XLAT_LEVEL;
	}

	k_spin_unlock(&xlat_lock, key);

	return ret;
}

static uint64_t get_region_desc(uint32_t attrs)
{
	unsigned int mem_type;
	uint64_t desc = 0;

	/* NS bit for security memory access from secure state */
	desc |= (attrs & MT_NS) ? PTE_BLOCK_DESC_NS : 0;

	/*
	 * AP bits for EL0 / ELh Data access permission
	 *
	 *   AP[2:1]   ELh  EL0
	 * +--------------------+
	 *     00      RW   NA
	 *     01      RW   RW
	 *     10      RO   NA
	 *     11      RO   RO
	 */

	/* AP bits for Data access permission */
	desc |= (attrs & MT_RW) ? PTE_BLOCK_DESC_AP_RW : PTE_BLOCK_DESC_AP_RO;

	/* Mirror permissions to EL0 */
	desc |= (attrs & MT_RW_AP_ELx) ?
		 PTE_BLOCK_DESC_AP_ELx : PTE_BLOCK_DESC_AP_EL_HIGHER;

	/* the access flag */
	desc |= PTE_BLOCK_DESC_AF;

	/* memory attribute index field */
	mem_type = MT_TYPE(attrs);
	desc |= PTE_BLOCK_DESC_MEMTYPE(mem_type);

	switch (mem_type) {
	case MT_DEVICE_nGnRnE:
	case MT_DEVICE_nGnRE:
	case MT_DEVICE_GRE:
		/* Access to Device memory and non-cacheable memory are coherent
		 * for all observers in the system and are treated as
		 * Outer shareable, so, for these 2 types of memory,
		 * it is not strictly needed to set shareability field
		 */
		desc |= PTE_BLOCK_DESC_OUTER_SHARE;
		/* Map device memory as execute-never */
		desc |= PTE_BLOCK_DESC_PXN;
		desc |= PTE_BLOCK_DESC_UXN;
		break;
	case MT_NORMAL_NC:
	case MT_NORMAL:
		/* Make Normal RW memory as execute never */
		if ((attrs & MT_RW) || (attrs & MT_P_EXECUTE_NEVER))
			desc |= PTE_BLOCK_DESC_PXN;

		if (((attrs & MT_RW) && (attrs & MT_RW_AP_ELx)) ||
		     (attrs & MT_U_EXECUTE_NEVER))
			desc |= PTE_BLOCK_DESC_UXN;

		if (mem_type == MT_NORMAL)
			desc |= PTE_BLOCK_DESC_INNER_SHARE;
		else
			desc |= PTE_BLOCK_DESC_OUTER_SHARE;
	}

	return desc;
}

static int add_map(struct ARM_MMU_ptables *ptables, const char *name,
		   uintptr_t phys, uintptr_t virt, size_t size, uint32_t attrs)
{
	uint64_t desc = get_region_desc(attrs);
	bool may_overwrite = !(attrs & MT_NO_OVERWRITE);

	MMU_DEBUG("mmap [%s]: virt %lx phys %lx size %lx attr %llx\r\n",
		  name, virt, phys, size, desc);
	__ASSERT(((virt | phys | size) & (CONFIG_MMU_PAGE_SIZE - 1)) == 0,
		 "address/size are not page aligned\r\n");
	desc |= phys;
	return set_mapping(ptables, virt, size, desc, may_overwrite);
}

static void invalidate_tlb_all(void)
{
	__asm__ volatile (
	"tlbi vmalle1; dsb sy; isb"
	: : : "memory");
}

/* OS execution regions with appropriate attributes */

static inline void add_ARM_MMU_flat_range(struct ARM_MMU_ptables *ptables,
					  const struct ARM_MMU_flat_range *range,
					  uint32_t extra_flags)
{
	uintptr_t address = (uintptr_t)range->start;
	size_t size = (uintptr_t)range->end - address;

	if (size) {
		add_map(ptables, range->name, address, address,
			size, range->attrs | extra_flags);
	}
}

static inline void add_ARM_MMU_region(struct ARM_MMU_ptables *ptables,
				      const struct ARM_MMU_region *region,
				      uint32_t extra_flags)
{
	if (region->size || region->attrs) {
		add_map(ptables, region->name, region->base_pa, region->base_va,
			region->size, region->attrs | extra_flags);
	}
}

static void setup_page_tables(struct ARM_MMU_ptables *ptables)
{
	unsigned int index;
	const struct ARM_MMU_flat_range *range;
	const struct ARM_MMU_region *region;
	uintptr_t max_va = 0, max_pa = 0;

	MMU_DEBUG("xlat tables:\r\n");
	for (index = 0; index < CONFIG_MAX_XLAT_TABLES; index++)
		MMU_DEBUG("%d: %p\r\n", index, xlat_tables + index * Ln_XLAT_NUM_ENTRIES);

	for (index = 0; index < MMU_config.num_regions; index++) {
		region = &MMU_config.mmu_regions[index];
		max_va = MAX(max_va, region->base_va + region->size);
		max_pa = MAX(max_pa, region->base_pa + region->size);
	}

	__ASSERT(max_va <= (1ULL << CONFIG_ARM64_VA_BITS),
		 "Maximum VA not supported\r\n");
	__ASSERT(max_pa <= (1ULL << CONFIG_ARM64_PA_BITS),
		 "Maximum PA not supported\r\n");

	/* setup translation table for OS execution regions */
	for (index = 0; index < MMU_config.num_regions; index++) {
		range = &MMU_config.mmu_os_ranges[index];
		add_ARM_MMU_flat_range(ptables, range, 0);
	}

	/*
	 * Create translation tables for user provided platform regions.
	 * Those must not conflict with our default mapping.
	 */
	for (index = 0; index < MMU_config.num_regions; index++) {
		region = &MMU_config.mmu_regions[index];
		add_ARM_MMU_region(ptables, region, MT_NO_OVERWRITE);
	}

	invalidate_tlb_all();
}

/* Translation table control register settings */
static uint64_t get_tcr(int el)
{
	uint64_t tcr;
	uint64_t va_bits = CONFIG_ARM64_VA_BITS;
	uint64_t tcr_ps_bits;

	tcr_ps_bits = TCR_PS_BITS;

	if (el == 1) {
		tcr = (tcr_ps_bits << TCR_EL1_IPS_SHIFT);
		/*
		 * TCR_EL1.EPD1: Disable translation table walk for addresses
		 * that are translated using TTBR1_EL1.
		 */
		tcr |= TCR_EPD1_DISABLE;
	} else
		tcr = (tcr_ps_bits << TCR_EL3_PS_SHIFT);

	tcr |= TCR_T0SZ(va_bits);
	/*
	 * Translation table walk is cacheable, inner/outer WBWA and
	 * inner shareable
	 */
	tcr |= TCR_TG0_4K | TCR_SHARED_INNER | TCR_ORGN_WBWA | TCR_IRGN_WBWA;

	return tcr;
}

static void enable_mmu_el1(struct ARM_MMU_ptables *ptables, unsigned int flags)
{
	ARG_UNUSED(flags);
	uint64_t val;

	/* Set MAIR, TCR and TBBR registers */
	write_mair_el1(MEMORY_ATTRIBUTES);
	write_tcr_el1(get_tcr(1));
	write_ttbr0_el1((uint64_t)ptables->base_xlat_table);

	/* Ensure these changes are seen before MMU is enabled */
	isb();

	/* Enable the MMU and caches */
	val = read_sctlr_el1();
	write_sctlr_el1(val | SCTLR_M_BIT | SCTLR_C_BIT | SCTLR_I_BIT);

	/* Ensure the MMU enable takes effect immediately */
	isb();

	MMU_DEBUG("MMU enabled with caches\r\n");
}

/* ARM MMU Driver Initial Setup */

static struct ARM_MMU_ptables kernel_ptables;
#ifdef CONFIG_USERSPACE
static sys_slist_t domain_list;
#endif

/*
 * @brief MMU default configuration
 *
 * This function provides the default configuration mechanism for the Memory
 * Management Unit (MMU).
 */
/* was: void z_arm64_mmu_init() */
void ARM_MMU_Initialize(void)
{
	unsigned int flags = 0;

	__ASSERT(CONFIG_MMU_PAGE_SIZE == KB(4),
		 "Only 4K page size is supported\r\n");

	__ASSERT(GET_EL(read_currentel()) == MODE_EL1,
		 "Exception level not EL1, MMU not enabled!\r\n");

	/* Ensure that MMU is already not enabled */
	__ASSERT((read_sctlr_el1() & SCTLR_M_BIT) == 0, "MMU is already enabled\r\n");

	/*
	 * Only booting core setup up the page tables.
	 */
	if (IS_PRIMARY_CORE()) {
		kernel_ptables.base_xlat_table = new_table();
		setup_page_tables(&kernel_ptables);
	}

	/* currently only EL1 is supported */
	enable_mmu_el1(&kernel_ptables, flags);
}
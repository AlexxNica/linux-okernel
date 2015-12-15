/*
 * vmx.c - The Intel VT-x driver for intra-kernel protection
 * using vt-x features. This file is derived from 
 * the dune code base which itself is dervied from the kvm
 * code base (with the hope that we can possibly at some point 
 * share code).
 *
 * This is still very much a (limited) research prototype.
 *
 * Author: C I Dalton <cid@hpe.com> 2015
 *
 * This is the original dune header:

 * This file is derived from Linux KVM VT-x support.
 * Copyright (C) 2006 Qumranet, Inc.
 * Copyright 2010 Red Hat, Inc. and/or its affiliates.
 *
 * Original Authors:
 *   Avi Kivity   <avi@qumranet.com>
 *   Yaniv Kamay  <yaniv@qumranet.com>
 *
 * This modified version is simpler because it avoids the following
 * features that are not requirements for Dune:
 *  * Real-mode emulation
 *  * Nested VT-x support
 *  * I/O hardware emulation
 *  * Any of the more esoteric X86 features and registers
 *  * KVM-specific functionality
 *
 * In essence we provide only the minimum functionality needed to run
 * a process in vmx non-root mode rather than the full hardware emulation
 * needed to support an entire OS.
 *
 * This driver is a research prototype and as such has the following
 * limitations:
 *
 * FIXME: Backward compatability is currently a non-goal, and only recent
 * full-featured (EPT, PCID, VPID, etc.) Intel hardware is supported by this
 * driver.
 *
 * FIXME: Eventually we should handle concurrent user's of VT-x more
 * gracefully instead of requiring exclusive access. This would allow
 * Dune to interoperate with KVM and other HV solutions.
 *
 * FIXME: We need to support hotplugged physical CPUs.
 *
 * Authors:
 *   Adam Belay   <abelay@stanford.edu>
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/ftrace.h>
#include <linux/slab.h>
#include <linux/tboot.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/syscalls.h>
#include <linux/version.h>

#include <asm/desc.h>
#include <asm/vmx.h>
#include <asm/unistd_64.h>
#include <asm/virtext.h>
#include <asm/percpu.h>
//#include <asm/paravirt.h>
#include <asm/preempt.h>
#include <asm/tlbflush.h>

#include "constants2.h"
#include "vmx.h"


static atomic_t vmx_enable_failed;

static DECLARE_BITMAP(vmx_vpid_bitmap, VMX_NR_VPIDS);
static DEFINE_SPINLOCK(vmx_vpid_lock);

static unsigned long *msr_bitmap;




static DEFINE_PER_CPU(struct vmcs *, vmxarea);
static DEFINE_PER_CPU(struct desc_ptr, host_gdt);
static DEFINE_PER_CPU(int, vmx_enabled);
DEFINE_PER_CPU(struct vmx_vcpu *, local_vcpu);

static struct vmcs_config {
	int size;
	int order;
	u32 revision_id;
	u32 pin_based_exec_ctrl;
	u32 cpu_based_exec_ctrl;
	u32 cpu_based_2nd_exec_ctrl;
	u32 vmexit_ctrl;
	u32 vmentry_ctrl;
} vmcs_config;

struct vmx_capability vmx_capability;


static inline int dummy_in_vmx_nr_mode(void)
{
	return 0;
}


static inline int real_in_vmx_nr_mode(void)
{
	unsigned long cr4;

	cr4 = native_read_cr4();
	
	if(cr4 & X86_CR4_VMXE)
		return 0;
	return 1;
}


static int (*in_vmx_nr_mode)(void) = dummy_in_vmx_nr_mode;	

inline int is_in_vmx_nr_mode(void)
{
	return in_vmx_nr_mode();
}




/* Adapted from https://github.com/rustyrussell/virtbench/blob/master/micro/vmcall.c */
int vmcall(unsigned int cmd)
{
	/* cid: need to guard the use of this call */
	asm volatile(".byte 0x0F,0x01,0xC1\n" ::"a"(cmd));
	return 0;
}


/* Start: imported code from original BV prototype */

/* Need to fix this...adjust dynamically based on real physical regions */
#define END_PHYSICAL 0x3FFEFFFFF /* with 1GB physical memory */

static int
no_cache_region(u64 addr, u64 size)
{
	if (((addr >= 0) && (addr < 0x9F00))||((addr >= BIOS_END) && (addr < END_PHYSICAL))){
		return 0;
	}
	return 1;
}
/* Adapted from e820_end_pfn */
static unsigned long e820_end_paddr(unsigned long limit_pfn)
{
	int i;
	unsigned long last_pfn = 0;
	unsigned long max_arch_pfn = (MAXMEM >> PAGE_SHIFT);

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];
		unsigned long start_pfn;
		unsigned long end_pfn;

		start_pfn = ei->addr >> PAGE_SHIFT;
		end_pfn = (ei->addr + ei->size) >> PAGE_SHIFT;

		if (start_pfn >= limit_pfn)
			continue;
		if (end_pfn > limit_pfn) {
			last_pfn = limit_pfn;
			break;
		}
		if (end_pfn > last_pfn)
			last_pfn = end_pfn;
	}

	if (last_pfn > max_arch_pfn)
		last_pfn = max_arch_pfn;

	HDEBUG(("last_pfn = %#lx max_arch_pfn = %#lx\n", last_pfn, max_arch_pfn));
	return last_pfn << PAGE_SHIFT;
}

int vt_alloc_page(void **virt, u64 *phys)
{
        struct page *pg;
        void* v;

        pg = alloc_page(GFP_KERNEL);

        v = page_address(pg);

        if(!v){
                printk(KERN_ERR "okernel: failed to alloc page.\n");
                return 0;
        }

        if(virt)
                *virt = v;
        if(phys)
                *phys = page_to_phys(pg);
        return 1;
}

int vt_alloc_pages(struct pt_page *pt, int order)
{
        struct page *pg;
        void* v;
	int i;

	if(!pt){
		printk(KERN_ERR "Null pt passed.\n");
		return 0;
	}
	
	pg = alloc_pages(GFP_KERNEL, order);

	if(!pg){
		printk(KERN_ERR "okernel: failed to alloc pages.\n");
		return 0;
	}
	
        v = page_address(pg);

        if(!v){
                printk(KERN_ERR "okernel: failed to get page vaddr.\n");
                return 0;
        }


	for(i = 0; i <  (1 << order); i++){
		pt[i].virt = v+i*PAGESIZE;
		pt[i].phys = page_to_phys(pg+i);
	}
	return 1;
}

int vt_ept_unmap_pages(u64 vaddr, unsigned long num_pages)
{
	return 0;
}

int vt_ept_replace_pages(u64 vaddr, unsigned long num_pages)
{
	return 0;
}



/* Essentially create 1:1 map of 'host' physical mem to 'guest' physical */
u64 vt_ept_4K_init(void)
{
	return 0;
}

unsigned long* find_pd_entry(struct vmx_vcpu *vcpu, u64 paddr)
{
	/* Find index in a PD that maps a particular 2MB range containing a given address. */

	/* First need the PDP enty from a given PML4 root */
	epte_t *pml3 = NULL;
	epte_t *pml2 = NULL;
	epte_t *pde  = NULL;
	unsigned long* pml2_p;
	
	int pml3_index, pml2_index;

	epte_t *pml4 =  (epte_t*) __va(vcpu->ept_root);

	pml3 = (epte_t *)epte_page_vaddr(*pml4);

	pml3_index = (paddr & (~(GIGABYTE -1))) >> GIGABYTE_SHIFT;

	HDEBUG(("addr (%#lx) pml3 index (%i)\n", (unsigned long)paddr, pml3_index));
	
	pml2 = (epte_t *)epte_page_vaddr(pml3[pml3_index]);

	pml2_index = ((paddr & (GIGABYTE -1)) >> PAGESIZE2M_SHIFT);

	HDEBUG(("addr (%#lx) pml2 index (%i)\n", (unsigned long)paddr, pml2_index));

	pde = (epte_t *)epte_page_vaddr(pml2[pml2_index]);

	pml2_p = &pml2[pml2_index];
	HDEBUG(("addr (%#lx) pde (%#lx)\n", (unsigned long)paddr, (unsigned long)pde));
	return pml2_p;
}

unsigned long* find_pt_entry(struct vmx_vcpu *vcpu, u64 paddr)
{
	/* Find index in a PD that maps a particular 2MB range containing a given address. */

	/* First need the PDP enty from a given PML4 root */
	epte_t *pml3 = NULL;
	epte_t *pml2 = NULL;
	epte_t *pml1 = NULL;

	unsigned long* pml1_p;
	
	int pml3_index, pml2_index, pml1_index;

	epte_t *pml4 =  (epte_t*) __va(vcpu->ept_root);

	pml3 = (epte_t *)epte_page_vaddr(*pml4);

	pml3_index = (paddr & (~(GIGABYTE -1))) >> GIGABYTE_SHIFT;

	HDEBUG(("addr (%#lx) pml3 index (%i)\n", (unsigned long)paddr, pml3_index));
	
	pml2 = (epte_t *)epte_page_vaddr(pml3[pml3_index]);


	pml2_index = ((paddr & (GIGABYTE -1)) >> PAGESIZE2M_SHIFT);

	HDEBUG(("addr (%#lx) pml2 index (%i)\n", (unsigned long)paddr, pml2_index));

	pml1 = (epte_t *)epte_page_vaddr(pml2[pml2_index]);

	HDEBUG(("check for 4k page mapping.\n"));
	BUG_ON(*pml1 & EPT_2M_PAGE);

	pml1_index = ((paddr & (PAGESIZE2M-1)) >> PAGESIZE_SHIFT);

	pml1_p = &pml1[pml1_index];
	
	HDEBUG(("addr (%#lx) pte (%#lx)\n", (unsigned long)paddr, (unsigned long)pml1));
	return pml1_p;
}


	
int split_2M_mapping(struct vmx_vcpu* vcpu, u64 paddr)
{
	unsigned long *pml2_e;
	pt_page *pt = NULL;
	unsigned int n_entries = PAGESIZE / 8;
	u64* q = NULL;
	int i = 0;
	u64 p_base_addr;
	u64 addr;
	
	if((paddr & (PAGESIZE2M -1)) != 0){
		printk(KERN_ERR "okernel: 2MB unaligned addr passed to is_2M_mapping.\n");
		return 0;
	}
	
	if(!(pml2_e =  find_pd_entry(vcpu, paddr))){
		printk(KERN_ERR "okernel: NULL pml2 entry for paddr (%#lx)\n",
		       (unsigned long)paddr);
		return 0;
	}

	/* check if 2M mapping or slpit already */
	if(!(*pml2_e & EPT_2M_PAGE)){
		HDEBUG(("paddr ept entry for 2MB region starting at phys addr (%#lx) already split.\n",
			(unsigned long)paddr));
		return 1;
	}

	/* 2M region base address */

	p_base_addr = (*pml2_e & ~(PAGESIZE2M-1));

	HDEBUG(("base EPT physical addr for table 2M split (%#lx) paddr (%#lx)\n",
		(unsigned long)p_base_addr, (unsigned long)paddr));
	
	/* split the plm2_e into 4k ptes,i.e. have it point to a PML1 table */

        /* First allocate a physical page for the PML1 table (512*4K entries) */ 
	if(!(pt   = (pt_page*)kmalloc(sizeof(pt_page), GFP_KERNEL))){
		printk(KERN_ERR "okernel: failed to allocate PT table.\n");
		return 0;
	}
	
	if(!(vt_alloc_page((void**)&pt[0].virt, &pt[0].phys))){
		printk(KERN_ERR "okernel: failed to allocate PML1 table.\n");
		return 0;
	}

	memset(pt[0].virt, 0, PAGESIZE);
	HDEBUG(("PML1 pt virt (%llX) pt phys (%llX)\n", (unsigned long long)pt[0].virt, pt[0].phys));

	/* Fill in eack of the 4k ptes for the PML1 */ 
	q = pt[0].virt;

	for(i = 0; i < n_entries; i++){
		addr = p_base_addr + i*PAGESIZE;
		if(no_cache_region(addr, PAGESIZE)){
			q[i] = addr | EPT_R | EPT_W | EPT_X;
		} else {
			q[i] = addr | EPT_R | EPT_W | EPT_X | EPT_CACHE_2 | EPT_CACHE_3;
		}
	}

	*pml2_e = pt[0].phys + EPT_R + EPT_W + EPT_X; 
	return 1;
}


/* Returns virtual mapping of the new page */
void* replace_ept_page(struct vmx_vcpu *vcpu, u64 paddr)
{
	unsigned long *pml1_p;
	pt_page *pt;
	u64 orig_paddr;
	u64 split_addr;

	split_addr = (paddr & ~(PAGESIZE2M-1));
	
	HDEBUG(("Check or split 2M mapping at (%#lx)\n", (unsigned long)split_addr));
	
	if(!(split_2M_mapping(vcpu, split_addr))){
		printk(KERN_ERR "okernel: couldn't split 2MB mapping for (%#lx)\n",
		       (unsigned long)paddr);
		return NULL;
	}

	HDEBUG(("Split or check ok: looking for pte for paddr (%#lx)\n",
		(unsigned long)paddr));
	
	if(!(pml1_p = find_pt_entry(vcpu, paddr))){
		printk(KERN_ERR "okernel: failed to find pte for (%#lx)\n",
		       (unsigned long)paddr);
		return NULL;
	}

	HDEBUG(("pte val for paddr (%#lx) is (%#lx)\n",
		(unsigned long)paddr, (unsigned long)*pml1_p)); 
	
	pt   = (pt_page*)kmalloc(sizeof(pt_page), GFP_KERNEL);

	if(!pt){
		printk(KERN_ERR "okernel: failed to allocate PT table in replace ept page.\n");
		return NULL;
	}
	
	if(!(vt_alloc_page((void**)&pt[0].virt, &pt[0].phys))){
		printk(KERN_ERR "okernel: failed to allocate PML1 table.\n");
		return NULL;
	}

	memset(pt[0].virt, 0, PAGESIZE);

	HDEBUG(("Replacement page pt virt (%llX) pt phys (%llX)\n", (unsigned long long)pt[0].virt, pt[0].phys));

	orig_paddr = (*pml1_p & ~(PAGESIZE-1));

	HDEBUG(("orig paddr (%#lx)\n", (unsigned long)orig_paddr));
	
	if(orig_paddr != paddr){
		printk(KERN_ERR "address mis-match in EPT tables.\n");
		return NULL;
	}
	
	HDEBUG(("Replacing (%#lx) as pte entry with (%#lx)\n",
		(unsigned long)(*pml1_p), (unsigned long)(pt[0].phys | EPT_R | EPT_W | EPT_X | EPT_CACHE_2 | EPT_CACHE_3)));

	*pml1_p = pt[0].phys | EPT_R | EPT_W | EPT_X | EPT_CACHE_2 | EPT_CACHE_3;

	HDEBUG(("copying data from va (%#lx) to va of replacement physical (%#lx)\n",
		(unsigned long)__va(orig_paddr), (unsigned long)pt[0].virt));
	
	memcpy(pt[0].virt, __va(orig_paddr), PAGESIZE);
	HDEBUG(("Done for pa (%#lx)\n", (unsigned long)paddr));
	return pt[0].virt;
}


int clone_kstack2(struct vmx_vcpu *vcpu)
{
	int n_pages;
	unsigned long k_stack;
	int i;
	u64 paddr;
	void* vaddr;
	
	n_pages = THREAD_SIZE / PAGESIZE;

	BUG_ON(n_pages != 4);

	k_stack  = (unsigned long)current->stack;
		
	HDEBUG(("kernel thread_info (tsk->stack) vaddr (%#lx) paddr (%#lx) top of stack (%#lx)\n",
		k_stack, __pa(k_stack), current_top_of_stack()));

	for(i = 0; i < n_pages; i++){
		paddr = __pa(k_stack + i*PAGESIZE);
		HDEBUG(("ept page clone on (%#lx)\n", (unsigned long)paddr));
		/* also need replace_ept_contiguous_region */
		if(!(vaddr = replace_ept_page(vcpu, paddr))){
			printk(KERN_ERR "failed to clone page at (%#lx)\n",
			       (unsigned long)paddr);
			return 0;
		}
                /* FIX: we assume for now that the thread_info
		 * structure is at the bottom of the first page */
		if(i == 0){
			vcpu->cloned_thread_info = (struct thread_info*)vaddr;
		}
	}
	return 1;
}

u64 vt_ept_2M_init(void)
{
	/*
	 * For now share a direct 1:1 EPT mapping of host physical to
	 * guest physical across all vmx 'containers'. 
	 *
	 * Setup the per-vcpu pagetables here. For now we just map up to
	 * 512G of physical RAM, and we use a 2MB page size. So we need
	 * one PML4 physical page, one PDPT physical page and 1 PD
	 * physical page per GB.  We need correspondingly, 1 PML4 entry
	 * (PML4E), 1 PDPT entrie per GB (PDPTE), and 512 PD entries
	 * (PDE) per PD.
	 *
	 * The first 2Mb region we break down into 4K page table entries
	 * so we can be more selectively over caching controls, etc. for
	 * that region.
	 */
	unsigned long mappingsize = 0;      
	unsigned long rounded_mappingsize = 0;
	unsigned int n_entries = PAGESIZE / 8; 
	unsigned int n_pt   = 0;
	unsigned int n_pd   = 0;
	unsigned int n_pdpt = 0;
	pt_page* pt = NULL;
	pt_page* pd = NULL;
	pt_page* pdpt = NULL;
	int i = 0, k = 0;
	u64* q = NULL;
	u64 addr = 0;
	u64* pml4_virt = NULL;
	u64  pml4_phys = 0;
	
	/* What range do the EPT tables need to cover (including areas like the APIC mapping)? */
	mappingsize = e820_end_paddr(MAXMEM);

	HDEBUG(("max physical address to map under EPT: %#lx\n", (unsigned long)mappingsize));
	
	/* Round upp to closest Gigabyte of memory */
	rounded_mappingsize = ((mappingsize + (GIGABYTE-1)) & (~(GIGABYTE-1)));      

	HDEBUG(("Need EPT tables covering (%lu) Mb (%lu) bytes for Phys Mapping sz: %lu MB\n", 
		rounded_mappingsize >> 20, rounded_mappingsize, mappingsize >> 20));

	if((rounded_mappingsize >> GIGABYTE_SHIFT) >  PML4E_MAP_LIMIT){
		/* Only setup one PDPTE entry for now so can map up to 512Gb */
		printk(KERN_ERR "Physical memory greater than (%d) Gb not supported.\n",
		       PML4E_MAP_LIMIT);
		return 0;
	}

	/* Only need 1 pdpt to map upto 512G */
	n_pdpt = 1;
	/* Need 1 PD per gigabyte of physical mem */
	n_pd = rounded_mappingsize >> GIGABYTE_SHIFT;
	/* We just split the 1st 2Mb region into 4K pages so need only 1 PT table. */
	n_pt = 1;
	
	/* pt - PML1, pd - PML2, pdpt - PML3 */
	pdpt = (pt_page*)kmalloc(sizeof(pt_page)* n_pdpt, GFP_KERNEL);
	pd   = (pt_page*)kmalloc(sizeof(pt_page)* n_pd, GFP_KERNEL);
	pt   = (pt_page*)kmalloc(sizeof(pt_page)* n_pt, GFP_KERNEL);
	
	HDEBUG(("Allocated (%u) pdpt (%u) pd (%u) pt tables.\n", n_pdpt, n_pd, n_pt));

        /* Allocate the paging structures from bottom to top so we start
	 * at the PT level (PML1) and finish with the PML4 table.
	 */
	
	/* 1st 2Meg mapping (PML1 / PT):
	 * At the moment we only use a PT for the 1st 2MB region, for
	 * the rest of memory we map in via 2MB PD entries. We break
	 * first 2M region into 4k pages so that we can use the CPU
	 * cache in real-mode otherwise we end up with UC memory for the
	 * whole 2M.
	 */

	BUG_ON(n_pt != 1);
	
	/* XXXX todo cid: recheck on the caching bits / ipat bit and when they should be set. */
	/* This is the 0-2MB first set of mappings which we break into 4K PTEs*/
	for(i = 0; i < n_pt; i++){
		if(!(vt_alloc_page((void**)&pt[i].virt, &pt[i].phys))){
			printk(KERN_ERR "okernel: failed to allocate PML1 table.\n");
			return 0;
		}
		memset(pt[i].virt, 0, PAGESIZE);
		HDEBUG(("n=(%d) PML1 pt virt (%llX) pt phys (%llX)\n", i, (unsigned long long)pt[i].virt, pt[i].phys));
	}

	q = pt[0].virt;

	for(i = 0; i < n_entries; i++){
		addr = i << 12;
		if(no_cache_region(addr, PAGESIZE)){
			q[i] = (i << 12) | EPT_R | EPT_W | EPT_X;
		} else {
			q[i] = (i << 12) | EPT_R | EPT_W | EPT_X | EPT_CACHE_2 | EPT_CACHE_3;
		}
	}
	
        /* Now the PD (PML2) tables (plug the pt[0] entry back in later) */
	for(i = 0; i < n_pd; i++){
		if(!(vt_alloc_page((void**)&pd[i].virt, &pd[i].phys))){
			printk(KERN_ERR "okernel: failed to allocate PML2 tables.\n");
			return 0;
		}
		memset(pd[i].virt, 0, PAGESIZE);
		HDEBUG(("n=(%d) PML2 pd virt (%llX) pd phys (%llX)\n", i, (unsigned long long)pd[i].virt, pd[i].phys));
	}
	/* XXXX todo cid: recheck correct CACHE / IPAT attribute setting. */
	for(k = 0; k < n_pd; k++){
		q = pd[k].virt;
		for(i = 0; i < n_entries; i++){
			addr = ((i + k*n_entries) << 21);
			if(no_cache_region(addr,  PAGESIZE2M)){
				q[i] = ((i + k*n_entries) << 21) | EPT_R | EPT_W | EPT_X | EPT_2M_PAGE;
			} else {
				q[i] = ((i + k*n_entries) << 21) | EPT_R | EPT_W | EPT_X | EPT_2M_PAGE | EPT_CACHE_2 | EPT_CACHE_3;
			}
		}
	}

	/* Point just the PD entry covering the 1st 2Mb region to the PT we set
	 * up earlier. The rest of the PD entries directly map a 2Mb
	 * page entry, not a PT table. 
	 */
	q = pd[0].virt;
	q[0] = pt[0].phys + EPT_R + EPT_W + EPT_X;


	/* Now the PDPT (PML3) tables */
	for(i = 0; i < n_pdpt; i++){
		if(!(vt_alloc_page((void**)&pdpt[i].virt, &pdpt[i].phys))){
			printk(KERN_ERR "okernel: failed to allocate PML3 tables.\n");
			return 0;
		}
		memset(pdpt[i].virt, 0, PAGESIZE);
		HDEBUG(("n=(%d) PML3 pdpt virt (%llX) pdpt phys (%llX)\n",
			i, (u64)pdpt[i].virt, pdpt[i].phys));
	}
	/* And link to the PD (PML2) tables created earlier...*/
       for(k = 0; k < n_pdpt; k++){
	    q = pdpt[k].virt;
	    for(i = 0; i < n_pd; i++){
		// These are the PDPTE entries - just 4 at present to map 4GB
		q[i] = pd[i].phys + EPT_R + EPT_W + EPT_X;
	    }
       }

       /* Finally create the PML4 table that is the root of the EPT tables (VMCS EPTRTR field) */
       if(!(vt_alloc_page((void**)&pml4_virt, &pml4_phys))){
	       printk(KERN_ERR "okernel: failed to allocate PML4 table.\n");
	       return 0;
       }
       
       memset(pml4_virt, 0, PAGESIZE);
       q = pml4_virt;
       
       /* Link to the PDPT table above.These are the PML4E entries - just one at present */
       for(i = 0; i < n_pdpt; i++){
	    q[i] = pdpt[i].phys + EPT_R + EPT_W + EPT_X;
       }
       
       HDEBUG(("PML4 plm4_virt (%#lx) *plm4_virt (%#lx) pml4_phys (%#lx)\n", 
	       (unsigned long)pml4_virt, (unsigned long)*pml4_virt,
	       (unsigned long)pml4_phys));
	
       return pml4_phys;
}
/* End: imported code from original BV prototype */



static inline bool cpu_has_secondary_exec_ctrls(void)
{
	return vmcs_config.cpu_based_exec_ctrl &
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;
}

static inline bool cpu_has_vmx_vpid(void)
{
	return vmcs_config.cpu_based_2nd_exec_ctrl &
		SECONDARY_EXEC_ENABLE_VPID;
}

static inline bool cpu_has_vmx_invpcid(void)
{
	return vmcs_config.cpu_based_2nd_exec_ctrl &
		SECONDARY_EXEC_ENABLE_INVPCID;
}

static inline bool cpu_has_vmx_invvpid_single(void)
{
	return vmx_capability.vpid & VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT;
}

static inline bool cpu_has_vmx_invvpid_global(void)
{
	return vmx_capability.vpid & VMX_VPID_EXTENT_GLOBAL_CONTEXT_BIT;
}

static inline bool cpu_has_vmx_ept(void)
{
	return vmcs_config.cpu_based_2nd_exec_ctrl &
		SECONDARY_EXEC_ENABLE_EPT;
}

static inline bool cpu_has_vmx_invept_individual_addr(void)
{
	return vmx_capability.ept & VMX_EPT_EXTENT_INDIVIDUAL_BIT;
}

static inline bool cpu_has_vmx_invept_context(void)
{
	return vmx_capability.ept & VMX_EPT_EXTENT_CONTEXT_BIT;
}

static inline bool cpu_has_vmx_invept_global(void)
{
	return vmx_capability.ept & VMX_EPT_EXTENT_GLOBAL_BIT;
}

static inline bool cpu_has_vmx_ept_ad_bits(void)
{
	return vmx_capability.ept & VMX_EPT_AD_BIT;
}

/*-------------------------------------------------------------------------------------*/
/* code moved                                                                          */
/*-------------------------------------------------------------------------------------*/
static inline void __invept(int ext, u64 eptp, gpa_t gpa)
{
	struct {
		u64 eptp, gpa;
	} operand = {eptp, gpa};

	asm volatile (ASM_VMX_INVEPT
			/* CF==1 or ZF==1 --> rc = -1 */
			"; ja 1f ; ud2 ; 1:\n"
			: : "a" (&operand), "c" (ext) : "cc", "memory");
}

static inline void ept_sync_global(void)
{
	if (cpu_has_vmx_invept_global())
		__invept(VMX_EPT_EXTENT_GLOBAL, 0, 0);
}

static inline void ept_sync_context(u64 eptp)
{
	if (cpu_has_vmx_invept_context())
		__invept(VMX_EPT_EXTENT_CONTEXT, eptp, 0);
	else
		ept_sync_global();
}

static inline void ept_sync_individual_addr(u64 eptp, gpa_t gpa)
{
	if (cpu_has_vmx_invept_individual_addr())
		__invept(VMX_EPT_EXTENT_INDIVIDUAL_ADDR,
				eptp, gpa);
	else
		ept_sync_context(eptp);
}

static inline void __vmxon(u64 addr)
{
	asm volatile (ASM_VMX_VMXON_RAX
			: : "a"(&addr), "m"(addr)
			: "memory", "cc");
}

static inline void __vmxoff(void)
{
	asm volatile (ASM_VMX_VMXOFF : : : "cc");
}

static inline void __invvpid(int ext, u16 vpid, gva_t gva)
{
    struct {
	u64 vpid : 16;
	u64 rsvd : 48;
	u64 gva;
    } operand = { vpid, 0, gva };

    asm volatile (ASM_VMX_INVVPID
		  /* CF==1 or ZF==1 --> rc = -1 */
		  "; ja 1f ; ud2 ; 1:"
		  : : "a"(&operand), "c"(ext) : "cc", "memory");
}

static inline void vpid_sync_vcpu_single(u16 vpid)
{
	if (vpid == 0)
		return;

	if (cpu_has_vmx_invvpid_single())
		__invvpid(VMX_VPID_EXTENT_SINGLE_CONTEXT, vpid, 0);
}

static inline void vpid_sync_vcpu_global(void)
{
	if (cpu_has_vmx_invvpid_global())
		__invvpid(VMX_VPID_EXTENT_ALL_CONTEXT, 0, 0);
}

static inline void vpid_sync_context(u16 vpid)
{
	if (cpu_has_vmx_invvpid_single())
		vpid_sync_vcpu_single(vpid);
	else
		vpid_sync_vcpu_global();
}

/*--------------------------------------------------------------------------------------------*/


static void vmcs_clear(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);
	u8 error;

	asm volatile (ASM_VMX_VMCLEAR_RAX "; setna %0"
		      : "=qm"(error) : "a"(&phys_addr), "m"(phys_addr)
		      : "cc", "memory");
	if (error)
		printk(KERN_ERR "kvm: vmclear fail: %p/%llx\n",
		       vmcs, phys_addr);
}

static void vmcs_load(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);
	u8 error;

	asm volatile (ASM_VMX_VMPTRLD_RAX "; setna %0"
			: "=qm"(error) : "a"(&phys_addr), "m"(phys_addr)
			: "cc", "memory");
	if (error)
		printk(KERN_ERR "vmx: vmptrld %p/%llx failed\n",
		       vmcs, phys_addr);
}

static __always_inline u16 vmcs_read16(unsigned long field)
{
	return vmcs_readl(field);
}

static __always_inline u32 vmcs_read32(unsigned long field)
{
	return vmcs_readl(field);
}

static __always_inline u64 vmcs_read64(unsigned long field)
{
#ifdef CONFIG_X86_64
	return vmcs_readl(field);
#else
	return vmcs_readl(field) | ((u64)vmcs_readl(field+1) << 32);
#endif
}

static noinline void vmwrite_error(unsigned long field, unsigned long value)
{
	printk(KERN_ERR "vmwrite error: reg %lx value %lx (err %d)\n",
	       field, value, vmcs_read32(VM_INSTRUCTION_ERROR));
	dump_stack();
}

static void vmcs_writel(unsigned long field, unsigned long value)
{
	u8 error;

	asm volatile (ASM_VMX_VMWRITE_RAX_RDX "; setna %0"
		       : "=q"(error) : "a"(value), "d"(field) : "cc");
	if (unlikely(error))
		vmwrite_error(field, value);
}

static void vmcs_write16(unsigned long field, u16 value)
{
	vmcs_writel(field, value);
}

static void vmcs_write32(unsigned long field, u32 value)
{
	vmcs_writel(field, value);
}

static void vmcs_write64(unsigned long field, u64 value)
{
	vmcs_writel(field, value);
#ifndef CONFIG_X86_64
	asm volatile ("");
	vmcs_writel(field+1, value >> 32);
#endif
}



static __init int adjust_vmx_controls(u32 ctl_min, u32 ctl_opt,
				      u32 msr, u32 *result)
{
	u32 vmx_msr_low, vmx_msr_high;
	u32 ctl = ctl_min | ctl_opt;

	rdmsr(msr, vmx_msr_low, vmx_msr_high);

	ctl &= vmx_msr_high; /* bit == 0 in high word ==> must be zero */
	ctl |= vmx_msr_low;  /* bit == 1 in low word  ==> must be one  */

	/* Ensure minimum (required) set of control bits are supported. */
	if (ctl_min & ~ctl)
		return -EIO;

	*result = ctl;
	return 0;
}

static __init bool allow_1_setting(u32 msr, u32 ctl)
{
	u32 vmx_msr_low, vmx_msr_high;

	rdmsr(msr, vmx_msr_low, vmx_msr_high);
	return vmx_msr_high & ctl;
}

static __init int setup_vmcs_config(struct vmcs_config *vmcs_conf)
{
	u32 vmx_msr_low, vmx_msr_high;
	u32 min, opt, min2, opt2;
	u32 _pin_based_exec_control = 0;
	u32 _cpu_based_exec_control = 0;
	u32 _cpu_based_2nd_exec_control = 0;
	u32 _vmexit_control = 0;
	u32 _vmentry_control = 0;

	min = PIN_BASED_EXT_INTR_MASK | PIN_BASED_NMI_EXITING;
	opt = PIN_BASED_VIRTUAL_NMIS;
	if (adjust_vmx_controls(min, opt, MSR_IA32_VMX_PINBASED_CTLS,
				&_pin_based_exec_control) < 0)
		return -EIO;

#if 0
	min =

	      CPU_BASED_CR8_LOAD_EXITING |
	      CPU_BASED_CR8_STORE_EXITING |
	      CPU_BASED_CR3_LOAD_EXITING |
	      CPU_BASED_CR3_STORE_EXITING |
	      CPU_BASED_MOV_DR_EXITING |
	      CPU_BASED_USE_TSC_OFFSETING |
	      CPU_BASED_INVLPG_EXITING;
#endif
#if 1
	min = CPU_BASED_USE_TSC_OFFSETING;
		
#endif
	opt = CPU_BASED_TPR_SHADOW |
	      CPU_BASED_USE_MSR_BITMAPS |
	      CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;

	if (adjust_vmx_controls(min, opt, MSR_IA32_VMX_PROCBASED_CTLS,
				&_cpu_based_exec_control) < 0)
		return -EIO;

	if ((_cpu_based_exec_control & CPU_BASED_TPR_SHADOW))
		_cpu_based_exec_control &= ~CPU_BASED_CR8_LOAD_EXITING &
					   ~CPU_BASED_CR8_STORE_EXITING;

	if (_cpu_based_exec_control & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS) {
		min2 = 0;
#if 0
		opt2 =  SECONDARY_EXEC_WBINVD_EXITING |
			SECONDARY_EXEC_ENABLE_VPID |
			SECONDARY_EXEC_ENABLE_EPT |
			SECONDARY_EXEC_RDTSCP |
			SECONDARY_EXEC_ENABLE_INVPCID;
#endif
#if 1
		opt2 =  SECONDARY_EXEC_WBINVD_EXITING |
			SECONDARY_EXEC_ENABLE_VPID |
			SECONDARY_EXEC_ENABLE_EPT |
			SECONDARY_EXEC_RDTSCP;
#endif
		if (adjust_vmx_controls(min2, opt2,
					MSR_IA32_VMX_PROCBASED_CTLS2,
					&_cpu_based_2nd_exec_control) < 0)
			return -EIO;
	}

	if (_cpu_based_2nd_exec_control & SECONDARY_EXEC_ENABLE_EPT) {
		/* CR3 accesses and invlpg don't need to cause VM Exits when EPT
		   enabled */
		_cpu_based_exec_control &= ~(CPU_BASED_CR3_LOAD_EXITING |
					     CPU_BASED_CR3_STORE_EXITING |
					     CPU_BASED_INVLPG_EXITING);
		rdmsr(MSR_IA32_VMX_EPT_VPID_CAP,
		      vmx_capability.ept, vmx_capability.vpid);
	}

	min = 0;

	min |= VM_EXIT_HOST_ADDR_SPACE_SIZE;

//	opt = VM_EXIT_SAVE_IA32_PAT | VM_EXIT_LOAD_IA32_PAT;
	opt = 0;
	if (adjust_vmx_controls(min, opt, MSR_IA32_VMX_EXIT_CTLS,
				&_vmexit_control) < 0)
		return -EIO;

	min = 0;
//	opt = VM_ENTRY_LOAD_IA32_PAT;
	opt = 0;
	if (adjust_vmx_controls(min, opt, MSR_IA32_VMX_ENTRY_CTLS,
				&_vmentry_control) < 0)
		return -EIO;

	rdmsr(MSR_IA32_VMX_BASIC, vmx_msr_low, vmx_msr_high);

	/* IA-32 SDM Vol 3B: VMCS size is never greater than 4kB. */
	if ((vmx_msr_high & 0x1fff) > PAGE_SIZE)
		return -EIO;


	/* IA-32 SDM Vol 3B: 64-bit CPUs always have VMX_BASIC_MSR[48]==0. */
	if (vmx_msr_high & (1u<<16))
		return -EIO;

	/* Require Write-Back (WB) memory type for VMCS accesses. */
	if (((vmx_msr_high >> 18) & 15) != 6)
		return -EIO;

	vmcs_conf->size = vmx_msr_high & 0x1fff;
	vmcs_conf->order = get_order(vmcs_config.size);
	vmcs_conf->revision_id = vmx_msr_low;

	vmcs_conf->pin_based_exec_ctrl = _pin_based_exec_control;
	vmcs_conf->cpu_based_exec_ctrl = _cpu_based_exec_control;
	vmcs_conf->cpu_based_2nd_exec_ctrl = _cpu_based_2nd_exec_control;
	vmcs_conf->vmexit_ctrl         = _vmexit_control;
	vmcs_conf->vmentry_ctrl        = _vmentry_control;

	vmx_capability.has_load_efer =
		allow_1_setting(MSR_IA32_VMX_ENTRY_CTLS,
				VM_ENTRY_LOAD_IA32_EFER)
		&& allow_1_setting(MSR_IA32_VMX_EXIT_CTLS,
				   VM_EXIT_LOAD_IA32_EFER);

	return 0;
}







static struct vmcs *__vmx_alloc_vmcs(int cpu)
{
	int node = cpu_to_node(cpu);
	struct page *pages;
	struct vmcs *vmcs;

	pages = alloc_pages_exact_node(node, GFP_KERNEL, vmcs_config.order);
	if (!pages)
		return NULL;
	vmcs = page_address(pages);
	memset(vmcs, 0, vmcs_config.size);
	vmcs->revision_id = vmcs_config.revision_id; /* vmcs revision id */
	return vmcs;
}


/**
 * vmx_free_vmcs - frees a VMCS region
 */
static void vmx_free_vmcs(struct vmcs *vmcs)
{
	free_pages((unsigned long)vmcs, vmcs_config.order);
}




/*-------------------------------------------------------------------------------------*/
/*  start: vmx__launch releated code                                                   */
/*-------------------------------------------------------------------------------------*/

/*
 * Set up the vmcs's constant host-state fields, i.e., host-state fields that
 * will not change in the lifetime of the guest.
 * Note that host-state that does change is set elsewhere. E.g., host-state
 * that is set differently for each CPU is set in vmx_vcpu_load(), not here.
 */
static void vmx_setup_constant_host_state(void)
{
	u32 low32, high32;
	unsigned long tmpl;
	struct desc_ptr dt;

	vmcs_writel(HOST_CR0, read_cr0() & ~X86_CR0_TS);  /* 22.2.3 */
	vmcs_writel(HOST_CR4, native_read_cr4());  /* 22.2.3, 22.2.5 */
	vmcs_writel(HOST_CR3, read_cr3());  /* 22.2.3 */

	vmcs_write16(HOST_CS_SELECTOR, __KERNEL_CS);  /* 22.2.4 */
	vmcs_write16(HOST_DS_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
	vmcs_write16(HOST_ES_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
	vmcs_write16(HOST_SS_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);  /* 22.2.4 */

	native_store_idt(&dt);
	vmcs_writel(HOST_IDTR_BASE, dt.address);   /* 22.2.4 */

	asm("mov $.Lkvm_vmx_return, %0" : "=r"(tmpl));
	vmcs_writel(HOST_RIP, tmpl); /* 22.2.5 */

	rdmsr(MSR_IA32_SYSENTER_CS, low32, high32);
	vmcs_write32(HOST_IA32_SYSENTER_CS, low32);
	rdmsrl(MSR_IA32_SYSENTER_EIP, tmpl);
	vmcs_writel(HOST_IA32_SYSENTER_EIP, tmpl);   /* 22.2.3 */

	rdmsr(MSR_EFER, low32, high32);
	vmcs_write32(HOST_IA32_EFER, low32);

	if (vmcs_config.vmexit_ctrl & VM_EXIT_LOAD_IA32_PAT) {
		rdmsr(MSR_IA32_CR_PAT, low32, high32);
		vmcs_write64(HOST_IA32_PAT, low32 | ((u64) high32 << 32));
	}

	vmcs_write16(HOST_FS_SELECTOR, 0);            /* 22.2.4 */
	vmcs_write16(HOST_GS_SELECTOR, 0);            /* 22.2.4 */

#ifdef CONFIG_X86_64
	rdmsrl(MSR_FS_BASE, tmpl);
	vmcs_writel(HOST_FS_BASE, tmpl); /* 22.2.4 */
	rdmsrl(MSR_GS_BASE, tmpl);
	vmcs_writel(HOST_GS_BASE, tmpl); /* 22.2.4 */
#else
	vmcs_writel(HOST_FS_BASE, 0); /* 22.2.4 */
	vmcs_writel(HOST_GS_BASE, 0); /* 22.2.4 */
#endif
}


static inline u16 vmx_read_ldt(void)
{
	u16 ldt;
	asm("sldt %0" : "=g"(ldt));
	return ldt;
}

static unsigned long segment_base(u16 selector)
{
	struct desc_ptr *gdt = this_cpu_ptr(&host_gdt);
	struct desc_struct *d;
	unsigned long table_base;
	unsigned long v;

	if (!(selector & ~3))
		return 0;

	table_base = gdt->address;

	if (selector & 4) {           /* from ldt */
		u16 ldt_selector = vmx_read_ldt();

		if (!(ldt_selector & ~3))
			return 0;

		table_base = segment_base(ldt_selector);
	}
	d = (struct desc_struct *)(table_base + (selector & ~7));
	v = get_desc_base(d);
#ifdef CONFIG_X86_64
       if (d->s == 0 && (d->type == 2 || d->type == 9 || d->type == 11))
               v |= ((unsigned long)((struct ldttss_desc64 *)d)->base3) << 32;
#endif
	return v;
}

static inline unsigned long vmx_read_tr_base(void)
{
	u16 tr;
	asm("str %0" : "=g"(tr));
	return segment_base(tr);
}

static void __vmx_setup_cpu(void)
{
	struct desc_ptr *gdt = this_cpu_ptr(&host_gdt);
	unsigned long sysenter_esp;
	unsigned long tmpl;

	/*
	 * Linux uses per-cpu TSS and GDT, so set these when switching
	 * processors.
	 */
	vmcs_writel(HOST_TR_BASE, vmx_read_tr_base()); /* 22.2.4 */
	vmcs_writel(HOST_GDTR_BASE, gdt->address);   /* 22.2.4 */

	rdmsrl(MSR_IA32_SYSENTER_ESP, sysenter_esp);
	vmcs_writel(HOST_IA32_SYSENTER_ESP, sysenter_esp); /* 22.2.3 */

	rdmsrl(MSR_FS_BASE, tmpl);
	vmcs_writel(HOST_FS_BASE, tmpl); /* 22.2.4 */
	rdmsrl(MSR_GS_BASE, tmpl);
	vmcs_writel(HOST_GS_BASE, tmpl); /* 22.2.4 */
}

static void __vmx_get_cpu_helper(void *ptr)
{
	struct vmx_vcpu *vcpu = ptr;

	BUG_ON(raw_smp_processor_id() != vcpu->cpu);
	vmcs_clear(vcpu->vmcs);
	if (__this_cpu_read(local_vcpu) == vcpu)
		__this_cpu_write(local_vcpu, NULL);
}

/**
 * vmx_get_cpu - called before using a cpu
 * @vcpu: VCPU that will be loaded.
 *
 * Disables preemption. Call vmx_put_cpu() when finished.
 */
static void vmx_get_cpu(struct vmx_vcpu *vcpu)
{
	int cur_cpu = get_cpu();

	if(vcpu->launched != 0){
		vmcs_load(vcpu->vmcs);
	} else {
		if (__this_cpu_read(local_vcpu) != vcpu) {
			__this_cpu_write(local_vcpu, vcpu);
			
			if (vcpu->cpu != cur_cpu) {
				if (vcpu->cpu >= 0)
					smp_call_function_single(vcpu->cpu,
								 __vmx_get_cpu_helper, (void *) vcpu, 1);
				else
					vmcs_clear(vcpu->vmcs);

				vpid_sync_context(vcpu->vpid);
				ept_sync_context(vcpu->eptp);
				
				vcpu->launched = 0;
				vmcs_load(vcpu->vmcs);
				__vmx_setup_cpu();
				vcpu->cpu = cur_cpu;
			} else {
				vmcs_load(vcpu->vmcs);
			}
		}
	}
}

/**
 * vmx_put_cpu - called after using a cpu
 * @vcpu: VCPU that was loaded.
 */
static void vmx_put_cpu(struct vmx_vcpu *vcpu)
{
	put_cpu();
}

static void __vmx_sync_helper(void *ptr)
{
	struct vmx_vcpu *vcpu = ptr;

	ept_sync_context(vcpu->eptp);
}

struct sync_addr_args {
	struct vmx_vcpu *vcpu;
	gpa_t gpa;
};


static void __vmx_sync_individual_addr_helper(void *ptr)
{
	struct sync_addr_args *args = ptr;

	ept_sync_individual_addr(args->vcpu->eptp,
				 (args->gpa & ~(PAGE_SIZE - 1)));
}

/**
 * vmx_ept_sync_global - used to evict everything in the EPT
 * @vcpu: the vcpu
 */
void vmx_ept_sync_vcpu(struct vmx_vcpu *vcpu)
{
	smp_call_function_single(vcpu->cpu,
		__vmx_sync_helper, (void *) vcpu, 1);
}

/**
 * vmx_ept_sync_individual_addr - used to evict an individual address
 * @vcpu: the vcpu
 * @gpa: the guest-physical address
 */
void vmx_ept_sync_individual_addr(struct vmx_vcpu *vcpu, gpa_t gpa)
{
	struct sync_addr_args args;
	args.vcpu = vcpu;
	args.gpa = gpa;

	smp_call_function_single(vcpu->cpu,
		__vmx_sync_individual_addr_helper, (void *) &args, 1);
}


static u64 construct_eptp(unsigned long root_hpa)
{
	u64 eptp;

	/* TODO write the value reading from MSR */
	eptp = VMX_EPT_DEFAULT_MT |
		VMX_EPT_DEFAULT_GAW << VMX_EPT_GAW_EPTP_SHIFT;
	if (cpu_has_vmx_ept_ad_bits())
		eptp |= VMX_EPT_AD_ENABLE_BIT;
	eptp |= (root_hpa & PAGE_MASK);

	return eptp;
}

/**
 * vmx_alloc_vmcs - allocates a VMCS region
 *
 * NOTE: Assumes the new region will be used by the current CPU.
 *
 * Returns a valid VMCS region.
 */
static struct vmcs *vmx_alloc_vmcs(void)
{
	return __vmx_alloc_vmcs(raw_smp_processor_id());
}



	
struct vmcs_cpu_state {
	unsigned long rsp;
	unsigned long rbp;
	unsigned long cr0;
	unsigned long cr3;
	unsigned long cr4;
	unsigned long rflags;
	unsigned long efer;
	
	u16 cs_selector;
	u16 ds_selector;
	u16 es_selector;
	u16 ss_selector;
	u16 tr_selector;
	u16 fs_selector;
	u16 gs_selector;

	unsigned long  idt_base;
	unsigned long  gdt_base;
	unsigned long  ldt_base;
	unsigned short idt_limit;
	unsigned short gdt_limit;
	unsigned short ldt_limit;

	unsigned long  tr_base;
	unsigned short tr_limit;
	
	unsigned long cs_base;
	unsigned long ds_base;
	unsigned long es_base;
	unsigned long ss_base;	
	unsigned long fs_base;
	unsigned long gs_base;
	
	unsigned long sysenter_cs;
	unsigned long sysenter_eip;
	unsigned long sysenter_esp;
};

void show_cpu_state(struct vmcs_cpu_state state)
{
	HDEBUG(("Control regs / flags: \n"));
	HDEBUG(("rsp     (%#lx)\n", state.rsp));
	HDEBUG(("rbp     (%#lx)\n", state.rbp));
	HDEBUG(("cr0     (%#lx)\n", state.cr0));
	HDEBUG(("cr3     (%#lx)\n", state.cr3));
	HDEBUG(("cr4     (%#lx)\n", state.cr4));	
	HDEBUG(("rflags  (%#lx)\n", state.rflags));
	HDEBUG(("efer    (%#lx)\n", state.efer));

	HDEBUG(("idt base (%#lx) limit (%#x)\n", state.idt_base, state.idt_limit));
	HDEBUG(("gdt base (%#lx) limit (%#x)\n", state.gdt_base, state.gdt_limit));
	HDEBUG(("ldt base (%#lx) limit (%#x)\n", state.ldt_base, state.ldt_limit));	
	
	HDEBUG(("Selectors: \n"));
	HDEBUG(("cs_s (%#x) ds_s (%#x) es_s (%#x) ss_s (%#x) tr_s (%#x)\n",
		state.cs_selector, state.ds_selector, state.es_selector,
		state.ss_selector, state.tr_selector));
	HDEBUG(("fs_s (%#x) gs_s (%#x)\n",
		state.fs_selector, state.gs_selector));

	HDEBUG(("fs_base (%#lx) gs_base (%#lx)\n",
		state.fs_base,state.gs_base));

	HDEBUG(("sysenter_cs (%lx), systenter_esp (%lx) systenter_eip (%lx)\n",
		state.sysenter_cs, state.sysenter_esp, state.sysenter_eip));
	return;
}

void get_cpu_state(struct vmcs_cpu_state* cpu_state)
{
	u32 low32, high32;
	struct desc_ptr idt;
	struct desc_ptr gdt;
	//struct desc_ptr ldt;
	unsigned long tr;
	unsigned long tmpl;

	
	/* Start with control regs / flags */
	cpu_state->rsp = cloned_thread.rsp;
	cpu_state->rflags = cloned_thread.rflags;
	cpu_state->rbp = cloned_thread.rbp;

	cpu_state->cr0 = read_cr0();
	cpu_state->cr3 = read_cr3();
	cpu_state->cr4 = native_read_cr4();
        
	rdmsr(MSR_EFER, low32, high32);
	cpu_state->efer = low32;

	
	/* Segment Selectors */
	cpu_state->cs_selector = __KERNEL_CS;
	cpu_state->ds_selector = __KERNEL_DS;
	cpu_state->es_selector = __KERNEL_DS;
	cpu_state->ss_selector = __KERNEL_DS;
	cpu_state->tr_selector = GDT_ENTRY_TSS*8;
	cpu_state->fs_selector = 0;
	cpu_state->gs_selector = 0;

	/* Segment Base + Limits */
	rdmsrl(MSR_FS_BASE, tmpl);
	cpu_state->fs_base = tmpl;
	rdmsrl(MSR_GS_BASE, tmpl);
	cpu_state->gs_base = tmpl;
	
	/*Segment AR Bytes */

	/* IDT, GDT, LDT */
	native_store_idt(&idt);
	cpu_state->idt_base = idt.address;
	cpu_state->idt_limit = idt.size;

	native_store_gdt(&gdt);
	cpu_state->gdt_base = gdt.address;
	cpu_state->gdt_limit = gdt.size;
	
	//native_store_ldt(&ldt);
	cpu_state->ldt_base = 0;
	cpu_state->ldt_limit = 0;

	tr = native_store_tr();
	cpu_state->tr_base = tr;
	cpu_state->tr_limit = 0xff;
	
	
	/* sysenter */
	rdmsrl(MSR_IA32_SYSENTER_CS, tmpl);
	cpu_state->sysenter_cs =  tmpl;
	rdmsrl(MSR_IA32_SYSENTER_EIP, tmpl);
	cpu_state->sysenter_eip = tmpl;
	rdmsrl(MSR_IA32_SYSENTER_ESP, tmpl);
	cpu_state->sysenter_esp =  tmpl;
	return;
}

#if 0
/********************************************************************************/
/* Clone this host state into the 'guest'                                       */
/********************************************************************************/

	if (vmcs_config.vmexit_ctrl & VM_EXIT_LOAD_IA32_PAT) {
		rdmsr(MSR_IA32_CR_PAT, low32, high32);
		vmcs_write64(HOST_IA32_PAT, low32 | ((u64) high32 << 32));
	}

/********************************************************************************/
/* Clone this host state into the 'guest' - done.                               */
/********************************************************************************/
#endif

/**
 * vmx_setup_initial_guest_state - configures the initial state of guest registers
 */
static void vmx_setup_initial_guest_state(struct vmx_vcpu *vcpu)
{
	
	/* Need to mask out X64_CR4_VMXE in guest read shadow */
	unsigned long cr4_mask = X86_CR4_VMXE;
	unsigned long cr4_shadow; 
	unsigned long cr4;

	struct vmcs_cpu_state current_cpu_state;
	struct pt_regs *regs;

	regs = task_pt_regs(current);
	
	get_cpu_state(&current_cpu_state);
	show_cpu_state(current_cpu_state);

	//vcpu->regs[VCPU_REGS_RSP] = cloned_thread.rsp;
	vcpu->regs[VCPU_REGS_RBP] = cloned_thread.rbp;
	vcpu->regs[VCPU_REGS_RAX] = cloned_thread.rax;
	vcpu->regs[VCPU_REGS_RCX] = cloned_thread.rcx;
	vcpu->regs[VCPU_REGS_RDX] = cloned_thread.rdx;
	vcpu->regs[VCPU_REGS_RBX] = cloned_thread.rbx;
	vcpu->regs[VCPU_REGS_RSI] = cloned_thread.rsi;
	vcpu->regs[VCPU_REGS_RDI] = cloned_thread.rdi;
	vcpu->regs[VCPU_REGS_R8] = cloned_thread.r8;
	vcpu->regs[VCPU_REGS_R9] = cloned_thread.r9;
	vcpu->regs[VCPU_REGS_R10] = cloned_thread.r10;
	vcpu->regs[VCPU_REGS_R11] = cloned_thread.r11;
	vcpu->regs[VCPU_REGS_R12] = cloned_thread.r12;
	vcpu->regs[VCPU_REGS_R13] = cloned_thread.r13;
	vcpu->regs[VCPU_REGS_R14] = cloned_thread.r14;
	vcpu->regs[VCPU_REGS_R15] = cloned_thread.r15;
	vcpu->cr2 = cloned_thread.cr2;
	

#if 0
	HDEBUG(("----start of 'current' regs from __show_regs:\n"));
	__show_regs(regs, 1);
	HDEBUG(("----end of 'current' regs from __show_regs.\n"));
#endif  
	/* Most likely will need to adjust */
	cr4 = current_cpu_state.cr4;
	cr4_shadow = (cr4 & ~X86_CR4_VMXE);
	vmcs_writel(GUEST_CR0, current_cpu_state.cr0);	
	vmcs_writel(CR0_READ_SHADOW, current_cpu_state.cr0);	
	vmcs_writel(GUEST_CR3, current_cpu_state.cr3);

	/* Make sure VMXE is not visible under a vcpu: we use this currently */
	/* as a way of detecting whether in root or NR mode. */
	vmcs_writel(GUEST_CR4, cr4);
	vmcs_writel(CR4_GUEST_HOST_MASK, cr4_mask);
	vmcs_writel(CR4_READ_SHADOW, cr4_shadow);

	/* Most of this we can set from the host state apart. Need to make
	   sure we clone the kernel stack pages in the EPT mapping.
	*/

	vmcs_writel(GUEST_RIP, cloned_thread.rip);
	//vmcs_writel(GUEST_RSP, current_cpu_state.rsp);
	vmcs_writel(GUEST_RSP, cloned_thread.rsp);
	vmcs_writel(GUEST_RFLAGS, cloned_thread.rflags);
	//vmcs_writel(GUEST_RFLAGS, 0x2);
	vmcs_writel(GUEST_IA32_EFER, current_cpu_state.efer);

	/* configure segment selectors */
	vmcs_write16(GUEST_CS_SELECTOR, current_cpu_state.cs_selector);
	vmcs_write16(GUEST_DS_SELECTOR, current_cpu_state.ds_selector);
	vmcs_write16(GUEST_ES_SELECTOR, current_cpu_state.es_selector);
	vmcs_write16(GUEST_FS_SELECTOR, current_cpu_state.fs_selector);
	vmcs_write16(GUEST_GS_SELECTOR, current_cpu_state.gs_selector);
	vmcs_write16(GUEST_SS_SELECTOR, current_cpu_state.ss_selector);
	vmcs_write16(GUEST_TR_SELECTOR, current_cpu_state.tr_selector);

        /* initialize sysenter */
	vmcs_write32(GUEST_SYSENTER_CS, current_cpu_state.sysenter_cs);
	vmcs_writel(GUEST_SYSENTER_ESP, current_cpu_state.sysenter_esp);
	vmcs_writel(GUEST_SYSENTER_EIP, current_cpu_state.sysenter_eip);
	
	vmcs_writel(GUEST_GDTR_BASE, current_cpu_state.gdt_base);
	vmcs_writel(GUEST_GDTR_LIMIT, current_cpu_state.gdt_limit);
	vmcs_writel(GUEST_IDTR_BASE, current_cpu_state.idt_base);
	vmcs_writel(GUEST_IDTR_LIMIT, current_cpu_state.idt_limit);


        /* guest LDTR */
	vmcs_write16(GUEST_LDTR_SELECTOR, 0);
	vmcs_writel(GUEST_LDTR_AR_BYTES, 0x0082);
	vmcs_writel(GUEST_LDTR_BASE, 0);
	vmcs_writel(GUEST_LDTR_LIMIT, 0);

	vmcs_writel(GUEST_TR_BASE, current_cpu_state.tr_base);
	vmcs_writel(GUEST_TR_LIMIT, current_cpu_state.tr_limit);
	vmcs_writel(GUEST_TR_AR_BYTES, 0x0080 | AR_TYPE_BUSY_64_TSS);

	
	// DO WE NEED CHANGE ANY OF THESE???
	vmcs_writel(GUEST_DR7, 0);

	/* guest segment bases */
	vmcs_writel(GUEST_CS_BASE, 0);
	vmcs_writel(GUEST_DS_BASE, 0);
	vmcs_writel(GUEST_ES_BASE, 0);
	vmcs_writel(GUEST_GS_BASE, current_cpu_state.gs_base);
	vmcs_writel(GUEST_SS_BASE, 0);
	vmcs_writel(GUEST_FS_BASE, current_cpu_state.fs_base);

        /* guest segment access rights */
	vmcs_writel(GUEST_CS_AR_BYTES, 0xA09B);
	vmcs_writel(GUEST_DS_AR_BYTES, 0xA093);
	vmcs_writel(GUEST_ES_AR_BYTES, 0xA093);
	vmcs_writel(GUEST_FS_AR_BYTES, 0xA093);
	vmcs_writel(GUEST_GS_AR_BYTES, 0xA093);
	vmcs_writel(GUEST_SS_AR_BYTES, 0xA093);

	/* guest segment limits */
	vmcs_write32(GUEST_CS_LIMIT, 0xFFFFFFFF);
	vmcs_write32(GUEST_DS_LIMIT, 0xFFFFFFFF);
	vmcs_write32(GUEST_ES_LIMIT, 0xFFFFFFFF);
	vmcs_write32(GUEST_FS_LIMIT, 0xFFFFFFFF);
	vmcs_write32(GUEST_GS_LIMIT, 0xFFFFFFFF);
	vmcs_write32(GUEST_SS_LIMIT, 0xFFFFFFFF);

	/* other random initialization */
	vmcs_write32(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_write32(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	vmcs_write64(GUEST_IA32_DEBUGCTL, 0);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);  /* 22.2.1 */
	return;
}


static void __vmx_disable_intercept_for_msr(unsigned long *msr_bitmap, u32 msr)
{
	int f = sizeof(unsigned long);
	/*
	 * See Intel PRM Vol. 3, 20.6.9 (MSR-Bitmap Address). Early manuals
	 * have the write-low and read-high bitmap offsets the wrong way round.
	 * We can control MSRs 0x00000000-0x00001fff and 0xc0000000-0xc0001fff.
	 */
	if (msr <= 0x1fff) {
		__clear_bit(msr, msr_bitmap + 0x000 / f); /* read-low */
		__clear_bit(msr, msr_bitmap + 0x800 / f); /* write-low */
	} else if ((msr >= 0xc0000000) && (msr <= 0xc0001fff)) {
		msr &= 0x1fff;
		__clear_bit(msr, msr_bitmap + 0x400 / f); /* read-high */
		__clear_bit(msr, msr_bitmap + 0xc00 / f); /* write-high */
	}
}

static void setup_msr(struct vmx_vcpu *vcpu)
{
	int set[] = { MSR_LSTAR };
	struct vmx_msr_entry *e;
	int sz = sizeof(set) / sizeof(*set);
	int i;

	sz = 0;

	BUILD_BUG_ON(sz > NR_AUTOLOAD_MSRS);

	vcpu->msr_autoload.nr = sz;

	/* XXX enable only MSRs in set */
	vmcs_write64(MSR_BITMAP, __pa(msr_bitmap));

	vmcs_write32(VM_EXIT_MSR_STORE_COUNT, vcpu->msr_autoload.nr);
	vmcs_write32(VM_EXIT_MSR_LOAD_COUNT, vcpu->msr_autoload.nr);
	vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, vcpu->msr_autoload.nr);

	vmcs_write64(VM_EXIT_MSR_LOAD_ADDR, __pa(vcpu->msr_autoload.host));
	vmcs_write64(VM_EXIT_MSR_STORE_ADDR, __pa(vcpu->msr_autoload.guest));
	vmcs_write64(VM_ENTRY_MSR_LOAD_ADDR, __pa(vcpu->msr_autoload.guest));

	for (i = 0; i < sz; i++) {
		uint64_t val;

		e = &vcpu->msr_autoload.host[i];
		e->index = set[i];
		rdmsrl(e->index, val);
		e->value = val;

		e = &vcpu->msr_autoload.guest[i];
		e->index = set[i];
	}
}


/**
 *  vmx_setup_vmcs - configures the vmcs with starting parameters
 */
static void vmx_setup_vmcs(struct vmx_vcpu *vcpu)
{
	vmcs_write16(VIRTUAL_PROCESSOR_ID, vcpu->vpid);
	vmcs_write64(VMCS_LINK_POINTER, -1ull); /* 22.3.1.5 */

	/* Control */
	vmcs_write32(PIN_BASED_VM_EXEC_CONTROL,
		vmcs_config.pin_based_exec_ctrl);

	vmcs_write32(CPU_BASED_VM_EXEC_CONTROL,
		vmcs_config.cpu_based_exec_ctrl);

	if (cpu_has_secondary_exec_ctrls()) {
		vmcs_write32(SECONDARY_VM_EXEC_CONTROL,
			     vmcs_config.cpu_based_2nd_exec_ctrl);
	}

	vmcs_write64(EPT_POINTER, vcpu->eptp);

	vmcs_write32(PAGE_FAULT_ERROR_CODE_MASK, 0);
	vmcs_write32(PAGE_FAULT_ERROR_CODE_MATCH, 0);
	vmcs_write32(CR3_TARGET_COUNT, 0);           /* 22.2.1 */

	vmcs_write64(MSR_BITMAP, __pa(msr_bitmap));
	
#if 0
	//setup_msr(vcpu);
	
	if (vmcs_config.vmentry_ctrl & VM_ENTRY_LOAD_IA32_PAT) {
		u32 msr_low, msr_high;
		u64 host_pat;
		rdmsr(MSR_IA32_CR_PAT, msr_low, msr_high);
		host_pat = msr_low | ((u64) msr_high << 32);
		/* Write the default value follow host pat */
		vmcs_write64(GUEST_IA32_PAT, host_pat);
		/* Keep arch.pat sync with GUEST_IA32_PAT */
		vmx->vcpu.arch.pat = host_pat;
	}

	for (i = 0; i < NR_VMX_MSR; ++i) {
		u32 index = vmx_msr_index[i];
		u32 data_low, data_high;
		int j = vmx->nmsrs;

		if (rdmsr_safe(index, &data_low, &data_high) < 0)
			continue;
		if (wrmsr_safe(index, data_low, data_high) < 0)
			continue;
		vmx->guest_msrs[j].index = i;
		vmx->guest_msrs[j].data = 0;
		vmx->guest_msrs[j].mask = -1ull;
		++vmx->nmsrs;
	}
#endif

	vmcs_config.vmentry_ctrl |= VM_ENTRY_IA32E_MODE;

	vmcs_write32(VM_EXIT_CONTROLS, vmcs_config.vmexit_ctrl);
	vmcs_write32(VM_ENTRY_CONTROLS, vmcs_config.vmentry_ctrl);

	vmcs_writel(CR0_GUEST_HOST_MASK, ~0ul);
	vmcs_writel(CR4_GUEST_HOST_MASK, ~0ul);

	//kvm_write_tsc(&vmx->vcpu, 0);
	vmcs_writel(TSC_OFFSET, 0);
	vmx_setup_constant_host_state();
}



/**
 * vmx_allocate_vpid - reserves a vpid and sets it in the VCPU
 * @vmx: the VCPU
 */
static int vmx_allocate_vpid(struct vmx_vcpu *vmx)
{
	int vpid;

	vmx->vpid = 0;

	spin_lock(&vmx_vpid_lock);
	vpid = find_first_zero_bit(vmx_vpid_bitmap, VMX_NR_VPIDS);
	if (vpid < VMX_NR_VPIDS) {
		vmx->vpid = vpid;
		__set_bit(vpid, vmx_vpid_bitmap);
	}
	spin_unlock(&vmx_vpid_lock);

	return vpid >= VMX_NR_VPIDS;
}

/**
 * vmx_free_vpid - frees a vpid
 * @vmx: the VCPU
 */
static void vmx_free_vpid(struct vmx_vcpu *vmx)
{
	spin_lock(&vmx_vpid_lock);
	if (vmx->vpid != 0)
		__clear_bit(vmx->vpid, vmx_vpid_bitmap);
	spin_unlock(&vmx_vpid_lock);
}



/**
 * vmx_create_vcpu - allocates and initializes a new virtual cpu
 *
 * Returns: A new VCPU structure
 */
static struct vmx_vcpu * vmx_create_vcpu(void)
{
	struct vmx_vcpu *vcpu = kmalloc(sizeof(struct vmx_vcpu), GFP_KERNEL);

	HDEBUG(("0\n"));
	if (!vcpu)
		return NULL;

	HDEBUG(("1\n"));
	memset(vcpu, 0, sizeof(*vcpu));

	vcpu->vmcs = vmx_alloc_vmcs();

	HDEBUG(("2\n"));
	
	if (!vcpu->vmcs)
		goto fail_vmcs;

	if (vmx_allocate_vpid(vcpu))
		goto fail_vpid;

	HDEBUG(("3\n"));
	vcpu->cpu = -1;
	//vcpu->syscall_tbl = (void *) &dune_syscall_tbl;


	spin_lock_init(&vcpu->ept_lock);
	//if (vmx_init_ept(vcpu))
	//	goto fail_ept;

	vcpu->ept_root = vt_ept_2M_init();

	if(vcpu->ept_root == 0)
		goto fail_ept;
	
	HDEBUG(("4\n"));
	vcpu->eptp = construct_eptp(vcpu->ept_root);

	HDEBUG(("5\n"));

	vmx_get_cpu(vcpu);
	HDEBUG(("6\n"));
	vmx_setup_vmcs(vcpu);
	
	HDEBUG(("7\n"));
	
	vmx_setup_initial_guest_state(vcpu);

	HDEBUG(("8\n"));	
	vmx_put_cpu(vcpu);
	HDEBUG(("9\n"));
	
	if (cpu_has_vmx_ept_ad_bits()) {
		vcpu->ept_ad_enabled = true;
		printk(KERN_INFO "vmx: enabled EPT A/D bits");
	}
	HDEBUG(("10\n"));
	if (vmx_create_ept(vcpu))
		goto fail_ept;

	HDEBUG(("11\n"));
	return vcpu;

fail_ept:
	HDEBUG(("12\n"));
	vmx_free_vpid(vcpu);
fail_vpid:
	HDEBUG(("13\n"));
	vmx_free_vmcs(vcpu->vmcs);
fail_vmcs:
	HDEBUG(("14\n"));
	kfree(vcpu);
	return NULL;
}

/**
 * vmx_destroy_vcpu - destroys and frees an existing virtual cpu
 * @vcpu: the VCPU to destroy
 */
static void vmx_destroy_vcpu(struct vmx_vcpu *vcpu)
{
	vmx_destroy_ept(vcpu);
	vmx_get_cpu(vcpu);
	ept_sync_context(vcpu->eptp);
	vmcs_clear(vcpu->vmcs);
	__this_cpu_write(local_vcpu, NULL);
	vmx_put_cpu(vcpu);
	vmx_free_vpid(vcpu);
	vmx_free_vmcs(vcpu->vmcs);
	kfree(vcpu);
}


#ifdef CONFIG_X86_64
#define R "r"
#define Q "q"
#else
#define R "e"
#define Q "l"
#endif

/**
 * vmx_run_vcpu - launches the CPU into non-root mode
 * @vcpu: the vmx instance to launch
 */
static int __noclone vmx_run_vcpu(struct vmx_vcpu *vcpu)
{
	asm(
		/* Store host registers */
		"push %%"R"dx; push %%"R"bp;"
		"push %%"R"cx \n\t" /* placeholder for guest rcx */
		"push %%"R"cx \n\t"
		"cmp %%"R"sp, %c[host_rsp](%0) \n\t"
		"je 1f \n\t"
		"mov %%"R"sp, %c[host_rsp](%0) \n\t"
		ASM_VMX_VMWRITE_RSP_RDX "\n\t"
		"1: \n\t"
		/* Reload cr2 if changed */
		"mov %c[cr2](%0), %%"R"ax \n\t"
		"mov %%cr2, %%"R"dx \n\t"
		"cmp %%"R"ax, %%"R"dx \n\t"
		"je 2f \n\t"
		"mov %%"R"ax, %%cr2 \n\t"
		"2: \n\t"
		/* Check if vmlaunch of vmresume is needed */
		"cmpl $0, %c[launched](%0) \n\t"
		/* Load guest registers.  Don't clobber flags. */
		"mov %c[rax](%0), %%"R"ax \n\t"
		"mov %c[rbx](%0), %%"R"bx \n\t"
		"mov %c[rdx](%0), %%"R"dx \n\t"
		"mov %c[rsi](%0), %%"R"si \n\t"
		"mov %c[rdi](%0), %%"R"di \n\t"
		"mov %c[rbp](%0), %%"R"bp \n\t"
#ifdef CONFIG_X86_64
		"mov %c[r8](%0),  %%r8  \n\t"
		"mov %c[r9](%0),  %%r9  \n\t"
		"mov %c[r10](%0), %%r10 \n\t"
		"mov %c[r11](%0), %%r11 \n\t"
		"mov %c[r12](%0), %%r12 \n\t"
		"mov %c[r13](%0), %%r13 \n\t"
		"mov %c[r14](%0), %%r14 \n\t"
		"mov %c[r15](%0), %%r15 \n\t"
#endif
		"mov %c[rcx](%0), %%"R"cx \n\t" /* kills %0 (ecx) */
		// "xchg %%bx, %%bx \n\t"
		/* Enter guest mode */
		"jne .Llaunched \n\t"
		ASM_VMX_VMLAUNCH "\n\t"
		"jmp .Lkvm_vmx_return \n\t"
		".Llaunched: " ASM_VMX_VMRESUME "\n\t"
		".Lkvm_vmx_return: "
		/* Save guest registers, load host registers, keep flags */
		"mov %0, %c[wordsize](%%"R"sp) \n\t"
		"pop %0 \n\t"
		"mov %%"R"ax, %c[rax](%0) \n\t"
		"mov %%"R"bx, %c[rbx](%0) \n\t"
		"pop"Q" %c[rcx](%0) \n\t"
		"mov %%"R"dx, %c[rdx](%0) \n\t"
		"mov %%"R"si, %c[rsi](%0) \n\t"
		"mov %%"R"di, %c[rdi](%0) \n\t"
		"mov %%"R"bp, %c[rbp](%0) \n\t"
#ifdef CONFIG_X86_64
		"mov %%r8,  %c[r8](%0) \n\t"
		"mov %%r9,  %c[r9](%0) \n\t"
		"mov %%r10, %c[r10](%0) \n\t"
		"mov %%r11, %c[r11](%0) \n\t"
		"mov %%r12, %c[r12](%0) \n\t"
		"mov %%r13, %c[r13](%0) \n\t"
		"mov %%r14, %c[r14](%0) \n\t"
		"mov %%r15, %c[r15](%0) \n\t"
#endif
		"mov %%rax, %%r10 \n\t"
		"mov %%rdx, %%r11 \n\t"

		"mov %%cr2, %%"R"ax   \n\t"
		"mov %%"R"ax, %c[cr2](%0) \n\t"

		"pop  %%"R"bp; pop  %%"R"dx \n\t"
		"setbe %c[fail](%0) \n\t"

		"mov $" __stringify(__USER_DS) ", %%rax \n\t"
		"mov %%rax, %%ds \n\t"
		"mov %%rax, %%es \n\t"
	      : : "c"(vcpu), "d"((unsigned long)HOST_RSP),
		[launched]"i"(offsetof(struct vmx_vcpu, launched)),
		[fail]"i"(offsetof(struct vmx_vcpu, fail)),
		[host_rsp]"i"(offsetof(struct vmx_vcpu, host_rsp)),
		[rax]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RAX])),
		[rbx]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RBX])),
		[rcx]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RCX])),
		[rdx]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RDX])),
		[rsi]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RSI])),
		[rdi]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RDI])),
		[rbp]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_RBP])),
#ifdef CONFIG_X86_64
		[r8]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R8])),
		[r9]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R9])),
		[r10]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R10])),
		[r11]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R11])),
		[r12]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R12])),
		[r13]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R13])),
		[r14]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R14])),
		[r15]"i"(offsetof(struct vmx_vcpu, regs[VCPU_REGS_R15])),
#endif
		[cr2]"i"(offsetof(struct vmx_vcpu, cr2)),
		[wordsize]"i"(sizeof(ulong))
	      : "cc", "memory"
		, R"ax", R"bx", R"di", R"si"
#ifdef CONFIG_X86_64
		, "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
#endif
	);

	vcpu->launched = 1;

	if (unlikely(vcpu->fail)) {
		printk(KERN_ERR "vmx: failure detected (err %x)\n",
		       vmcs_read32(VM_INSTRUCTION_ERROR));
		return VMX_EXIT_REASONS_FAILED_VMENTRY;
	}

	return vmcs_read32(VM_EXIT_REASON);

#if 0
	vmx->idt_vectoring_info = vmcs_read32(IDT_VECTORING_INFO_FIELD);
	vmx_complete_atomic_exit(vmx);
	vmx_recover_nmi_blocking(vmx);
	vmx_complete_interrupts(vmx);
#endif
}


static void vmx_step_instruction(void)
{
        vmcs_writel(GUEST_RIP, vmcs_readl(GUEST_RIP) +
                               vmcs_read32(VM_EXIT_INSTRUCTION_LEN));
}

static void vmx_handle_cpuid(struct vmx_vcpu *vcpu)
{
	unsigned int eax, ebx, ecx, edx;

	eax = vcpu->regs[VCPU_REGS_RAX];
	ecx = vcpu->regs[VCPU_REGS_RCX];
	native_cpuid(&eax, &ebx, &ecx, &edx);
	vcpu->regs[VCPU_REGS_RAX] = eax;
	vcpu->regs[VCPU_REGS_RBX] = ebx;
	vcpu->regs[VCPU_REGS_RCX] = ecx;
	vcpu->regs[VCPU_REGS_RDX] = edx;
}

static int vmx_handle_nmi_exception(struct vmx_vcpu *vcpu)
{
	u32 intr_info;

	vmx_get_cpu(vcpu);
	intr_info = vmcs_read32(VM_EXIT_INTR_INFO);
	vmx_put_cpu(vcpu);

	printk(KERN_INFO "vmx: got an exception\n");
	if ((intr_info & INTR_INFO_INTR_TYPE_MASK) == INTR_TYPE_NMI_INTR)
		return 0;

	printk(KERN_ERR "vmx: unhandled nmi, intr_info %x\n", intr_info);
	vcpu->ret_code = ((EFAULT) << 8);
	return -EIO;
}


/**
 * vmx_launch - the main loop for a cloned VMX okernel process (thread)
 */
int vmx_launch(void)
{
	int ret = 0;
	unsigned long cmd;
	//int done = 0;
	unsigned long c_rip;	
	struct vmx_vcpu *vcpu;
	int schedule_ok = 0;
	unsigned long cloned_rflags;
	struct thread_info *nr_ti;
	struct thread_info *r_ti;
	unsigned int nr_irq_enabled;
	
#ifdef CONFIG_PREEMPT_RCU
	unsigned long r_rcu_read_lock_nesting;
#endif	     

#if 0
	r_preempt_count = preempt_count();
	nr_lockdep_depth = current->lockdep_depth;
#endif	
	c_rip = cloned_thread.rip;

	HDEBUG(("c_rip: (#%#lx)\n", c_rip));

	vcpu = vmx_create_vcpu();
	
	if (!vcpu)
		return -ENOMEM;
	
	printk(KERN_ERR "vmx: created VCPU (VPID %d)\n",
	       vcpu->vpid);

	if(!clone_kstack2(vcpu)){
		printk(KERN_ERR "okernel: clone kstack failed.\n");
		goto tmp_finish;
	}

	HDEBUG(("Check for held locks before  entering vmexit() handling loop:\n"));
	debug_show_all_locks();

	schedule_ok = 0;
	current->lockdep_depth_nr = 0;

	printk(KERN_ERR "R: Before vmexit handling loop: in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
	       in_atomic(), irqs_disabled(), current->pid, current->comm);
	printk(KERN_ERR "R: preempt_count (%d) rcu_preempt_depth (%d)\n",
	       preempt_count(), rcu_preempt_depth());

	while (1) {

		vmx_get_cpu(vcpu);
		local_irq_disable();

#if 0
		if (!__thread_has_fpu(current))
			math_state_restore();
#endif

#if 1
		if(schedule_ok){
			schedule_ok = 0;
			HDEBUG(("checking if resched needed...\n"));
			if (need_resched()) {
				/* should be safe to use printk here...*/
				local_irq_enable();
				vmx_put_cpu(vcpu);
				HDEBUG(("cond_resched called.\n"));
				cond_resched();
				local_irq_disable();
				vmx_get_cpu(vcpu);
				continue;
			} else {
				HDEBUG(("no resched needed.\n"));
			}
		}
#endif
#if 1
		
		if (signal_pending(current)) {
			int signr;
			siginfo_t info;

			//HDEBUG(("signal pending...\n"));
			local_irq_enable();

			vmx_put_cpu(vcpu);

			spin_lock_irq(&current->sighand->siglock);
			signr = dequeue_signal(current, &current->blocked,
					       &info);
			spin_unlock_irq(&current->sighand->siglock);
			if (!signr)
				continue;

			if (signr == SIGKILL) {
				printk(KERN_INFO "vmx: got sigkill, dying");
				vcpu->ret_code = ((ENOSYS) << 8);
				break;
			}

#if 0
			x  = DUNE_SIGNAL_INTR_BASE + signr;
			x |= INTR_INFO_VALID_MASK;
			vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, x);
			continue;
#endif
		}
#endif

		/*************************** GO FOR IT... ************************/
		ret = vmx_run_vcpu(vcpu);
                /*************************** GONE FOR IT *************************/

		//cloned_rflags = vmcs_readl(GUEST_RFLAGS);
		//if((cloned_rflags & RFLAGS_IF_BIT) ||


		if ((current->hardirqs_enabled_nr == 1)){
			local_irq_enable();
			if(!rcu_scheduler_active){
				schedule_ok = 1;
			}
		}
		
		if (ret == EXIT_REASON_VMCALL ||
		    ret == EXIT_REASON_CPUID) {
			vmx_step_instruction();
		}

		vmx_put_cpu(vcpu);
	

		/* The cloned thread may still have pre-emption
		 * disabled, so we can safely do this since it is
		 * maintained as a per-cpu variable */
		//vmx_put_cpu(vcpu);
		
		if (ret == EXIT_REASON_VMCALL){
			/* Currently we only use vmcall() in safe
			 * contexts so can printk here...*/
			cmd = vcpu->regs[VCPU_REGS_RAX];
			nr_ti = vcpu->cloned_thread_info;
			r_ti = current_thread_info();
			
			printk(KERN_ERR "R: vmcall in vmexit: (%lu) preempt_c (%d) Rsaved (%#x) NR saved (%#x)\n",
			       cmd, preempt_count(), r_ti->saved_preempt_count, nr_ti->saved_preempt_count);

			printk(KERN_ERR "R: vmcall in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
			       in_atomic(), irqs_disabled(), current->pid, current->comm);
			printk(KERN_ERR "R: preempt_count (%d) rcu_preempt_depth (%d)\n",
			       preempt_count(), rcu_preempt_depth());

			/* check for consistenncy */
			BUG_ON(irqs_disabled());

			switch(cmd){
			case VMCALL_SCHED:
				printk(KERN_ERR "R: calling schedule...\n");
				schedule_ok = 0;
				asm volatile("xchg %bx, %bx");
				schedule();
				/* Re-sync cloned-thread thread_info */
				//printk(KERN_ERR "R: syncing cloned thread_info state...\n");
				//asm volatile("xchg %bx, %bx");
				//memcpy(nr_ti, r_ti, sizeof(struct thread_info));
				printk(KERN_ERR "R: returning from schedule.\n");
				asm volatile("xchg %bx, %bx");
				continue;
#if 0
			case VMCALL_PREEMPT_SCHED:
				schedule_ok = 0;
				okernel_schedule();
				continue;
#endif
			case VMCALL_DOEXIT:
				printk(KERN_ERR "R: calling do_exit...\n");
				do_exit(0);
			default:
				printk(KERN_ERR "R: unexpected VMCALL argument.\n");
				BUG();
			}
		} else if (ret == EXIT_REASON_CPUID) {
			vmx_handle_cpuid(vcpu);
		} else if (ret == EXIT_REASON_EPT_VIOLATION) {
			goto tmp_finish;
		} else if (ret == EXIT_REASON_EXCEPTION_NMI) {
			//if (vmx_handle_nmi_exception(vcpu)){
			//	goto tmp_finish;
			//}
			goto tmp_finish;
		} else if (ret != EXIT_REASON_EXTERNAL_INTERRUPT) {
			goto tmp_finish;
		}
	}

tmp_finish:
	/* (Likely) this may (will) cause a problem if irqs were
	 * disabled / locks held, etc. in cloned thread on
	 * vmexit fault - we will have inconsistent kernel
	 * state we will need to sort out.*/
	local_irq_enable();
	//vmx_put_cpu(vcpu);

	printk(KERN_CRIT "R: leaving vmexit() loop (VPID %d) - ret (%x) - trigger BUG() for now...\n",
	       vcpu->vpid, ret);
	BUG();
	//*ret_code = vcpu->ret_code;
	//vmx_destroy_vcpu(vcpu);
	return 0;
}

	
/*-------------------------------------------------------------------------------------*/
/*  end: vmx__launch releated code                                                     */
/*-------------------------------------------------------------------------------------*/





/**
 * __vmx_enable - low-level enable of VMX mode on the current CPU
 * @vmxon_buf: an opaque buffer for use as the VMXON region
 */
static __init int __vmx_enable(struct vmcs *vmxon_buf)
{
	u64 phys_addr = __pa(vmxon_buf);
	u64 old, test_bits;

	printk(KERN_ERR "okernel: __vmx_enable 0.\n");
	
	if (native_read_cr4() & X86_CR4_VMXE)
		return -EBUSY;

	printk(KERN_ERR "okernel: __vmx_enable 1.\n");


	rdmsrl(MSR_IA32_FEATURE_CONTROL, old);
#if 0
	if(old & FEATURE_CONTROL_LOCKED){
		if(!(old & FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX)){
			printk(KERN_ERR "okernel: __vmx_enable vxmon disabled by FW.\n");
			return -1;
		}
	} else { /* try enable since feature not locked */
		printk(KERN_ERR "okernel __vmx_enable trying to enable VMXON.\n");
		old |= FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;
		old |= FEATURE_CONTROL_LOCKED;
		wrmsrl(MSR_IA32_FEATURE_CONTROL, old);
		rdmsrl(MSR_IA32_FEATURE_CONTROL, old);

		if(!(old & FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX)){
			printk(KERN_ERR "okernel: __vmx_enable failed to enable VXMON.\n");
			return -1;
		}
		printk(KERN_ERR "okernel __vmx_enable VMXON enabled.\n");
	}
#endif
#if 1
	test_bits = FEATURE_CONTROL_LOCKED;
	test_bits |= FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;

	/*
	if (tboot_enabled())
		test_bits |= FEATURE_CONTROL_VMXON_ENABLED_INSIDE_SMX;
	*/
	if ((old & test_bits) != test_bits) {
		/* enable and lock */
		printk(KERN_ERR "okernel: VMX_FEATURE_CONTROL NOT ENABLED - fixing...\n");
		wrmsrl(MSR_IA32_FEATURE_CONTROL, old | test_bits);
	}
#endif

	printk(KERN_DEBUG "okernel __vmx_enable: 2.\n");
	cr4_set_bits(X86_CR4_VMXE);

	printk(KERN_ERR "okernel: __vmx_enable 3.\n");
	__vmxon(phys_addr);
	printk(KERN_ERR "okernel: __vmx_enable 4 physaddr (%#lx)\n", (unsigned long)phys_addr);
	
	vpid_sync_vcpu_global();
	ept_sync_global();

	return 0;
}



/**
 * vmx_enable - enables VMX mode on the current CPU
 * @unused: not used (required for on_each_cpu())
 *
 * Sets up necessary state for enable (e.g. a scratchpad for VMXON.)
 */
static __init void vmx_enable(void *unused)
{
	int ret;
	struct vmcs *vmxon_buf = __this_cpu_read(vmxarea);

	if ((ret = __vmx_enable(vmxon_buf)))
		goto failed;

	__this_cpu_write(vmx_enabled, 1);
	native_store_gdt(this_cpu_ptr(&host_gdt));

	printk(KERN_INFO "vmx: VMX enabled on CPU %d\n",
	       raw_smp_processor_id());
	return;

failed:
	atomic_inc(&vmx_enable_failed);
	printk(KERN_ERR "vmx: failed to enable VMX, err = %d\n", ret);
}

/**
 * vmx_disable - disables VMX mode on the current CPU
 */
static void vmx_disable(void *unused)
{
	if (__this_cpu_read(vmx_enabled)) {
		__vmxoff();
		cr4_clear_bits(X86_CR4_VMXE);
		__this_cpu_write(vmx_enabled, 0);
	}
}

/**
 * vmx_free_vmxon_areas - cleanup helper function to free all VMXON buffers
 */
static void vmx_free_vmxon_areas(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (per_cpu(vmxarea, cpu)) {
			vmx_free_vmcs(per_cpu(vmxarea, cpu));
			per_cpu(vmxarea, cpu) = NULL;
		}
	}
}


int __init vmx_init(void)
{
	int r, cpu;
	
        if (!cpu_has_vmx()) {
		printk(KERN_ERR "vmx: CPU does not support VT-x\n");
		return -EIO;
	}

	printk(KERN_ERR "okernel: vmx_init 0.\n");
	
	if (setup_vmcs_config(&vmcs_config) < 0)
		return -EIO;
	
	if (!cpu_has_vmx_vpid()) {
		printk(KERN_ERR "vmx: CPU is missing required feature 'VPID'\n");
		return -EIO;
	}

	if (!cpu_has_vmx_ept()) {
		printk(KERN_ERR "vmx: CPU is missing required feature 'EPT'\n");
		return -EIO;
	}

	if (!vmx_capability.has_load_efer) {
		printk(KERN_ERR "vmx: ability to load EFER register is required\n");
		return -EIO;
	}
	
	msr_bitmap = (unsigned long *)__get_free_page(GFP_KERNEL);
        if (!msr_bitmap) {
                return -ENOMEM;
        }
        /* FIXME: do we need APIC virtualization (flexpriority?) */
	/* cid: Neeed to look at this */ 
#if 1
	memset(msr_bitmap, 0x0, PAGE_SIZE);
        //__vmx_disable_intercept_for_msr(msr_bitmap, MSR_FS_BASE);
        //__vmx_disable_intercept_for_msr(msr_bitmap, MSR_GS_BASE);

        set_bit(0, vmx_vpid_bitmap); /* 0 is reserved for host */
#endif
	printk(KERN_ERR "okernel: vmx_init 1.\n");
 
	for_each_possible_cpu(cpu) {
		struct vmcs *vmxon_buf;

		vmxon_buf = __vmx_alloc_vmcs(cpu);
		if (!vmxon_buf) {
			vmx_free_vmxon_areas();
			return -ENOMEM;
		}

		per_cpu(vmxarea, cpu) = vmxon_buf;
	}

	atomic_set(&vmx_enable_failed, 0);
	if (on_each_cpu(vmx_enable, NULL, 1)) {
		printk(KERN_ERR "vmx: timeout waiting for VMX mode enable.\n");
		r = -EIO;
		goto failed1; /* sadly we can't totally recover */
	}

	if (atomic_read(&vmx_enable_failed)) {
		r = -EBUSY;
		goto failed2;
	}
	
	in_vmx_nr_mode = real_in_vmx_nr_mode;
	
        return 0;

	
failed2:
	on_each_cpu(vmx_disable, NULL, 1);
failed1:
	vmx_free_vmxon_areas();
	return r;
}

EXPORT_SYMBOL(vmcall);

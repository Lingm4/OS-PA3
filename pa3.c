/**********************************************************************
 * Copyright (c) 2020-2023
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "list_head.h"
#include "vm.h"

/**
 * Ready queue of the system
 */
extern struct list_head processes;

/**
 * Currently running process
 */
extern struct process *current;

/**
 * Page Table Base Register that MMU will walk through for address translation
 */
extern struct pagetable *ptbr;

/**
 * TLB of the system.
 */
extern struct tlb_entry tlb[1UL << (PTES_PER_PAGE_SHIFT * 2)];


/**
 * The number of mappings for each page frame. Can be used to determine how
 * many processes are using the page frames.
 */
extern unsigned int mapcounts[];


/**
 * lookup_tlb(@vpn, @rw, @pfn)
 *
 * DESCRIPTION
 *   Translate @vpn of the current process through TLB. DO NOT make your own
 *   data structure for TLB, but should use the defined @tlb data structure
 *   to translate. If the requested VPN exists in the TLB and it has the same
 *   rw flag, return true with @pfn is set to its PFN. Otherwise, return false.
 *   The framework calls this function when needed, so do not call
 *   this function manually.
 *
 * RETURN
 *   Return true if the translation is cached in the TLB.
 *   Return false otherwise
 */

bool lookup_tlb(unsigned int vpn, unsigned int rw, unsigned int *pfn)
{
	/*
	for(int i = 0; i < 10; i++)
		printf("%d%d%d ", tlb[i].valid, tlb[i].vpn, tlb[i].rw);
	printf("\n");
	*/
	
	for(int i = 0; i < NR_TLB_ENTRIES; i++){
		if(tlb[i].vpn == vpn && tlb[i].rw & rw == rw && tlb[i].valid){
			*pfn = tlb[i].pfn;
			return true;
		}
	}
	return false;
}


/**
 * insert_tlb(@vpn, @rw, @pfn)
 *
 * DESCRIPTION
 *   Insert the mapping from @vpn to @pfn for @rw into the TLB. The framework will
 *   call this function when required, so no need to call this function manually.
 *   Note that if there exists an entry for @vpn already, just update it accordingly
 *   rather than removing it or creating a new entry.
 *   Also, in the current simulator, TLB is big enough to cache all the entries of
 *   the current page table, so don't worry about TLB entry eviction. ;-)
 */
void insert_tlb(unsigned int vpn, unsigned int rw, unsigned int pfn)
{
	int target_tlb_idx = 0;
	for(target_tlb_idx = 0; tlb[target_tlb_idx].valid; target_tlb_idx++);
	for(int i = 0; i < NR_TLB_ENTRIES; i++){
		if(tlb[i].valid && tlb[i].vpn == vpn){
			target_tlb_idx = i;
			break;
		}
	}
	
	tlb[target_tlb_idx].valid = true;
	tlb[target_tlb_idx].vpn = vpn;
	tlb[target_tlb_idx].rw = rw;
	tlb[target_tlb_idx].pfn = pfn;
}


/**
 * alloc_page(@vpn, @rw)
 *
 * DESCRIPTION
 *   Allocate a page frame that is not allocated to any process, and map it
 *   to @vpn. When the system has multiple free pages, this function should
 *   allocate the page frame with the **smallest pfn**.
 *   You may construct the page table of the @current process. When the page
 *   is allocated with ACCESS_WRITE flag, the page may be later accessed for writes.
 *   However, the pages populated with ACCESS_READ should not be accessible with
 *   ACCESS_WRITE accesses.
 *
 * RETURN
 *   Return allocated page frame number.
 *   Return -1 if all page frames are allocated.
 */
unsigned int alloc_page(unsigned int vpn, unsigned int rw)
{
	int pfn = 0;
	for(pfn = 0; pfn < NR_PAGEFRAMES; pfn++){
		if(!mapcounts[pfn]) break;
	}
	
	if(pfn < NR_PAGEFRAMES){
		mapcounts[pfn]++;
		struct pte *target_pte = NULL;
		int vpn1 = vpn >> PTES_PER_PAGE_SHIFT;
		int vpn2 = vpn % (1 << PTES_PER_PAGE_SHIFT);
		if(ptbr->outer_ptes[vpn1] == NULL){
			ptbr->outer_ptes[vpn1] = (struct pte_directory *)malloc(sizeof(struct pte_directory));
			for(int i = 0; i < NR_PTES_PER_PAGE; i++){
				ptbr->outer_ptes[vpn1]->ptes[i].valid = false;
			}
			target_pte = &(ptbr->outer_ptes[vpn1]->ptes[vpn2]);
		}else{
			target_pte = &(ptbr->outer_ptes[vpn1]->ptes[vpn2]);
		}
		target_pte->valid = true;
		target_pte->rw = rw;
		target_pte->pfn = pfn;
		target_pte->private = 0;
		return pfn;
	}
	else return -1;
}


/**
 * free_page(@vpn)
 *
 * DESCRIPTION
 *   Deallocate the page from the current processor. Make sure that the fields
 *   for the corresponding PTE (valid, rw, pfn) is set @false or 0.
 *   Also, consider the case when a page is shared by two processes,
 *   and one process is about to free the page. Also, think about TLB as well ;-)
 */
void free_page(unsigned int vpn)
{
	int vpn1 = vpn >> PTES_PER_PAGE_SHIFT;
	int vpn2 = vpn % (1 << PTES_PER_PAGE_SHIFT);
	struct pte *target_pte = &(ptbr->outer_ptes[vpn1]->ptes[vpn2]);
	int pfn = target_pte->pfn;
	mapcounts[pfn]--;

	//modify pagetable
	target_pte->valid = false;
	bool is_ptes_empty = true;
	for(int i = 0; i < NR_PTES_PER_PAGE; i++){
		if(ptbr->outer_ptes[vpn1]->ptes[i].valid) is_ptes_empty = false;
	}
	if(is_ptes_empty) free(ptbr->outer_ptes[vpn1]);

	//modify tlb
	for(int i = 0; i < NR_TLB_ENTRIES; i++){
		if(tlb[i].vpn == vpn && mapcounts[pfn] == 0){
			tlb[i].valid = false;
		}
	}
}


/**
 * handle_page_fault()
 *
 * DESCRIPTION
 *   Handle the page fault for accessing @vpn for @rw. This function is called
 *   by the framework when the __translate() for @vpn fails. This implies;
 *   0. page directory is invalid
 *   1. pte is invalid
 *   2. pte is not writable but @rw is for write
 *   This function should identify the situation, and do the copy-on-write if
 *   necessary.
 *
 * RETURN
 *   @true on successful fault handling
 *   @false otherwise
 */
bool handle_page_fault(unsigned int vpn, unsigned int rw)
{
	int vpn1 = vpn >> PTES_PER_PAGE_SHIFT;
	int vpn2 = vpn % (1 << PTES_PER_PAGE_SHIFT);

	if(!ptbr->outer_ptes[vpn1]){
		return true;
	}else if(!ptbr->outer_ptes[vpn1]->ptes[vpn2].valid){
		return true;
	}
	
	struct pte *pte = &ptbr->outer_ptes[vpn1]->ptes[vpn2];
	if(!(pte->rw & rw) && pte->private & rw){
		
		pte->rw = pte->private;
		pte->private = 0;
		
		if(mapcounts[pte->pfn] > 1){
			printf("copy on write\n");
			mapcounts[pte->pfn]--;
			int pfn = alloc_page(vpn, pte->rw);
			for(int i = 0; i < NR_TLB_ENTRIES; i++)
				if(tlb[i].valid && tlb[i].vpn == vpn)
					tlb[i].pfn = pfn;
		}

		for(int i = 0; i < NR_TLB_ENTRIES; i++)
			if(tlb[i].valid && tlb[i].vpn == vpn)
				tlb[i].rw = rw;
		return true;
	}
	return false;
}


/**
 * switch_process()
 *
 * DESCRIPTION
 *   If there is a process with @pid in @processes, switch to the process.
 *   The @current process at the moment should be put into the @processes
 *   list, and @current should be replaced to the requested process.
 *   Make sure that the next process is unlinked from the @processes, and
 *   @ptbr is set properly.
 *
 *   If there is no process with @pid in the @processes list, fork a process
 *   from the @current. This implies the forked child process should have
 *   the identical page table entry 'values' to its parent's (i.e., @current)
 *   page table. 
 *   To implement the copy-on-write feature, you should manipulate the writable
 *   bit in PTE and mapcounts for shared pages. You may use pte->private for 
 *   storing some useful information :-)
 */
void switch_process(unsigned int pid)
{
	struct process *next_process = NULL;
	struct process *pos;
	
	//find process
	//printf("find process\n");
	list_for_each_entry(pos, &processes, list){
		if(pos->pid == pid){
			next_process = pos;
			break;
		}
	}

	//fork
	if(!next_process){
		
		//printf("fork\n");
		//make new process
		printf("make new process\n");
		next_process = (struct process *)malloc(sizeof(struct process));
		next_process->pid = pid;
		INIT_LIST_HEAD(&next_process->list);

		//copy pagetable
		//printf("copy pagetable\n");
		for(int i = 0; i < NR_PTES_PER_PAGE; i++){
			if(current->pagetable.outer_ptes[i] == NULL)
				next_process->pagetable.outer_ptes[i] = NULL;
			else{
				next_process->pagetable.outer_ptes[i] = (struct pte_directory *)malloc(sizeof(struct pte_directory));
				for(int j = 0; j < NR_PTES_PER_PAGE; j++){
					//modify rw and backup to private
					if(current->pagetable.outer_ptes[i]->ptes[j].rw & ACCESS_WRITE){
						current->pagetable.outer_ptes[i]->ptes[j].private = current->pagetable.outer_ptes[i]->ptes[j].rw;
						current->pagetable.outer_ptes[i]->ptes[j].rw = ACCESS_READ;
					}
					next_process->pagetable.outer_ptes[i]->ptes[j] = current->pagetable.outer_ptes[i]->ptes[j];
				}
			}
		}

		//modify mapcounts
		//printf("modify mapcounts\n");
		struct pagetable *pt = &next_process->pagetable;
		for(int i = 0; i < NR_PTES_PER_PAGE; i++){
			
			if(!pt->outer_ptes[i]) continue;
			
			//printf("modify mapcounts in outer_ptes[%d]\n", i);
			for(int j = 0; j < NR_PTES_PER_PAGE; j++){
				if(pt->outer_ptes[i]->ptes[j].valid)
					mapcounts[pt->outer_ptes[i]->ptes[j].pfn]++;
			}
		}
	}

	//switch
	//printf("switch\n");
	list_add_tail(&current->list, &processes);
	current = next_process;
	ptbr = &next_process->pagetable;

	//flush tlb
	for(int i = 0; i < NR_TLB_ENTRIES; i++)
		tlb[i].valid = false;
}

/*
 * Inter-VM Communication by use of Shared Memory in Xen.
 *
 * Back End Split Driver implementing the read operation in 
 * accordance with Front End Split Driver. Drivers can be used in 
 * all Xen Domains. 
 *	
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <xen/interface/grant_table.h>
#include <xen/interface/io/blkif.h>
#include <xen/interface/xen.h>
#include <xen/interface/io/ring.h> 
#include <asm/xen/hypercall.h>
#include <asm/hypervisor.h>
//#include <config/sys/hypervisor.h>
#include <xen/grant_table.h>
#include <asm/pgtable.h>
#include <asm/sync_bitops.h>
#include <asm/uaccess.h>
//#include <asm/io.h>
#include <xen/evtchn.h>
#include <xen/page.h>
#include <xen/xen.h>
//#include <linux/proc_fs.h>

/* Domain Id of Back End Split Driver */
#define DOM_SELF 8
/* Domain Id of Front End Split Driver */
#define DOM_TGT 9
/* This is the control gntref given by xen-front */
#define CTRL_GREF 843

/* 
 * Number of Control & Data Pages in Front
 * & Back End Drivers needs to be synchronised
 *
 */
#define N_CTRL_PAGES 1
#define N_DATA_PAGES 8

/* Structure for storing Information */
struct back_info 	{
	grant_ref_t gref;
	domid_t domid_self;
	domid_t domid_tgt;
	int n_cpages;
	int n_dpages;
}	b_info;

/* Stores Data Pages' Information */
struct back_data_info {
	pte_t *pte;
	struct vm_struct *vm;
	int n_dpages;
	grant_ref_t gref[N_DATA_PAGES];
	grant_handle_t handle[N_DATA_PAGES];
}	d_info;

/* Stored Control page's Information */
struct back_ctrl_info {
	pte_t *pte;
	struct vm_struct *vm;
	grant_ref_t gref;
	grant_handle_t handle[0];
}	c_info;

/* Data written onto Control Page */
struct control_page_info {
	/* Number of Data Pages */
	int n_dpages;
	/* Grant references for data pages */
	grant_ref_t gref[N_DATA_PAGES];
	/* Currently, static page mapping */
	int mapping_type;
}	*ctrl_info;


int gref;
/* insmod value param -- not tested --*/
module_param(gref, int, 0644);
/* Grant handle for Data Pages */
grant_handle_t handle[N_DATA_PAGES];


/* Free Pages where npages is in order */
void free_npages(void *addr, int npages)
{
	free_pages((unsigned long)addr, npages);
}

/* Freeing control page */
void free_cpages(struct back_ctrl_info c_info)
{
	free_npages(c_info.vm, 0);
}

/* Freeing Data Pages */
void free_dpages(struct back_data_info d_info)
{
	free_npages(d_info.vm, 3);
}


/* unmapping control page's grants */
int ivc_unmap_cgrant(struct back_ctrl_info c_info, int flags)
{
	struct gnttab_unmap_grant_ref op;

	gnttab_set_unmap_op(&op, (unsigned long)c_info.vm->addr, flags,
							 c_info.handle[0]);

		
	BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1));

	if (op.status != GNTST_okay)
		printk(KERN_INFO "(xen-back): unmap grant failed (err_code %d)"
								, op.status);

	return op.status;
}


/* unmapping data pages' grants */
int ivc_unmap_dgrant(struct back_data_info d_info, int flags, int frame)
{
	struct gnttab_unmap_grant_ref op;

	gnttab_set_unmap_op(&op, (unsigned long)(d_info.vm->addr + 
					(frame * PAGE_SIZE)), flags, handle[frame]);

		
	BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1));

	if (op.status != GNTST_okay)
		printk(KERN_INFO "(xen-back): unmap grant failed (err_code %d)"
								, op.status);

	return op.status;
}


/* mapping control page grants */
int ivc_map_cgrant(struct back_info b_info, struct back_ctrl_info c_info,
								int flags)
{
	struct gnttab_map_grant_ref op;
	
	gnttab_set_map_op(&op, (unsigned long)c_info.vm->addr, flags,
			  c_info.gref, b_info.domid_tgt);

	BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1));

	if (op.status != GNTST_okay) {
		printk(KERN_INFO "(xen-back): map cgrant failed (err_code %d)",
								 op.status);
	}
	else {
		*c_info.handle = op.handle;
	}

	return op.status;
}

/* Mapping data pages' grants */
int ivc_map_dgrant(struct back_info b_info, int frame, int flags)
{
	struct gnttab_map_grant_ref op;
	
	gnttab_set_map_op(&op, (unsigned long)(d_info.vm->addr + 
					(frame * PAGE_SIZE)), flags,
			  	d_info.gref[frame], b_info.domid_tgt);

	BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1));

	if (op.status != GNTST_okay) {
		printk(KERN_INFO "(xen-back): map dgrant failed (err_code %d)",
								 op.status);
	}
	else {
		handle[frame] = op.handle;
	}

	return op.status;
}


/* Test print cpages */
void test_print_cpage(struct back_ctrl_info c_info, int bytes)
{
	int i;	
 	printk(KERN_INFO "Bytes in cpage ");
	for(i = 0; i < bytes; i++)
	{
    	printk("%c", ((char*)(c_info.vm->addr))[i]);
    }
}


/* Test print dpages */
void test_print_dpage(struct back_data_info d_info, int bytes)
{
	int i, j;	
 	printk(KERN_INFO "Bytes in dpage ");

 	for (j = 0; j < b_info.n_dpages; j++)
 	{	
		for(i = 0; i < bytes; i++)
		{
	    	printk("%c", ((char*)(d_info.vm->addr + (j * PAGE_SIZE)))[i]);
	    }

 	}

 }


/* Filling from control pages */
void fill_from_cpages(struct control_page_info *ctrl_info)
{
	int i;

	for (i = 0; i < ctrl_info->n_dpages; i++) {
		d_info.gref[i] = ctrl_info->gref[i];
	}

	b_info.n_dpages = ctrl_info->n_dpages;
	d_info.n_dpages = ctrl_info->n_dpages;
}


/* Initializing Control Pages */
static int init_cpages(void)
{
	int ret;
	c_info.vm = alloc_vm_area(PAGE_SIZE, &c_info.pte);

	if (!c_info.vm)	{
         free_vm_area(c_info.vm);
         printk(KERN_INFO "(xen-back): could not get free cpage(s)");
         return -ENOMEM;
    }

	b_info.n_cpages = N_CTRL_PAGES;

    	c_info.gref = CTRL_GREF;
    	ret = ivc_map_cgrant(b_info, c_info, GNTMAP_host_map);

	if (ret < 0)
		goto out;

//	test_print_cpage(c_info, 23);

	ctrl_info = kmalloc(sizeof(struct control_page_info), GFP_KERNEL);

	if (!ctrl_info) {
		printk(KERN_INFO "(xen-back): could not get free ctrl_page");
		ret = -ENOMEM;
		goto out;
	}
	
	memcpy(ctrl_info, (struct control_page_info *) c_info.vm->addr, 
					sizeof(struct control_page_info));

	return 0;

out:
	return ret;
}


/* Initializing Data Pages */
static int init_dpages(void)
{
	int ret = 0, frame, i;

	d_info.vm = alloc_vm_area(PAGE_SIZE * b_info.n_dpages, &d_info.pte);

//	printk(KERN_INFO "PRINTING ADDRESS %p", d_info.vm);
//	printk(KERN_INFO "PRINTING ADDRESS in ld %lu", (unsigned long)d_info.vm->addr);

	if (!d_info.vm)	{
         free_vm_area(d_info.vm);
         printk(KERN_INFO "(xen-back): could not get free dpage(s)");
         return -ENOMEM;
    }


	d_info.n_dpages = ctrl_info->n_dpages;

	for (i = 0; i < ctrl_info->n_dpages; i++) {
		d_info.gref[i] = ctrl_info->gref[i];
	}


    for(frame = 0; frame < d_info.n_dpages; frame++)
    {
	    ret = ivc_map_dgrant(b_info, frame, GNTMAP_host_map);
	    if (ret < 0) {
	    	goto out;   
	    }
	}

//	test_print_dpage(d_info, 23);

out:
	return ret;
}


/* Initializer : Module Xen-IVC */
static int __init xen_ivc_init(void)
{
	int ret, out;

	b_info.domid_tgt = DOM_TGT;
	b_info.domid_self = DOM_SELF;
	b_info.n_dpages = N_DATA_PAGES;
	d_info.n_dpages = N_DATA_PAGES;

	ret = init_cpages();
	if (ret < 0)
		goto undo_cpages;

	ret = init_dpages();
	if (ret < 0)
		goto undo_dpages;

	printk(KERN_INFO "(xen-back): module loaded into kernel");
	goto out;

undo_dpages:
	
//	free_dpages(d_info);

undo_cpages:

	if (ctrl_info)
		kfree(ctrl_info);

	free_cpages(c_info);

	out = ivc_unmap_cgrant(c_info, GNTMAP_host_map);

out:
	return ret;
}


/* Cleanup : Module Xen-IVC */
static void __exit xen_ivc_exit(void)
{

	int ret, i;

	ret = ivc_unmap_cgrant(c_info, GNTMAP_host_map);
	if (ret != 0)
		printk(KERN_INFO "(xen-back): unmapping shared cframe failed");

//	printk(KERN_INFO "(xen-back): vm->addr %lu", (unsigned long) d_info.vm->addr);


	for (i = 0; i < b_info.n_dpages; i++)
	{
		ret = ivc_unmap_dgrant(d_info, GNTMAP_host_map, i);
		if (ret != 0)
			printk(KERN_INFO "(xen-back): unmapping shared dframe failed");
	}

	if (ctrl_info)
			kfree(ctrl_info);

	if (c_info.vm)
		free_vm_area(c_info.vm);

	if (d_info.vm)
		free_vm_area(d_info.vm);

	printk(KERN_INFO "(xen-back): module unloaded from kernel");

}


module_init(xen_ivc_init);
module_exit(xen_ivc_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richy Gerard <rdevasahayam@cs.stonybrook.edu>");
MODULE_DESCRIPTION("Xen Inter-VM Shared Memory Communication");

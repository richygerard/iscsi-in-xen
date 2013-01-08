/*
 * Inter-VM Communication by use of Shared Memory in Xen.
 *
 * Front End Split Driver implementing the read operation in 
 * accordance with Back End Split Driver. Drivers can be used in 
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
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/blkdev.h> 
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <asm/pgtable.h>
#include <asm/sync_bitops.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/xen/hypercall.h>
#include <asm/setup.h>
#include <asm/pgalloc.h>
#include <asm/hypervisor.h>
#include <asm/xen/page.h>
#include <xen/evtchn.h>
#include <xen/page.h>
#include <xen/xenbus.h>
#include <xen/interface/io/ring.h>
#include <xen/interface/io/blkif.h>
#include <xen/xen.h>
#include <xen/events.h>
#include <xen/interface/xen.h>
#include <xen/grant_table.h>
#include <xen/interface/memory.h>

/* Domain Id of Front End Split Driver */
#define DOM_SELF 9
/* Domain Id of Back End Split Driver */
#define DOM_TGT 8

/* Number of Control Pages - need to be synced xen-back when used */
#define N_CTRL_PAGES 1
/* Number of Data Pages - need to be synced with xen-back when used*/
#define N_DATA_PAGES 8

/* Structure for storing Dom related stuff */
struct front_info 	{
	/* Self DomId */
	domid_t domid_self;
	/* Target Domain */
	domid_t domid_tgt;
	/* Number of Control Pages */
	int n_cpage;
	/* Number of Data Pages */
	int n_dpages;
}	f_info;

/* Storing Data Info in Front End */
struct front_data_info {
	/* Data Pointer */
	void *vm;
	/* Machine Frame Numbers */
	long mfn[N_DATA_PAGES];
	int n_dpages;
	/* Grant Handles */
	grant_handle_t handle[N_DATA_PAGES];
	/* Grant References */
	grant_ref_t gref[N_DATA_PAGES];
}	d_info;

/* Storing Control Info in Front End */
struct front_ctrl_info {
	/* Control Data Pointer */
	void *vm;
	/* Machine Frame Number */
	long mfn;
	/* Grant Reference */
	grant_ref_t gref;
}	c_info;

/* Control Page Information to be written to Shared Page */
struct control_page_info {
	/* Number of Data Pages */
	int n_dpages;
	/* Grant references for data pages */
	grant_ref_t gref[N_DATA_PAGES];
	/* Currently, static page mapping */
	int mapping_type;
}	ctrl_info;



/* Free Pages where npages is in order */
void free_npages(void *addr, int npages)
{
	free_pages((unsigned long)addr, npages);
}


/* Freeing Control Page */
void free_cpages(struct front_ctrl_info c_info)
{
	free_npages(c_info.vm, 0);
}


/* Freeing Data Pages */
void free_dpages(struct front_data_info d_info)
{
	free_npages(d_info.vm, 3);
}


/* Test write onto control page */
void test_write_cpage(struct front_ctrl_info c_info, int bytes)
{
 	printk(KERN_INFO "(xen-front): writing bytes to cpage");
 	memcpy((char*)c_info.vm, "TEST TEST TEST TEST TES", bytes);
}


/* Test write onto data page */
void test_write_dpage(struct front_data_info d_info, int bytes)
{
 	printk(KERN_INFO "(xen-front): writing bytes to dpage");
 	memcpy((char*)d_info.vm, "TEST TEST TEST TEST TES", bytes);
}


/* Granting control page */
int ivc_grant_cpages(struct front_info f_info, struct front_ctrl_info c_info)
{
	int ret = 0;

	ret = gnttab_grant_foreign_access(f_info.domid_tgt, c_info.mfn, 0);

	if (ret < 0){
		printk(KERN_INFO "(xen-front): failed granting access to page");
		goto err;
	}

	printk(KERN_INFO "(xen-front): grant reference (gnt_cpage) is %d", ret);

err:
	return ret;
}


/* Granting data pages */
int ivc_grant_dpages(struct front_info f_info, struct front_data_info d_info, 
																int frame)
{
	int ret = 0;

	ret = gnttab_grant_foreign_access(f_info.domid_tgt, d_info.mfn[frame],
									 0);

	if (ret < 0){
		printk(KERN_INFO "(xen-front): failed grant access to frame");
		goto err;
	}

	d_info.gref[frame] = ret;

err:
	return ret;
}


/* Ungranting control page */
int ivc_ungrant_cpages(struct front_ctrl_info c_info)
{
	int ret;

	if (unlikely(gnttab_query_foreign_access(c_info.gref) != 0))	{
		printk(KERN_INFO "(xen-front): remote domain still using grant \
							 %d", c_info.gref);
		ret = -1;
	}
	
	gnttab_end_foreign_access(c_info.gref, 0, (unsigned long)c_info.vm);

	return ret;
}


/* Ungranting data pages */
int ivc_ungrant_dpages(struct front_data_info d_info, int frame, int all)
{
	int ret;
	if (all == 1)
	{
		for (frame = 0; frame < d_info.n_dpages; frame++)
		{
			if (unlikely(gnttab_query_foreign_access
						(d_info.gref[frame]) != 0)) {
				printk(KERN_INFO "(xen-front): remote domain \
				still using grant %d", d_info.gref[frame]);
				ret = -1;
			}

			gnttab_end_foreign_access(d_info.gref[frame], 0, (unsigned long)
								d_info.vm + (frame * PAGE_SIZE));
		}
	}
	else {
		if (unlikely(gnttab_query_foreign_access
						(d_info.gref[frame]) != 0)) {
			printk(KERN_INFO "(xen-front): remote domain still \
					using grant %d", d_info.gref[frame]);
			ret = -1;
		}

		gnttab_end_foreign_access(d_info.gref[frame], 0, (unsigned long)
								d_info.vm + (frame * PAGE_SIZE));
	}
	
	return ret;
}


/* Initializing Control pages */
static int init_cpages(void)
{
	int ret = 0;
	c_info.vm = kmalloc (PAGE_SIZE, GFP_KERNEL);

	if (!c_info.vm)	{
		printk(KERN_INFO "(xen-front): could not get free page(s)");
		ret = -ENOMEM;
		goto out;
	}

	f_info.n_cpage = N_CTRL_PAGES;
	c_info.mfn = virt_to_mfn(c_info.vm);

 //   	test_write_cpage(c_info, 23);

	c_info.gref = ivc_grant_cpages(f_info, c_info);


    	if (c_info.gref < 0)	{
		free_cpages(c_info);
		ret = c_info.gref;
    		goto out;
    	}
    
    return 0;

out:

    return ret;
}


/* Initializing Data pages */
static int init_dpages(void)
{
	
	void *vm;
	int ret, i, j;

	d_info.vm = (void *) __get_free_pages(GFP_KERNEL, 3);

	if (!d_info.vm)	{
		printk(KERN_INFO "(xen-front): could not get free page(s)");
		return -ENOMEM;
	}

	f_info.n_dpages = N_DATA_PAGES;
	d_info.n_dpages = N_DATA_PAGES;

	vm = (void *)d_info.vm;

	/* Getting grant references for the pages */
	for (i = 0; i < f_info.n_dpages; i++)
	{
		/* Getting the machine frame numbers for the pages*/
		d_info.mfn[i] = virt_to_mfn(vm);

//   		test_write_dpage(d_info, 23);

		d_info.gref[i] = ivc_grant_dpages(f_info, d_info, i);

		if (d_info.gref[i] < 0)	{
	    	goto out;
	    }

		vm = vm + PAGE_SIZE;

	}

    return 0;

out:

	for (j = 0; j < i; j++) {
		ivc_ungrant_dpages(d_info, j, 0);
	}

	free_dpages(d_info);

    return ret;
}


/* Writing the control page */
static void write_cpages(void)
{
	int i;

	/* static page mapping between vms */
	ctrl_info.mapping_type = 1;
	ctrl_info.n_dpages = f_info.n_dpages;

	printk(KERN_INFO "(xen-front): writing grants to cpage(s)");

	for (i = 0; i < ctrl_info.n_dpages; i++)
		ctrl_info.gref[i] = d_info.gref[i];

 	memcpy(c_info.vm, (void *)&ctrl_info, sizeof (struct control_page_info));

}


/* Initializer : Module Xen-IVC */
static int __init xen_ivc_init(void)
{
	int ret, out;

	f_info.domid_tgt = DOM_TGT;
	f_info.domid_self = DOM_SELF;

	/* Initialize cpages */
	ret = init_cpages();
	if (ret < 0)
		goto undo_cpages;

	/* Initialize dpages */
	ret = init_dpages();
	if (ret < 0)
		goto undo_dpages;

	/* Writing cpages */
	write_cpages();

	printk(KERN_INFO "(xen-front): module loaded into kernel");

	return 0;

undo_dpages:
	
	out = ivc_ungrant_dpages(d_info, 0, 1);

	free_dpages(d_info);

undo_cpages:
	
	out = ivc_ungrant_cpages(c_info);

	free_cpages(c_info);

	return ret;
}


/* Cleanup : Module Xen-IVC */
static void __exit xen_ivc_exit(void)
{
	int ret;

	/* Ungranting the control pages */
	ret = ivc_ungrant_cpages(c_info);

	/* Ungranting the data pages */
	ret = ivc_ungrant_dpages(d_info, 0, 1);

	printk(KERN_INFO "(xen-front): module unloaded from kernel");
}


module_init(xen_ivc_init);
module_exit(xen_ivc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richy Gerard <rdevasahayam@cs.stonybrook.edu>");
MODULE_DESCRIPTION("Xen Inter-VM Shared Memory Communication");

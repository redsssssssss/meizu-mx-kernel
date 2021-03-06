/* drivers/media/video/samsung/fimg2d3x/fimg2d_core.c
 *
 * Copyright  2010 Samsung Electronics Co, Ltd. All Rights Reserved. 
 *		      http://www.samsungsemi.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This file implements fimg2d core functions.
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <plat/sysmmu.h>
#include <linux/sched.h>

#include <linux/dma-mapping.h>
#ifdef CONFIG_VIDEO_SAMSUNG_MEMSIZE_FIMG2D
#if defined(CONFIG_S5P_MEM_CMA)
#include <linux/cma.h>
#elif defined(CONFIG_S5P_MEM_BOOTMEM)
#include <mach/media.h>
#include <plat/media.h>
#endif
#endif

#include "fimg2d.h"

int g2d_clk_enable(struct g2d_global *g2d_dev)
{
	if(!atomic_read(&g2d_dev->clk_enable_flag)) {
		clk_enable(g2d_dev->clock);
		atomic_set(&g2d_dev->clk_enable_flag, 1);
		return 0;
	}
	return -1;
}

int g2d_clk_disable(struct g2d_global *g2d_dev)
{
	if(atomic_read(&g2d_dev->clk_enable_flag)) {
		if(atomic_read(&g2d_dev->in_use) == 0) {
			clk_disable(g2d_dev->clock);
			atomic_set(&g2d_dev->clk_enable_flag, 0);
			return 0;
		} 
	}
	return -1;
}
void g2d_sysmmu_on(struct g2d_global *g2d_dev)
{
	exynos_sysmmu_enable(g2d_dev->dev, __pa(swapper_pg_dir));
}

void g2d_sysmmu_off(struct g2d_global *g2d_dev)
{
	exynos_sysmmu_disable(g2d_dev->dev);
}

void g2d_sysmmu_set_pgd(struct g2d_global *g2d_dev, u32 pgd)
{
	exynos_sysmmu_set_ptbase(g2d_dev->dev, pgd);
}

void g2d_fail_debug(g2d_params *params)
{
	FIMG2D_ERROR("src : %d, %d, %d, %d / %d, %d / 0x%x, %d, 0x%x)\n",
			params->src_rect.x,
			params->src_rect.y,
			params->src_rect.w,
			params->src_rect.h,
			params->src_rect.full_w,
			params->src_rect.full_h,
			params->src_rect.color_format,
			params->src_rect.bytes_per_pixel,
			(u32)params->src_rect.addr);
	FIMG2D_ERROR("dst : %d, %d, %d, %d / %d, %d / 0x%x, %d, 0x%x)\n",
			params->dst_rect.x,
			params->dst_rect.y,
			params->dst_rect.w,
			params->dst_rect.h,
			params->dst_rect.full_w,
			params->dst_rect.full_h,
			params->dst_rect.color_format,
			params->dst_rect.bytes_per_pixel,
			(u32)params->dst_rect.addr);
	FIMG2D_ERROR("clip: %d, %d, %d, %d\n",
			params->clip.t,
			params->clip.b,
			params->clip.l,
			params->clip.r);
	FIMG2D_ERROR("flag: %d, %d, %d, %d / %d, %d, %d, %d / %d, %d, %d, %d\n",
			params->flag.rotate_val,
			params->flag.alpha_val,
			params->flag.blue_screen_mode,
			params->flag.color_key_val,
			params->flag.color_switch_val,
			params->flag.src_color,
			params->flag.third_op_mode,
			params->flag.rop_mode,
			params->flag.mask_mode,
			params->flag.render_mode,
			params->flag.potterduff_mode,
			params->flag.memory_type);
}

int g2d_init_regs(struct g2d_global *g2d_dev, g2d_params *params)
{
	u32 blt_cmd = 0;

	g2d_rect * src_rect = &params->src_rect;
	g2d_rect * dst_rect = &params->dst_rect;
	g2d_clip * clip     = &params->clip;
	g2d_flag * flag     = &params->flag;

	if (g2d_check_params(params) < 0)
		return -1;

	/* source image */	
	blt_cmd |= g2d_set_src_img(g2d_dev, src_rect, flag);    

	/* destination image */		
	blt_cmd |= g2d_set_dst_img(g2d_dev, dst_rect);

	/* rotation */
	blt_cmd |= g2d_set_rotation(g2d_dev, flag);

	/* clipping */
	blt_cmd |= g2d_set_clip_win(g2d_dev, clip);

	/* color key */
	blt_cmd |= g2d_set_color_key(g2d_dev, flag);

	/* pattern */	
	blt_cmd |= g2d_set_pattern(g2d_dev, src_rect, flag);

	/* rop & alpha blending */
	blt_cmd |= g2d_set_alpha(g2d_dev, flag);

	/* command */
	g2d_set_bitblt_cmd(g2d_dev, src_rect, dst_rect, clip, blt_cmd);

	return 0;
}

int g2d_do_blit(struct g2d_global *g2d_dev, g2d_params *params)
{
	struct mm_struct *mm;
	int ret = false;
	unsigned long 	pgd;
	int need_dst_clean = true;
	void *src_start_addr;
	unsigned int src_size;
	void *clp_start_addr;
	unsigned int clp_size;

	if ((params->src_rect.addr == NULL) 
		|| (params->dst_rect.addr == NULL)) {
		FIMG2D_ERROR("error : addr Null\n");
		return false;
	}		

	if (params->flag.memory_type == G2D_MEMORY_KERNEL) {
		params->src_rect.addr = (unsigned char *)phys_to_virt((unsigned long)params->src_rect.addr);
		params->dst_rect.addr = (unsigned char *)phys_to_virt((unsigned long)params->dst_rect.addr);
		pgd = (unsigned long)init_mm.pgd;
		mm = &init_mm;
	} else {
		pgd = (unsigned long)current->mm->pgd;
		mm = current->mm;
	}
	if (params->flag.memory_type == G2D_MEMORY_USER)
	{
		g2d_clip clip_src;

		g2d_clip_for_src(&params->src_rect, &params->dst_rect, &params->clip, &clip_src);

		src_start_addr = (void*)GET_START_ADDR(params->src_rect);
		src_size = (unsigned int)(GET_RECT_SIZE(params->src_rect) + 8);
		
		clp_start_addr = (void*)GET_START_ADDR_C(params->dst_rect, params->clip);
		clp_size = (unsigned int)GET_RECT_SIZE_C(params->dst_rect, params->clip);
		
		g2d_dev->src_attribute =
			g2d_check_pagetable(mm, src_start_addr, src_size);
		if (g2d_dev->src_attribute == G2D_PT_NOTVALID) {
			FIMG2D_DEBUG("Src is not in valid pagetable, process=%s\n", current->comm);
			ret = false;
			goto fail;			
		}

		g2d_dev->dst_attribute = 
			g2d_check_pagetable(mm, clp_start_addr, clp_size);
		if (g2d_dev->dst_attribute == G2D_PT_NOTVALID) {
			FIMG2D_DEBUG("Dst is not in valid pagetable, process=%s\n", current->comm);
			ret = false;
			goto fail;
		}

		g2d_pagetable_clean(mm, src_start_addr, src_size);
		g2d_pagetable_clean(mm, clp_start_addr, clp_size);
	
		if (params->flag.render_mode & G2D_CACHE_OP) {
			/*g2d_mem_cache_oneshot(mm, (void *)GET_START_ADDR(params->src_rect), 
				(void *)GET_START_ADDR(params->dst_rect),
				(unsigned int)GET_REAL_SIZE(params->src_rect), 
				(unsigned int)GET_REAL_SIZE(params->dst_rect));*/
		//	need_dst_clean = g2d_check_need_dst_cache_clean(mm, params);
			g2d_mem_inner_cache(mm, params);
			g2d_mem_outer_cache(mm, g2d_dev, params, &need_dst_clean);
		}
	}

	if(g2d_init_regs(g2d_dev, params) < 0) {
		ret = false;
		goto fail;
	}
	g2d_sysmmu_set_pgd(g2d_dev,	(unsigned long)virt_to_phys((void *)pgd));
	/* Do bitblit */
	g2d_start_bitblt(g2d_dev, params);

	if (!need_dst_clean)
		g2d_mem_outer_cache_inv(mm, params);

	if (!g2d_wait_for_finish(g2d_dev, params))
	{
		ret = false;
		goto fail;
	}
	ret = true;
fail:
	return ret;
}

int g2d_wait_for_finish(struct g2d_global *g2d_dev, g2d_params *params)
{
	if(atomic_read(&g2d_dev->is_mmu_faulted) == 1) {
		FIMG2D_ERROR("error : sysmmu_faulted early\n");
		atomic_set(&g2d_dev->is_mmu_faulted, 0);
		return false;
	}

	if(wait_event_interruptible_timeout(g2d_dev->waitq,
		(atomic_read(&g2d_dev->in_use) == 0),
		msecs_to_jiffies(G2D_TIMEOUT)) == 0) {
		if(atomic_read(&g2d_dev->is_mmu_faulted) == 1) {
			FIMG2D_ERROR("error : sysmmu_faulted\n");
			FIMG2D_ERROR("faulted addr: 0x%x\n", g2d_dev->faulted_addr);
		} else {
			g2d_reset(g2d_dev);
			FIMG2D_ERROR("error : waiting for interrupt is timeout\n");
		}
		atomic_set(&g2d_dev->is_mmu_faulted, 0);
		g2d_fail_debug(params);
		return false;
	} else if(atomic_read(&g2d_dev->is_mmu_faulted) == 1) {
		FIMG2D_ERROR("error : sysmmu_faulted but auto recoveried\n");
		atomic_set(&g2d_dev->is_mmu_faulted, 0);
		return false;
	}

	atomic_set(&g2d_dev->in_use, 0);
	return true;
}

int g2d_init_mem(struct device *dev, unsigned int *base, unsigned int *size)
{
#ifdef CONFIG_VIDEO_SAMSUNG_MEMSIZE_FIMG2D
#ifdef CONFIG_S5P_MEM_CMA
	struct cma_info mem_info;
	int err;
	char cma_name[8];

	/* CMA */
	sprintf(cma_name, "fimg2d");
	err = cma_info(&mem_info, dev, 0);
	FIMG2D_DEBUG("[cma_info] start_addr : 0x%x, end_addr : 0x%x, "
			"total_size : 0x%x, free_size : 0x%x\n",
			mem_info.lower_bound, mem_info.upper_bound,
			mem_info.total_size, mem_info.free_size);
	if (err) {
		FIMG2D_ERROR("%s: get cma info failed\n", __func__);
		return -1;
	}
	*size = mem_info.total_size;
	*base = (dma_addr_t)cma_alloc
		(dev, cma_name, (size_t)(*size), 0);

	FIMG2D_DEBUG("size = 0x%x\n", *size);
	FIMG2D_DEBUG("*base phys= 0x%x\n", *base);
	FIMG2D_DEBUG("*base virt = 0x%x\n", (u32)phys_to_virt(*base));

#else
	*base = s5p_get_media_memory_bank(S5P_MDEV_FIMG2D, 0);
#endif

#else /* on reserve memory for fimg2d */
	*base = 0;
	*size = 0;
#endif
	return 0;
}


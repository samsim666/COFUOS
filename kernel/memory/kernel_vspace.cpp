#include "vm.hpp"
#include "pm.hpp"
#include "constant.hpp"
#include "util.hpp"
#include "lang.hpp"
#include "hal.hpp"
#include "cpu.hpp"
#include "assert.hpp"
#include "../exception/include/kdb.hpp"

using namespace UOS;

constexpr auto pdpt_table = (VM::PDPT *)HIGHADDR(PDPT8_PBASE);

VM::kernel_vspace::kernel_vspace(void){
	assert(pe_kernel);
	auto module_base = pe_kernel->imgbase;
	auto stk_commit = pe_kernel->stk_commit;
	auto stk_reserve = pe_kernel->stk_reserve;
	assert(module_base >= HIGHADDR(0x01000000) && stk_commit <= stk_reserve && stk_reserve <= 0x100*PAGE_SIZE);
	PDT* pdt_table = (PDT*)HIGHADDR(PDT0_PBASE);
	PT* pt0_table = (PT*)HIGHADDR(PT0_PBASE);

	stk_reserve = align_up(stk_reserve,PAGE_SIZE) >> 12;
	stk_commit = align_up(stk_commit,PAGE_SIZE) >> 12;
	auto stk_top = LOWADDR(KRNL_STK_TOP) >> 12;
	assert(stk_top < 0x200);
#ifdef VM_TEST
	dbgprint("stack committed : %p - %p",HIGHADDR((stk_top - stk_commit)*PAGE_SIZE),HIGHADDR(stk_top*PAGE_SIZE));
	dbgprint("stack reserved : %p - %p",HIGHADDR((stk_top - stk_reserve)*PAGE_SIZE),HIGHADDR((stk_top - stk_commit)*PAGE_SIZE));
#endif
	//stack guard
	assert(!pt0_table[stk_top].present);
	pt0_table[stk_top].preserve = 1;
	pt0_table[stk_top].bypass = 1;

	for (unsigned i = stk_top - stk_reserve;i < (stk_top - stk_commit);++i){
		assert(!pt0_table[i].present);
		pt0_table[i].preserve = 1;
	}
	//preserve & bypass boot area
	for (unsigned i = 0;i < (DIRECT_MAP_TOP >> 12);++i){
		assert(pt0_table[i].present);
		pt0_table[i].bypass = 1;
	}
	for (unsigned i = (DIRECT_MAP_TOP >> 12);i < (BOOT_AREA_TOP >> 12);++i){
		assert(pt0_table[i].present);
		pt0_table[i].present = 0;
		pt0_table[i].preserve = 1;
		pt0_table[i].bypass = 1;
		__invlpg((void*)HIGHADDR(i*PAGE_SIZE));
	}
	BLOCK block;
	block.self = (BOOT_AREA_TOP >> 12);
	block.size = stk_top - block.self - stk_reserve;
	block.prev_valid = block.next_valid = 0;
#ifdef VM_TEST
	dbgprint("free block : 0x%x - 0x%x",block.self, block.self + block.size);
#endif
	block.put(pt0_table + block.self);
	pdt_table[0].head = block.self;
	put_max_size(pdt_table[0],block.size);
	//MAP bypass
	constexpr auto map_off = LOWADDR(MAP_VIEW_BASE) >> 21;
	assert(pdt_table[map_off].present);
	pdt_table[map_off].bypass = 1;
	//PMMBMP bypass
	constexpr auto pdt_off = LOWADDR(PMMBMP_BASE) >> 21;
	auto pdt_count = align_up(pm.bmp_page_count(),0x200) >> 9;
	for (unsigned i = 0;i < pdt_count;++i){
		assert(pdt_table[pdt_off + i].present);
		pdt_table[pdt_off + i].bypass = 1;
	}
	//kernel bypass
	auto krnl_index = LOWADDR(module_base) >> 21;
	assert(krnl_index < 0x200 && pdt_table[krnl_index].present);
	pdt_table[krnl_index].bypass = 1;
}

void VM::kernel_vspace::new_pdt(PDPT& pdpt,map_view& view){
	assert(!pdpt.present);
	auto phy_addr = pm.allocate();
	view.map(phy_addr);
	PDT* table = (PDT*)view;
	zeromemory(table,PAGE_SIZE);

	pdpt = {0};
	pdpt.pdt_addr = phy_addr >> 12;
	pdpt.write = 1;
	pdpt.present = 1;
}

void VM::kernel_vspace::new_pt(VM::PDT& pdt,map_view& view){
	assert(!pdt.present && !pdt.bypass);
	auto phy_addr = pm.allocate();
	view.map(phy_addr);
	PT* table = (PT*)view;
	zeromemory(table,PAGE_SIZE);

	pdt = {0};
	pdt.pt_addr = phy_addr >> 12;
	pdt.write = 1;
	put_max_size(pdt,0x200);
	pdt.present = 1;

	BLOCK block = {0};
	block.size = 0x200;
	block.put(table);
}

bool VM::kernel_vspace::common_check(qword addr,size_t page_count){
	if (addr && page_count)
		;
	else
		return false;
	
	if ((addr & PAGE_MASK) || !IS_HIGHADDR(addr))
		return false;

	if (LOWADDR(addr + page_count*PAGE_SIZE) > (qword)0x008000000000){
		//over 512G not supported
		return false;
	}
	return true;
}

qword VM::kernel_vspace::reserve(qword addr,size_t page_count){
	if (!page_count)
		return 0;
	if (addr){
		if (!common_check(addr,page_count))
			return 0;
	}
	else{
		if (page_count > 0x40000){
			//over 1G not supported
			return 0;
		}
	}
	interrupt_guard ig;
	lock_guard<spin_lock> guard(lock);
	if (addr){
		return reserve_fixed(addr,page_count) ? addr : 0;
	}
	else{
		return (page_count < 0x200) ? reserve_any(page_count) : reserve_big(page_count);
	}

}

qword VM::kernel_vspace::reserve_any(size_t page_count){
	assert(lock.is_locked());
	assert(page_count <= 0x200);
	map_view pdt_view;
	map_view pt_view;
	for (unsigned pdpt_index = 0;pdpt_index < 0x200;++pdpt_index){
		if (!pdpt_table[pdpt_index].present){   //allocate new PDT
			new_pdt(pdpt_table[pdpt_index],pdt_view);
		}
		else{
			pdt_view.map(pdpt_table[pdpt_index].pdt_addr << 12);
		}
		PDT* pdt_table = (PDT*)pdt_view;
		for (unsigned i = 0;i < 0x200;++i){
			auto& cur = pdt_table[i];
			if (cur.bypass)
				continue;
			if (!cur.present){  //allocate new PT
				new_pt(cur,pt_view);
			}
			else{
				pt_view.map(cur.pt_addr << 12);
			}
			PT* table = (PT*)pt_view;
			qword base_addr = HIGHADDR(\
				pdpt_index*0x40000000 \
				+ i * 0x200000 \
			);
			auto res = imp_reserve_any(cur,table,base_addr,page_count);
			if (res)
				return res;
		}
	}
	return 0;
}

qword VM::kernel_vspace::reserve_big(size_t pagecount){
	assert(lock.is_locked());
	auto aligned_count = align_up(pagecount,0x200) / 0x200;
	map_view pdt_view;
	for (unsigned pdpt_index = 0;pdpt_index < 0x200;++pdpt_index){
		if (!pdpt_table[pdpt_index].present){
			new_pdt(pdpt_table[pdpt_index],pdt_view);
		}
		else{
			pdt_view.map(pdpt_table[pdpt_index].pdt_addr << 12);
		}
		PDT* pdt_table = (PDT*)pdt_view;
		unsigned avl_base = 0;
		size_t avl_pages = 0;
		for (unsigned i = 0;i < 0x200;++i){
			auto& cur = pdt_table[i];
			if (cur.bypass || (cur.present && get_max_size(cur) != 0x200)){
				avl_base = i + 1;
				avl_pages = 0;
			}
			else if (++avl_pages == aligned_count){
				break;
			}
		}
		if (avl_pages == aligned_count){
			assert(avl_base + avl_pages <= 0x200);
			qword base_addr = HIGHADDR(\
				pdpt_index*0x40000000 \
				+ avl_base*0x200000 \
			);
			map_view pt_view;
			for (unsigned i = 0;i < avl_pages;++i){
				assert(pagecount);
				auto& cur = pdt_table[avl_base + i];
				assert(!cur.bypass);
				//TODO optimise
				if (cur.present){
					assert(get_max_size(cur) == 0x200);
					pt_view.map(cur.pt_addr << 12);
				}
				else{
					new_pt(cur,pt_view);
				}
				PT* table = (PT*)pt_view;
				auto res = imp_reserve_fixed(cur,table,0,min((size_t)0x200,pagecount));
				if (!res)
					BugCheck(corrupted,cur);
				pagecount -= min((size_t)0x200,pagecount);
			}
			return base_addr;
		}
	}
	return 0;
}

bool VM::kernel_vspace::reserve_fixed(qword base_addr,size_t page_count){
	assert(lock.is_locked());
	assert(base_addr && page_count);
	assert(0 == (base_addr & PAGE_MASK));

	map_view pdt_view;
	map_view pt_view;

	auto pdpt_index = (base_addr >> 30) & 0x1FF;
	auto pdt_index = (base_addr >> 21) & 0x1FF;
	auto addr = base_addr;
	size_t count = 0;
	while (count < page_count){
		if (!pdpt_table[pdpt_index].present){
			new_pdt(pdpt_table[pdpt_index],pdt_view);
		}
		else{
			pdt_view.map(pdpt_table[pdpt_index].pdt_addr << 12);
		}
		PDT* pdt_table = (PDT*)pdt_view;
		while(count < page_count){
			auto& cur = pdt_table[pdt_index];
			if (cur.bypass){
				goto rollback;
			}
			if (!cur.present){
				new_pt(cur,pt_view);
			}
			else{
				pt_view.map(cur.pt_addr << 12);
			}
			PT* table = (PT*)pt_view;
			auto off = (addr >> 12) & 0x1FF;
			auto size = min(page_count - count,0x200 - off);

			auto res = imp_reserve_fixed(cur,table,off,size);
			if (!res){
				goto rollback;
			}
			addr += PAGE_SIZE*size;
			count += size;
			if (++pdt_index == 0x200){
				pdt_index = 0;
				break;
			}
		}
		++pdpt_index;
		assert(pdpt_index < 0x200);
	}
	assert(count == page_count);
	return true;
rollback:
	assert(count < page_count);
	if (count)
		locked_release(base_addr,count);
	return false;
}

bool VM::kernel_vspace::release(qword addr,size_t page_count){
	if (!common_check(addr,page_count))
		return false;
	interrupt_guard ig;
	lock_guard<spin_lock> guard(lock);
	auto res = imp_iterate(pdpt_table,addr,page_count,[](PT& pt,qword,qword) -> bool{
		if (pt.bypass)
			return false;
		if (pt.present){
			return (pt.page_addr && !pt.user) ? true : false;
		}
		return pt.preserve ? true : false;
	});
	if (res != page_count)
		return false;
	locked_release(addr,page_count);
	return true;
}

void VM::kernel_vspace::locked_release(qword base_addr,size_t page_count){
	assert(lock.is_locked());
	assert(base_addr && page_count);
	assert(0 == (base_addr & PAGE_MASK));

	map_view pdt_view;
	map_view pt_view;

	auto pdpt_index = (base_addr >> 30) & 0x1FF;
	auto pdt_index = (base_addr >> 21) & 0x1FF;
	auto addr = base_addr;
	size_t count = 0;
	while(count < page_count){
		if (!pdpt_table[pdpt_index].present){
			BugCheck(corrupted,pdpt_table + pdpt_index);
		}
		pdt_view.map(pdpt_table[pdpt_index].pdt_addr << 12);
		PDT* pdt_table = (PDT*)pdt_view;
		while(count < page_count){
			auto& cur = pdt_table[pdt_index];
			if (cur.bypass || cur.user || !cur.present){
				BugCheck(corrupted,cur);
			}
			pt_view.map(cur.pt_addr << 12);
			PT* table = (PT*)pt_view;
			auto off = (addr >> 12) & 0x1FF;
			auto size = min(page_count - count,0x200 - off);

			imp_release(cur,table,addr,size);

			addr += PAGE_SIZE*size;
			count += size;
			if (++pdt_index == 0x200){
				pdt_index = 0;
				break;
			}
		}
		++pdpt_index;
		assert(pdpt_index < 0x200);
	}
	assert(count == page_count);
}

bool VM::kernel_vspace::commit(qword base_addr,size_t page_count){
	if (!common_check(base_addr,page_count))
		return false;
	auto avl_count = pm.available();
	if (page_count >= avl_count){
		//no physical memory
		return false;
	}
	interrupt_guard ig;
	lock_guard<spin_lock> guard(lock);
	auto res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		return (pt.preserve && !pt.bypass && !pt.present) ? true : false;
	});
	if (res != page_count)
		return false;

	res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		assert(pt.preserve && !pt.bypass && !pt.present);
		pt.page_addr = pm.allocate() >> 12;
		pt.xd = 1;
		pt.pat = 0;
		pt.write = 1;
		pt.user = 0;
		pt.present = 1;
		return true;
	});
	if (res != page_count)
		BugCheck(corrupted,res);
	return true;
}

bool VM::kernel_vspace::protect(qword base_addr,size_t page_count,qword attrib){
	if (!common_check(base_addr,page_count))
		return false;
	//check for valid attrib
	qword mask = PAGE_XD | PAGE_GLOBAL | PAGE_CD | PAGE_WT | PAGE_WRITE;
	if (attrib & ~mask)
		return false;
	interrupt_guard ig;
	lock_guard<spin_lock> guard(lock);
	auto res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		return (pt.present && !pt.bypass && !pt.user && pt.page_addr) ? true : false;
	});
	if (res != page_count)
		return false;

	PTE_CALLBACK fun = [](PT& pt,qword addr,qword attrib) -> bool{
		assert(pt.present && !pt.bypass && !pt.user && pt.page_addr);
		pt.xd = (attrib & PAGE_XD) ? 1 : 0;
		pt.global = (attrib & PAGE_GLOBAL) ? 1 : 0;
		pt.cd = (attrib & PAGE_CD) ? 1 : 0;
		pt.wt = (attrib & PAGE_WT) ? 1 : 0;
		pt.write = (attrib & PAGE_WRITE) ? 1 : 0;
		__invlpg((void*)addr);
		return true;
	};
	res = imp_iterate(pdpt_table,base_addr,page_count,fun,attrib);
	if (res != page_count)
		BugCheck(corrupted,res);
	return true;
}

bool VM::kernel_vspace::assign(qword base_addr,qword phy_addr,size_t page_count){
	if (!common_check(base_addr,page_count) || !phy_addr)
		return false;
	interrupt_guard ig;
	lock_guard<spin_lock> guard(lock);
	auto res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword){
		return (pt.preserve && !pt.present) ? true : false;
	});
	if (res != page_count)
		return false;
	//assume phy_addr is far lower than base_addr
	//use delta to calc back phy_addr
	if (base_addr < phy_addr)
		BugCheck(not_implemented,phy_addr);
	
	PTE_CALLBACK fun = [](PT& pt,qword addr,qword delta) -> bool{
		assert(pt.preserve && !pt.present);
		assert(addr >= delta);
		pt.page_addr = (addr - delta) >> 12;
		pt.xd = 1;
		pt.bypass = 1;
		pt.global = 1;
		pt.pat = 0;
		pt.cd = 1;
		pt.wt = 1;
		pt.user = 0;
		pt.write = 1;
		pt.present = 1;
		return true;
	};
	res = imp_iterate(pdpt_table,base_addr,page_count,fun,base_addr - phy_addr);
	if (res != page_count)
		BugCheck(corrupted,res);
	return true;
}

bool VM::kernel_vspace::peek(void*,qword,size_t){
	//TODO
	return false;
}
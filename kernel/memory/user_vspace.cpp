#include "vm.hpp"
#include "pm.hpp"
#include "sync/include/lock_guard.hpp"
#include "constant.hpp"
#include "lang.hpp"
#include "assert.hpp"
#include "intrinsics.hpp"

using namespace UOS;

user_vspace::user_vspace(void) : cr3(pm.allocate(PM::MUST_SUCCEED)), pl4te(pm.allocate(PM::MUST_SUCCEED)) {
	map_view view(cr3);
	{
		zeromemory((void*)view,PAGE_SIZE);
		auto pl4t_table = (qword*)view;
		pl4t_table[0] = (pl4te | PAGE_USER | PAGE_WRITE | PAGE_PRESENT);
		pl4t_table[0x100] = (PDPT8_PBASE | PAGE_USER | PAGE_WRITE | PAGE_PRESENT);
	}
	view.map(pl4te);
	qword pa_pdt = pm.allocate(PM::MUST_SUCCEED);
	zeromemory((void*)view,PAGE_SIZE);
	auto& pdpt = *(PDPT*)view;
	pdpt.pdt_addr = pa_pdt >> 12;
	pdpt.user = 1;
	pdpt.write = 1;
	pdpt.present = 1;
	//bypass lowest 2M region
	view.map(pa_pdt);
	zeromemory((void*)view,PAGE_SIZE);
	(*(PDT*)view).bypass = 1;
	used_pages = 3;
}

user_vspace::~user_vspace(void){
	interrupt_guard<rwlock> guard(objlock);
	{
		map_view pdpt_view(pl4te);
		auto pdpt_table = (PDPT*)pdpt_view;
		for (unsigned pdpt_index = 0;pdpt_index < 0x200;++pdpt_index){
			if (pdpt_table[pdpt_index].present){
				qword pa_pdt = pdpt_table[pdpt_index].pdt_addr << 12;
				{
					map_view pdt_view(pa_pdt);
					auto pdt_table = (PDT*)pdt_view;
					for (unsigned pdt_index = 0;pdt_index < 0x200;++pdt_index){
						if (pdt_table[pdt_index].present){
							qword pa_pt = pdt_table[pdt_index].pt_addr << 12;
							{
								map_view pt_view(pa_pt);
								auto pt_table = (PT*)pt_view;
								for (unsigned pt_index = 0;pt_index < 0x200;++pt_index){
									auto& cur = pt_table[pt_index];
									if (cur.present){
										assert(cur.user);
										cur.present = 0;
										pm.release(cur.page_addr << 12);
									}
								}
							}
							pm.release(pa_pt);
						}
					}
				}
				pm.release(pa_pdt);
			}
		}
	}
	pm.release(pl4te);
	pm.release(cr3);
}

qword user_vspace::get_cr3(void) const{
	return cr3;
}

bool user_vspace::common_check(qword addr,dword page_count){
	if (addr && page_count)
		;
	else
		return false;
	
	if ((addr & PAGE_MASK) || IS_HIGHADDR(addr))
		return false;
	
	if (addr + page_count*PAGE_SIZE > size_512G){
		//over 512G not supported
		return false;
	}
	return true;
}

qword user_vspace::reserve(qword addr,dword page_count){
	if (addr){
		if (!common_check(addr,page_count))
			return 0;
	}
	else{
		if (!page_count || page_count > 0x40000){
			//over 1G not supported
			return 0;
		}
	}
	map_view view(pl4te);
	auto pdpt_table = (PDPT*)view;
	interrupt_guard<rwlock> guard(objlock);

	if (addr){
		return reserve_fixed(pdpt_table,addr,page_count) ? addr : 0;
	}
	else{
		return (page_count < 0x200) ? \
			reserve_any(pdpt_table,page_count) : \
			reserve_big(pdpt_table,page_count);
	}
}

bool user_vspace::release(qword addr,dword page_count){
	if (!common_check(addr,page_count))
		return false;
	map_view view(pl4te);
	auto pdpt_table = (PDPT*)view;
	interrupt_guard<rwlock> guard(objlock);
	auto res = imp_iterate(pdpt_table,addr,page_count,[](PT& pt,qword,qword) -> bool{
		if (pt.bypass)
			return false;
		if (pt.present){
			return (pt.page_addr && pt.user);
		}
		return pt.preserve;
	});
	if (res != page_count)
		return false;
	safe_release(pdpt_table,addr,page_count);
	return true;
}

bool user_vspace::commit(qword base_addr,dword page_count){
	if (!common_check(base_addr,page_count))
		return false;

	map_view view(pl4te);
	auto pdpt_table = (PDPT*)view;
	interrupt_guard<rwlock> guard(objlock);
	auto res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		return (pt.preserve && !pt.bypass && !pt.present);
	});
	if (res != page_count)
		return false;
	res = pm.reserve(page_count);
	if (!res)
		return false;
	res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		assert(pt.preserve && !pt.bypass && !pt.present);
		pt.page_addr = pm.allocate(PM::TAKE) >> 12;
		pt.xd = 1;
		pt.pat = 0;
		pt.user = 1;
		pt.write = 1;
		pt.present = 1;
		return true;
	});
	if (res != page_count)
		bugcheck("page count mismatch (%x,%x)",res,page_count);
	used_pages += page_count;
	return true;
}

bool user_vspace::protect(qword base_addr,dword page_count,qword attrib){
	if (!common_check(base_addr,page_count))
		return false;
	qword mask = PAGE_XD | PAGE_GLOBAL | PAGE_CD | PAGE_WT | PAGE_WRITE;
	if (attrib & ~mask)
		return false;
	map_view view(pl4te);
	auto pdpt_table = (PDPT*)view;
	interrupt_guard<rwlock> guard(objlock);
	auto res = imp_iterate(pdpt_table,base_addr,page_count,[](PT& pt,qword,qword) -> bool{
		return (pt.present && !pt.bypass && pt.user && pt.page_addr);
	});
	if (res != page_count)
		return false;
	
	PTE_CALLBACK fun = [](PT& pt,qword addr,qword attrib) -> bool{
		assert(pt.present && !pt.bypass && pt.user && pt.page_addr);
		pt.xd = (attrib & PAGE_XD) ? 1 : 0;
		pt.global = (attrib & PAGE_GLOBAL) ? 1 : 0;
		pt.cd = (attrib & PAGE_CD) ? 1 : 0;
		pt.wt = (attrib & PAGE_WT) ? 1 : 0;
		pt.write = (attrib & PAGE_WRITE) ? 1 : 0;
		invlpg((void*)addr);
		return true;
	};
	res = imp_iterate(pdpt_table,base_addr,page_count,fun,attrib);
	if (res != page_count)
		bugcheck("page count mismatch (%x,%x)",res,page_count);
	return true;
}

PT user_vspace::peek(qword va){
	do{
		if (IS_HIGHADDR(va) || va >= size_512G)
			break;
		map_view view(pl4te);
		auto pdpt_table = (PDPT*)view;
		auto pdpt_index = (va >> 30) & 0x1FF;
		auto pdt_index = (va >> 21) & 0x1FF;
		auto pt_index = (va >> 12) & 0x1FF;

		if (!pdpt_table[pdpt_index].present)
			break;
		map_view pdt_view(pdpt_table[pdpt_index].pdt_addr << 12);
		auto pdt_table = (PDT*)pdt_view;
		if (!pdt_table[pdt_index].present)
			break;
		map_view pt_view(pdt_table[pdt_index].pt_addr << 12);
		auto pt_table = (PT*)pt_view;
		return pt_table[pt_index];
	}while(false);
	return PT {0};
}
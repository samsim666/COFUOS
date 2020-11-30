#include "vm.hpp"
#include "pm.hpp"
//#include "ps.hpp"
#include "sysinfo.hpp"
#include "constant.hpp"
#include "util.hpp"
#include "bits.hpp"
#include "assert.hpp"
//#include "mp.hpp"
#include "lock_guard.hpp"
#include "atomic.hpp"

using namespace UOS;

void invlpg(volatile void* p){
	__invlpg((void*)p);
}

VM::map_view::map_view(qword pa){
	assert(0 == (pa & PAGE_MASK));

	auto table = (volatile qword*)MAP_TABLE_BASE;
	for (unsigned index = 0; index < 0x200; ++index)
	{
		qword origin_value = table[index];
		if (origin_value & 0x01)
			continue;
		
		qword new_value = 0x8000000000000103 | pa;

		if (origin_value == cmpxchg(table + index, new_value, origin_value)){
			//gain this slot, set VA
			ptr = (void*)(MAP_VIEW_BASE + PAGE_SIZE * index);
			return;
		}
	}
	BugCheck(bad_alloc,table);
}

VM::map_view::~map_view(void){
	qword addr = (qword)ptr;
	assert(0 == (addr & PAGE_MASK));
	if (addr < MAP_VIEW_BASE)
		BugCheck(out_of_range,ptr);
	unsigned index = (addr - MAP_VIEW_BASE) >> 12;
	if (index >= 0x200)
		BugCheck(out_of_range,ptr);

	auto table = (volatile qword *)MAP_TABLE_BASE;
	qword origin_value = table[index];
	if (origin_value & 0x01)
		BugCheck(corrupted, origin_value);
	if (origin_value != cmpxchg(table + index, (qword)0, origin_value))
		BugCheck(corrupted, origin_value);
	invlpg(ptr);
}

VM::map_view::operator byte*(void){
	return (byte*)ptr;
}
VM::map_view::operator const byte*(void) const{
	return (const byte*)ptr;
}

bool VM::spy(void* dst,qword base,size_t len){
	//TODO
	
	memcpy(dst,(const void*)base,len);
	
	return true;
	
}


VM::window::window(void* pm){
	vbase=(byte*)sys->reserve(nullptr,1);
	sys->map(vbase,pm,PAGE_WRITE | PAGE_NX);
}

VM::window::~window(void){
	sys->release(vbase,1);
}

void VM::window::zero(void){
	zeromemory(vbase,0x1000);
}

size_t VM::window::read(void* dst,size_t off,size_t len) const{
	if (off>=PAGE_SIZE)
		return 0;
	size_t read_length=min(PAGE_SIZE-off,len);
	memcpy(dst,vbase+off,read_length);
	return read_length;
}


volatile byte* VM::VMG::base(void)const volatile{
	return (volatile byte*)(highaddr*HIGHADDR(0)+index*0x40000000);

}

volatile qword* VM::VMG::table(void)const volatile{
	return (volatile qword*)(base()+0x1000*(offset*0x10+1));
	
}

volatile byte* VM::VMG::bitmap(void)const volatile{
	return base()+0x1000*(offset*0x10+2);
}

#pragma warning(push)
#pragma warning(disable:4365)

void VM::VMG::lock(void)volatile{
	volatile byte* lck=(volatile byte*)this + 7;	//directly operate on sync bit
	assert(0 == sync);
	while(true){
		byte expected=*lck & (byte)~0x20;
		
		//if (expected == _cmpxchgb(lck,expected,expected | 0x20) )
		if (expected == cmpxchg<byte>(lck,expected | (byte)0x20,expected))
			break;
		_mm_pause();
	}
	assert(1 == sync);
	
}

void VM::VMG::unlock(void)volatile{
	assert(1 == sync);
	
	volatile byte* lck=(volatile byte*)this + 7;	//directly operate on sync bit
	
	byte expected= *lck | (byte)0x20;
	//expected=_cmpxchgb(lck,expected,expected & ~0x20);
	expected = cmpxchg<byte>(lck,expected & (byte)~0x20,expected);
	
	assert(0 != (expected & 0x20));
	assert(0 == sync);

}
#pragma warning(pop)


VM::VMG::PT_mapper::PT_mapper(volatile VM::VMG& _vmg,size_t pdt_index) : vmg(_vmg), base((qword*)(_vmg.base()+0x1000 * (0x0B + 0x10 * _vmg.offset))){
	assert(pdt_index < 0x200);
	volatile qword* pdt=vmg.table();//(qword*)(vmg.base() + 0x1000 * (1 + 0x10 * vmg.offset));
	volatile qword* pt0=(volatile qword*)(vmg.base() + 0x1000 * (0x0A + 0x10 * vmg.offset));
	
	assert(0 == pt0[0x0B + vmg.offset*0x10]);	//PTn shall empty
	
	assert(0xFF == *(vmg.bitmap() + vmg.offset*0x10/8) );
	assert(0x0F == 0x0F & *(vmg.bitmap() + 1 + vmg.offset*0x10/8) );		//VMG info should have reserved
	pt0[0x0B + vmg.offset*0x10] = pdt[pdt_index];	//map to PTn page
	invlpg(base);
}

VM::VMG::PT_mapper::PT_mapper(volatile VM::VMG& _vmg,void* pm) : vmg(_vmg), base((qword*)(_vmg.base()+0x1000 * (0x0B + 0x10 * _vmg.offset))){
	assert(nullptr != pm);
	page_assert(pm);

	volatile qword* pt0=(volatile qword*)(vmg.base() + 0x1000 * (0x0A + 0x10 * vmg.offset));
	
	assert(0 == pt0[0x0B + vmg.offset*0x10]);	//PTn shall empty
	
	assert(0xFF == *(vmg.bitmap() + vmg.offset*0x10/8) );
	assert(0x0F == 0x0F & *(vmg.bitmap() + 1 + vmg.offset*0x10/8) );		//VMG info should have reserved
	pt0[0x0B + vmg.offset*0x10]=(qword)pm | PAGE_PRESENT | PAGE_WRITE | PAGE_NX ;
	invlpg(base);
}

VM::VMG::PT_mapper::~PT_mapper(void){
	volatile qword* pt0=(volatile qword*)(vmg.base() + 0x1000 * (0x0A + 0x10 * vmg.offset));
	assert(0 != pt0[0x0B + vmg.offset*0x10]);	//PTn shall not empty
	pt0[0x0B + vmg.offset*0x10] = 0;
	invlpg(base);
	
}

volatile qword& VM::VMG::VMG::PT_mapper::operator[](size_t index) volatile{
	assert(index < 0x200);
	return base[index];
}


void VM::VMG::construct(const PE64& pe){
#ifdef _DEBUG
	static bool init=false;
	assert(false == init);
#endif
	sys->index=0;		//first GB in sys area
	sys->highaddr=1;
	sys->offset=1;		//VMG info starts at 16-th page
	volatile qword* pdt=(qword*)HIGHADDR(PDT0_PBASE);	//PDT of first sys area (this area)
	//VMG of sys area is placed at PDPT_HIGH_PBASE,whose first entrance is initialized during krnldr
	assert(HIGHADDR(sys->pmpdt<<12) == (qword)pdt);
	
	volatile qword* pt=(qword*)HIGHADDR(PT0_PBASE);

	volatile byte* base=sys->base();		//LA of this GB
	//VMG info starts at 16-th page
	pt[0x10]=0;	//krnldr store PT0 here,unmap it
	invlpg(base+0x10000);
	pt[0x11]=PDT0_PBASE | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_GLOBAL | PAGE_NX ;	//PDT area
	invlpg(base+0x11000);	//krnldr store common_buffer here,unmap it
	pt[0x1A]=PT0_PBASE | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_GLOBAL | PAGE_NX ;	//PT0 area
	
	volatile byte* mapped_addr=base+0x1000 * (0x1B);		// addr of PTn page
	
	//PT_mapper not available,do it manually
	//TRICK: use PTn page to map PA (simulate PT_mapper)

	for (size_t i=0;i<8;i++){
		
		void* p=(void*)pm.allocate();
		pt[0x1B]=(qword)p | PAGE_PRESENT | PAGE_WRITE | PAGE_NX ;	//map physical page to PTn page
		invlpg(mapped_addr);
		zeromemory((void*)mapped_addr,PAGE_SIZE);
		
		if (0==i){
			*(volatile dword*)mapped_addr = 0x0FFFFFFF;	//mask out used pages in bitmap (28 pages)
			
		}
		
		pt[0x12+i]=(qword)p | PAGE_PRESENT | PAGE_WRITE | PAGE_NX ;
	}
	
	pt[0x1B]=0;		//clear PTn page
	invlpg(mapped_addr);
	
	//mark already used pages
	
	BugCheck(not_implemented,0);
	
	bits bmp((void*)sys->bitmap(),0x8000);
	size_t off = LOWADDR(sysinfo->PMM_qmp_vbase) >> 12;
	assert(off < 8*0x8000);
	bmp.set(off,sysinfo->PMM_qmp_page,1);	//PMMQMP
	
	//TRICK: off also krnl stk top(with guard page)
	
	qword stack_reserve=pe.stack().first >> 12;
	bmp.set( off - stack_reserve - 1 , stack_reserve + 1 , 1 );	//stack
		
	off = LOWADDR(pe.base()) >> 12;
	
	bmp.set(off,align_up(pe.header_size(),PAGE_SIZE) >> 12,1);
	
	auto section=pe.section();	
	
	do{
		off = LOWADDR( section.base() ) >> 12;
		bmp.set(off,align_up(section.size(),PAGE_SIZE) >> 12,1);
		

	}while(section.next());
	
	
#ifdef _DEBUG
	init=true;
#endif

}

//general constructor for 1G user or sys area
VM::VMG::VMG(bool k,word id) : present(0),writable(1),user(1),writethrough(0),cachedisable(0),accessed(0),highaddr(k?1:0),largepage(0),offset(0),pmpdt(0),index((assert(id < (word)512),id)),sync(0),xcutedisable(0) {

	void* d=pmm.allocate(table(),PM::must_succeed);		//PDT page
	void* t=pmm.allocate((void*)PM::nowhere,PM::must_succeed);		//PT page
	
	//NOTE: VM::window works on sys,thus available
	
	{	//zeroing PDT and assign first PT
		VM::window w(d);
		w.zero();
		w.at<qword>(0) = (qword)t | PAGE_PRESENT | PAGE_WRITE;
		
	}	//now first 2M of this G can be mapped
	{
		//zeroing PT and put VMG info in
		VM::window w(t);
		w.zero();
		//w.at<qword>(0)	GAP
		
		w.at<qword>(1) = (qword)d | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_NX | (highaddr?PAGE_GLOBAL:0);	//PDT
		w.at<qword>(11)=(qword)t | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_NX | (highaddr?PAGE_GLOBAL:0);	//PT0
		//first page of bitmap,mask out used area
		void* p=pmm.allocate(bitmap(),PM::must_succeed);
		w.at<qword>(2) = (qword)p | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_NX | (highaddr?PAGE_GLOBAL:0);	//bitmap[0]
		{
			VM::window wp(p);
			wp.zero();
			wp.at<word>(0) = (word)0xFFF;	//used 12 pages
		}
		
		//fill the rest of bitmap
		for (size_t i=1;i<8;i++){
			p = pmm.allocate(bitmap() + i*PAGE_SIZE , PM::zero_page | PM::must_succeed);
			w.at<qword>(i+2) = (qword)p | PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_NX | (highaddr?PAGE_GLOBAL:0);
		}
		
	}
	pmpdt=(qword)d >> 12;	//put the 1G page into PDPT
	present=1;	//activate it
}





bool VM::VMG::bitset(size_t off,size_t bitcount) volatile{
	assert(1 == sync);
	assert(off+bitcount < 0x8000*8);
	
	bits bmp((void*)bitmap(),0x8000);
	
	
	for (size_t i=0;i<bitcount;i++){
		if (bmp.at(off+i))
			return false;
	}
	
	bmp.set(off,bitcount,1);
	
	return true;
	
	
}


bool VM::VMG::bitscan(size_t& res,size_t bitcount)volatile{
	assert(1 == sync);
	
	bits bmp((void*)bitmap(),0x8000);
	
	res=0;
	
	while(res<0x8000*8-bitcount){
		size_t cnt=0;
		while(cnt<bitcount){
			if (bmp.at(res+cnt))
				break;
			++cnt;
		}
		if (cnt==bitcount){	//found
			bmp.set(res,bitcount,1);
			return true;
		}
		
		//reset and continue
		res+=cnt+1;
		
		
	}
	
	return false;
	
	/*
	
	byte* bmp=bitmap();
	size_t res = req;	//req ? req : 0;
	size_t cnt = bitcount;
	size_t i = res/8;		//current byte
	//assertinv(0,res%8);		//???
	for (;i<0x8000;i++){
		if (cnt<8){		//single byte
			byte mask=BITMASK(cnt);
			
			if (bmp[i] & mask)
				;
			else{	//found
				cnt=0;
				break;
			}
			
			if (bitcount==cnt){		//less than 8 pages,try to find result at tail of a byte
				//for (size_t j=1;j<cnt;j++){
				for (size_t j=0;j<8-cnt;j++){	//n-bit mask needs 8-n times to move through a byte
					mask<<=1;
					res++;
					if (bmp[i] & mask)
						;
					else{	//found
						cnt=0;
						break;
					}
				}
				if (0==cnt)
					break;
				
			}
			
			//else bitcount more than 8,no,re-find
		}
		else{	//more than 8 pages remaining,find a '0' byte
			if (0==bmp[i]){
				if (cnt-=8)
					continue;
				else
					break;
			}
		}
		if (req)	//fixed address not available
			return false;
		
		//reset and continue to find
		cnt=bitcount;
		res=(i+1)*8;
	}
	if (cnt)	//not found
		return false;
	
	for (i=0;i<bitcount;i++){
		bmp[i+res/8]=0xFF;
	}
	
	bmp[i+res/8] |= BITMASK(bitcount%8)<<(res%8);
	
	req=res;
	
	return true;
	*/
}

void VM::VMG::bitclear(size_t off,size_t bitcount)volatile{
	assert(1 == sync);
	assert(off+bitcount < 0x8000*8);
	
	bits bmp((void*)bitmap(),0x8000);
	
#ifdef _DEBUG
	for (size_t i=0;i<bitcount;i++){
		assert(0 != bmp.at(off+i));
	}
#endif

	bmp.set(off,bitcount,0);

	
	/*
	
	byte* bmp=bitmap();
	
	if (off & 7){	//misaligned heading
		byte mask=~BITMASK(off & 7);
		
		assert(mask,bmp[off/8] & mask);
		
		bmp[off/8] &= ~mask;
		
	}
	assert(0,off & 7);
	while(bitcount>=8){
		assert(0xFF,bmp[off/8]);
		
		bmp[off/8]=0;
		off+=8;
		bitcount-=8;
		
	}
	
	
	if (bitcount){	//misaligned tailing
		byte mask=BITMASK(bitcount);
		
		assert(mask,bmp[off/8] & mask);
		
		bmp[off/8] &= ~mask;
		
	}
	*/
	
}

bool VM::VMG::bitcheck(size_t off,size_t bitcount) const volatile{
	assert(1 == sync);
	assert(off+bitcount < 0x8000*8);
	
	const bits bmp((void*)bitmap(),0x8000);
	
	for (size_t i=0;i<bitcount;i++){
		if (!bmp.at(off+i))
			return false;
	}
	return true;
	
}

qword VM::VMG::PTE_set(volatile qword& dst,void* pm,qword attrib)volatile{
	assert(1 == sync);
	//MP_assert(true);
	
	assert(0 == attrib & ~0xC00000000000019F);
	assert(0 == (qword)pm & 0xFFF0000000000FFF);
	
	//return _xchgq(dst,(qword)pm | attrib);
	return xchg<qword>(&dst,(qword)pm | attrib);
}


void VM::VMG::check_range(volatile void* p,size_t l) const volatile{
	if ((volatile byte*)p-base() >= 0x40000000)	//1G
		BugCheck(out_of_range,p);
	if ((volatile byte*)p+l*0x1000-base() > 0x40000000)	//1G
		BugCheck(out_of_range,p);

}


/*
8 bits in 1 byte		3
4096 bytes in 1 page	12
8 pages for bitmap		3

total 18 bits		BITMASK(18)

*/

void* VM::VMG::reserve(void* fixbase,size_t pagecount)volatile{
	assert(0 != pagecount);
	//assert(0,attrib & ~0x80000000000011E);
	page_assert(fixbase);
	
	if (fixbase){	//check if fix base within this VMG
		check_range(fixbase,pagecount);
	}
	
	lock_guard<volatile VMG> lck(*this);
	
	size_t off;
	
	if (fixbase){	//if given address available
		off=((qword)fixbase >> 12) & BITMASK(18);
		if (!bitset(off,pagecount))
			return nullptr;
	}
	else{		//find a VM area
		if (!bitscan(off,pagecount))
			return nullptr;
	}
	assert(off+pagecount < 8*0x8000);
	
	size_t cur=off;		//#-th page in this GB
	volatile qword* pdt=table();
	while(pagecount){
		
		
		if (pdt[cur/0x200] & 1)	//PT available
			;
		else{	//new PT
			assert(0 == pdt[cur/0x200]);	//shall be empty slot
			
			//WARNING: zero_page cause window construction !
			void* pm=pmm.allocate((void*)PM::nowhere,PM::must_succeed);
			
			PT_mapper w(*this,pm);
			zeromemory(&w[0],PAGE_SIZE);
			
			PTE_set(pdt[cur/0x200],pm,PAGE_PRESENT | PAGE_WRITE | PAGE_CD | PAGE_WT | PAGE_NX | (highaddr?PAGE_GLOBAL:0) );
			
		}
		
		assert(0 != pdt[cur/0x200] & 0x000FFFFFFFFFF000);		//shall have PT's PM
		
		PT_mapper w(*this,cur/0x200);	//map PT
		
		//window w((void*)(pdt[cur/0x200] & 0x000FFFFFFFFFF000));
		
		size_t border=(cur+0x200) & ~0x1FF;		//upper bound page # of current PT

		while (pagecount && cur<border){
			qword oldval = PTE_set(w[cur % 0x200],nullptr,PAGE_WRITE);	//not present
			
			assert(0 == oldval);
			
			pagecount--,cur++;
		}
		
	}
	
	
	return (void*)(base()+PAGE_SIZE*off);
}

void VM::VMG::release(void* base,size_t pagecount)volatile{
	assert(0 != pagecount);
	page_assert(base);
	assert(nullptr != base);
	
	//check if within this VMG
	check_range(base,pagecount);
	
	
	lock_guard<volatile VMG> lck(*this);
	
	size_t off=((qword)base >> 12) & BITMASK(18);

	bitclear(off,pagecount);	//since already locked it's okay to put in this order
	
	volatile qword* pdt=table();
	while(pagecount){
		assert(0 != pdt[off/0x200] & 1);	//present
		assert(0 != pdt[off/0x200] & 0x000FFFFFFFFFF000);	//has PA
		
		PT_mapper w(*this,off/0x200);	//load PT
		//window w((void*)(pdt[off/0x200] & 0x000FFFFFFFFFF000));
		
		size_t border = (off+0x200) & ~0x1FF;		//upper bound page # of current PT
		
		while(pagecount && off < border){
			qword cur=xchg<qword>(&w[off % 0x200],0);
			assert(0 != cur & 1);
			
			if (cur & PAGE_COMMIT){	//shall release PM
				pmm.release((void*)(cur & 0x000FFFFFFFFFF000));
			}
			
			
			pagecount--,off++;
		}
		
	}
	
}

bool VM::VMG::commit(void* base,size_t pagecount,qword attrib)volatile{
	assert(0 != pagecount);
	page_assert(base);
	assert(nullptr != base);
	
	check_range(base,pagecount);
	
	//assert(0,attrib & 0x800000000000011F);
	attrib = (attrib & 0x800000000000011F) | PAGE_PRESENT | PAGE_COMMIT;
	if (highaddr)
		attrib &= ~PAGE_USER;
	
	lock_guard<volatile VMG> lck(*this);
	
	size_t off=((qword)base >> 12) & BITMASK(18);

	//check bitmap first
	if (!bitcheck(off,pagecount))
		return false;
	


	volatile qword* pdt=table();
	while(pagecount){
		assert(0 != pdt[off/0x200] & 1);
		assert(0 != pdt[off/0x200] & 0x000FFFFFFFFFF000);
		
		PT_mapper w(*this,off/0x200);	//load PT
		//window w((void*)(pdt[off/0x200] & 0x000FFFFFFFFFF000));
		
		size_t border = (off+0x200) & ~0x1FF;
		
		while(pagecount && off < border){
			qword cur = w[off % 0x200];
			
			//shall have reserved
			assert(0 != cur);
			assert(0 == cur & 1);
			
			
			PTE_set(w[off % 0x200],
				pmm.allocate(this->base() + PAGE_SIZE*off , PM::must_succeed),
				attrib);
			
			pagecount--,off++;
		}
	}
	return true;
}

bool VM::VMG::map(void* vbase,void* pm,qword attrib)volatile{
	page_assert(vbase);
	page_assert(pm);
	assert(nullptr != vbase);
	assert(nullptr != pm);
	
	check_range(vbase,1);
	
	
	attrib = (attrib & 0x800000000000011F) | 1;
	if (highaddr)
		attrib &= ~PAGE_USER;

	lock_guard<volatile VMG> lck(*this);
	size_t off=((qword)vbase >> 12) & BITMASK(18);
	
	//check bitmap
	
	if (!bitcheck(off,1))
		return false;
	
	
	
	volatile qword* pdt=table();
	
	assert(0 != pdt[off/0x200] & 1);
	assert(0 != pdt[off/0x200] & 0x000FFFFFFFFFF000);
	
	PT_mapper w(*this,off/0x200);	//load PT
	//window w((void*)(pdt[off/0x200] & 0x000FFFFFFFFFF000));
	
	
	qword cur = w[off % 0x200];
	
	assert(0 != cur);
	assert(0 == cur & 1);
	
	PTE_set(w[off % 0x200],pm,attrib);
	return true;
}

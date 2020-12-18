#include "acpi.hpp"
#include "util.hpp"
#include "lang.hpp"
#include "../exception/include/kdb.hpp"
#include "bugcheck.hpp"
#include "assert.hpp"
#include "../memory/include/vm.hpp"

using namespace UOS;

#define HEADER_SIZE 36

bool ACPI::validate(const void* base_addr,size_t limit){
	if (limit < 8)
		return false;
	auto addr = (const byte*)base_addr;
	auto size = *(const dword*)(addr + 4);
	if (size < HEADER_SIZE || size > limit)
		return false;
	byte sum = 0;
	while(size--){
		sum += *addr++;
	}
	return sum == 0;
}


ACPI::ACPI(void) {
	struct RSDP{
		qword addr : 56;
		qword type : 8;
	};
	static_assert(sizeof(RSDP) == 8,"RSDP size mismatch");
	auto rsdp = (RSDP const*)&sysinfo->ACPI_RSDT;
	do{
		if (!rsdp)
			break;
		dbgprint("Going through ACPI");
		auto aligned_addr = align_down(rsdp->addr,PAGE_SIZE);
		auto offset = rsdp->addr - aligned_addr;
		VM::map_view rsdt_view(aligned_addr);
		auto view = (dword const*)((byte const*)rsdt_view + offset);
		if (!validate(view,PAGE_SIZE - offset))
			break;
		
		auto size = view[1];
		assert(size >= HEADER_SIZE);
		size -= HEADER_SIZE;
		if (rsdp->type){    //XSDT
			if (*view != 0x54445358 /*XSDT*/ || (size & 0x07))
				break;
			auto it = (qword const*)(view + HEADER_SIZE/4);
			while(size){
				parse_table(*it);
				++it;
				size -= 8;
			}
		}
		else{   //RSDT
			if(*view != 0x54445352 /*RSDT*/ || (size & 0x03))
				break;
			view += (HEADER_SIZE/4);
			while(size){
				parse_table(*view);
				++view;
				size -= 4;
			}
		}
		return;
	}while(false);
	BugCheck(hardware_fault,rsdp->addr);
}


void ACPI::parse_table(qword pbase){
	auto aligned_pbase = align_down(pbase,PAGE_SIZE);
	auto offset = pbase - aligned_pbase;
	VM::map_view table_view(aligned_pbase);
	auto view = (dword const*)((byte const*)table_view + offset);
	if (!validate(view,PAGE_SIZE - offset))
		return;
	char table_name[5];
	memcpy(table_name,view,4);
	table_name[4] = 0;
	dbgprint("ACPI table %s @ %x ~ %x",table_name,pbase,pbase + *(view + 1));
	if (*view == 0x43495041 /*APIC*/){
		madt = new MADT(view);
	}
	if (*view == 0x50434146 /*FACP*/){
		if (fadt)
			BugCheck(hardware_fault,pbase);
		fadt = new FADT(view);
	}
}

FADT::FADT(const dword* view){
	auto size = *(view + 1);
	memcpy(this,view + HEADER_SIZE/4,min<size_t>(size,sizeof(FADT)));
	if (size < sizeof(FADT)){
		zeromemory((byte*)this + size,sizeof(FADT) - size);
	}
	dbgprint("Profile : %d",PreferredPowerManagementProfile);
	dbgprint("SCI IRQ %d",SCI_Interrupt);
	dbgprint("RTC century support : %s",Century ? "true" : "false");
	dbgprint("FADT flags = 0x%x",(qword)Flags);
}

MADT::MADT(void const* vbase){
	byte const* cur = (byte const*)vbase;
	auto limit = cur + *((dword const*)vbase + 1);
	cur += HEADER_SIZE;
	local_apic_pbase = *(dword const*)cur;
	if (0 == (*(cur+4) & 0x01)){	//no 8259
		pic_present = false;
	}
	dbgprint("8259 PIC present : %s",pic_present ? "true" : "false");
	cur += 8;
	bool io_apic_found = false;
	while (cur < limit){
		switch(*cur){
		case 0: //Processor Local APIC
			if (8 == *(cur+1) && (1 & *(cur+4))){
				byte uid = *(cur+2);
				byte apic_id = *(cur+3);
				dbgprint("Processor #%d : APICid = 0x%x",uid,(qword)apic_id);
				processors.push_back(processor{uid,apic_id});
			}
			break;
		case 1:	//IO APIC
			if (12 == *(cur+1)){
				auto cur_gsi = *(dword const*)(cur+8);
				dbgprint("IO APIC id = %d GSI#%d @ %p",*(cur+2),cur_gsi,(qword)*(dword const*)(cur+4));
				if (!io_apic_found || cur_gsi < gsi_base){
					io_apic_pbase = *(dword const*)(cur+4);
					gsi_base = cur_gsi;
				}
				io_apic_found = true;
			}
			break;
		case 2:	//Interrupt Source Override
			if (10 == *(cur+1) && 0 == *(cur+2)){
				dword gsi = *(dword const*)(cur+4);
				byte irq = *(cur+3);
				byte mode = *(cur+8);
				dbgprint("IRQ#%d ==> GSI#%d mode 0x%x",irq,gsi,(qword)mode);
				redirects.push_back(redirect{gsi,irq,mode});
			}
			break;
		case 3:	//NMI Source
			if (8 == *(cur+1)){
				dword gsi = *(dword const*)(cur+4);
				byte mode = *(cur+2);
				dbgprint("GSI#%d as NMI mode 0x%x",gsi,(qword)mode);
				redirects.push_back(redirect{gsi,2,mode});
			}
			break;
		case 4:	//Local APIC NMI
			if (6 == *(cur+1)){
				byte uid = *(cur+2);
				byte mode = *(cur+3);
				byte pin = *(cur+5);
				dbgprint("NMI pin%d for Processor#%d mode 0x%x",pin,uid,(qword)mode);
				nmi_pins.push_back(nmi{uid,pin,mode});
			}
			break;
		case 5:	//Local APIC Address Override
			if (12 == *(cur+1)){
				local_apic_pbase = *(qword const*)(cur+4);
				dbgprint("local APIC redirect @ %p",local_apic_pbase);
			}
			break;
		}
		if (0 == *(cur + 1))
			BugCheck(hardware_fault,cur);
		cur += *(cur + 1);
	}
}

const MADT& ACPI::get_madt(void) const{
	if (!madt)
		BugCheck(hardware_fault,this);
	return *madt;
}

const FADT& ACPI::get_fadt(void) const{
	if (!fadt)
		BugCheck(hardware_fault,this);
	return *fadt;
}
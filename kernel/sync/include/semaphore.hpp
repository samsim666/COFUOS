#pragma once
#include "types.h"
#include "process/include/waitable.hpp"

namespace UOS{
	class semaphore : public waitable{
		const dword total;
		dword count;
		bool named = false;
	public:
		semaphore(dword initial);
		~semaphore(void);
		OBJTYPE type(void) const override{
			return SEMAPHORE;
		}
		bool check(void) override;
		REASON wait(qword = 0,wait_callback = nullptr) override;
		bool signal(void);
		bool relax(void) override;
		void manage(void*) override;
	};
}
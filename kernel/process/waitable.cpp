#include "waitable.hpp"
#include "thread.hpp"
#include "core_state.hpp"
#include "dev/include/timer.hpp"
#include "sync/include/lock_guard.hpp"
#include "assert.hpp"

using namespace UOS;


void thread_queue::put(thread* th){
	th->next = nullptr;
	if (tail){
		assert(head && tail->next == nullptr);
		tail->next = th;
		tail = th;
	}
	else{
		assert(head == nullptr);
		head = tail = th;
	}
}

thread* thread_queue::get(void){
	if (head){
		assert(tail);
		auto th = head;
		if (th->next == nullptr){
			assert(head == tail);
			tail = nullptr;
		}
		head = th->next;
		th->next = nullptr;
		
		return th;
	}
	else{
		assert(tail == nullptr);
		return nullptr;
	}
}

thread*& thread_queue::next(thread* th){
	return th->next;
}

void thread_queue::clear(void){
	head = tail = nullptr;
}

waitable::~waitable(void){
	notify(ABANDONED);
}

size_t waitable::notify(REASON reason){
	thread* ptr;
	interrupt_guard<void> ig;
	{
		lock_guard<spin_lock> guard(lock);
		ptr = wait_queue.head;
		wait_queue.clear();
	}
	return imp_notify(ptr,reason);
}

waitable::REASON waitable::imp_wait(qword us){
	IF_assert;
	assert(lock.is_locked());
	this_core core;
	thread* this_thread = core.this_thread();
	qword ticket = 0;
	if (us)
		ticket = timer.wait(us,on_timer,this_thread);
	this_thread->set_state(thread::WAITING,ticket,this);
	wait_queue.put(this_thread);

	lock.unlock();
	core.switch_to(ready_queue.get());
	return this_thread->get_reason();
}

waitable::REASON waitable::wait(qword us){
	interrupt_guard<void> ig;
	lock.lock();
	return imp_wait(us);
}

void waitable::on_timer(qword ticket,void* ptr){
	IF_assert;
	auto th = (thread*)ptr;
	if (th->get_ticket() != ticket)
		return;
	/*
	assert(th->state == thread::WAITING);
	if (th->wait_for)
		th->wait_for->cancel(th);
	
	th->timer_ticket = 0;
	th->set_state(thread::READY);
	*/
	th->set_state(thread::READY,waitable::TIMEOUT);
	this_core core;
	auto this_thread = core.this_thread();
	if (th->get_priority() < this_thread->get_priority()){
		this_thread->set_state(thread::READY);
		ready_queue.put(this_thread);
		core.switch_to(th);
	}
	else{
		ready_queue.put(th);
	}
}

//wait-timeout should happen less likely, currently O(n)
void waitable::cancel(thread* th){
	assert(th);
	interrupt_guard<spin_lock> guard(lock);
	auto prev = wait_queue.head;
	if (prev == th){
		wait_queue.get();
		return;
	}
	while(prev){
		auto next = thread_queue::next(prev);
		if (next == th)
			break;
		prev = next;
	}
	if (prev){
		auto th_next = thread_queue::next(th);
		if (th_next){
			thread_queue::next(prev) = th_next;
		}
		else{
			assert(th == wait_queue.tail);
			thread_queue::next(prev) = nullptr;
			wait_queue.tail = prev;
		}
		return;
	}
	bugcheck("wait_queue corrupted @ %p",&wait_queue);
}

size_t waitable::imp_notify(thread* th,REASON reason){
	IF_assert;
	if (!th)
		return 0;
	thread* target = nullptr;
	this_core core;
	thread* this_thread = core.this_thread();
	word cur_priority = this_thread->get_priority();
	size_t count = 0;
	
	while(th){
		auto next = thread_queue::next(th);
		th->set_state(thread::READY,reason);
		auto th_priority = th->get_priority();
		if (th_priority < cur_priority){
			if (target){
				ready_queue.put(target);
			}
			target = th;
			cur_priority = th_priority;
		}
		else{
			ready_queue.put(th);
		}
		++count;
		th = next;
	}

	if (target){
		if (this_thread->get_state() == thread::STOPPED){
			ready_queue.put(target);
		}
		else{
			this_thread->set_state(thread::READY);
			ready_queue.put(this_thread);
			core.switch_to(target);
		}
	}
	return count;
}

#pragma once
#include <Ntddk.h>

struct FastMutex {
public:
	void Init();

	void Lock();
	void Unlock();

private:
	FAST_MUTEX m_mutex;
};
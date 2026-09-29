#pragma once
#include <windows.h>

namespace DebugRegisterUpdates
{
// CONTEXT_DEBUG_REGISTERS writes all slots, even when a caller edits only one.
// Serialize the entire suspend/read/modify/write/resume transaction. Acquire
// before SuspendThread so an updater cannot suspend a thread holding this lock.
// Exception handlers must not acquire it: they can run on a suspended target.
class Guard
{
public:
    Guard() { AcquireSRWLockExclusive(&lock_); }
    ~Guard() { ReleaseSRWLockExclusive(&lock_); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    inline static SRWLOCK lock_ = SRWLOCK_INIT;
};
}

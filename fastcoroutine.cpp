// Copyright 2013 A. Douglas Gale
// Permission is granted to use the fastcoroutine implementation
// for any use (including commercial use), provided this copyright
// notice is present in your product's source code.

// fastcoroutine.cpp

#include "fastcoroutine.h"

// MINGW bug: need stdlib included first to get _aligned_malloc
#include <cstdlib>

#include <Windows.h>
#include <cstdio>
#include <malloc.h>
#include <map>
#include <exception>
#include <iostream>

// Assembly language stuff:
extern "C" void TaskBootup(void *rsp);
extern "C" void SwitchToTask(FastCoroutine::SavedContextFrame *rsp,
    FastCoroutine::SavedContextFrame **old_task_rsp);
//extern "C" void SwitchToNextTask();
extern "C" void StartNewTask();
extern "C" _NT_TIB *get_TEB();
extern "C" void task_terminate() { std::terminate(); }

// ============================================================================
//
// ============================================================================

// Since we're not *really* switching contexts, we only actually need to save
// the callee saved registers!
struct FastCoroutine::SavedContextFrame
{
  // Pair-up the xmmwords because we need to
  // prevent 16-byte alignment of xmm6
  void *rbp;
  void *xmm6lo, *xmm6hi;
  void *xmm7lo, *xmm7hi;
  void *xmm8lo, *xmm8hi;
  void *xmm9lo, *xmm9hi;
  void *xmm10lo, *xmm10hi;
  void *xmm11lo, *xmm11hi;
  void *xmm12lo, *xmm12hi;
  void *xmm13lo, *xmm13hi;
  void *xmm14lo, *xmm14hi;
  void *xmm15lo, *xmm15hi;
  void *r15; void *r14;
  void *r13; void *r12;
  void *rdi; void *rsi;
  void *rbx;
};

// ============================================================================
//
// ============================================================================

// This has an extra entry at the bottom of
// the stack to handle thread startup
struct NewFrame
{
  FastCoroutine::SavedContextFrame context;

  void (*rip)();
};

// ============================================================================
//
// ============================================================================

FastCoroutine::Task::Task(Task *owner)
  : rsp(0)
  , owner(owner)
  , stack(0)
  , state(INVALID)
{
}

FastCoroutine::Task::~Task()
{
  if (stack)
    _aligned_free(stack);
}

void FastCoroutine::Task::GetTibStackRange()
{
  // If you pretend the stack is a vector
  // StackBase = stack.end()
  // StackLimit = stack.begin()
  auto &teb = *get_TEB();
  stack = teb.StackLimit;
  stacksize = uintptr_t(teb.StackBase) - uintptr_t(stack);
}

void FastCoroutine::Task::SetTibStackRange()
{
  auto &teb = *get_TEB();
  teb.StackLimit = stack;
  teb.StackBase = (char*)stack + stacksize;
}

void FastCoroutine::Task::CreateContext(
  void (*f)(void*,void*,void*,void*),
  void *a0, void *a1,
  void *a2, void *a3)
{
  // Allocate a new stack and put the context at the end of it
  // We need to make sure that the stack is misaligned, because it
  // is always misaligned upon the call to SwitchTo*Task
  // So we add 8
  stacksize = 1<<20;
  stack = (void**)_aligned_malloc(stacksize, 16) + 0;

  // Compute the location of the starting context and get a reference to it
  NewFrame &frame = ((NewFrame*)((char*)stack + stacksize))[-1];
  memset(&frame, 0xee, sizeof(frame));

  frame.rip = StartNewTask;

  // When we're creating a new task, we own all the registers
  // in the frame, so we put all the information into those
  // and make it start at a task creation thunk

  // Converting a void * to a void (*)() is not legal
  // (but I know it works on gcc/amd64)
  static_assert(sizeof(void(*)()) == sizeof(void*),
      "Function pointer and void pointer must be same size");

  frame.context.rbx = (void*)uintptr_t(this);
  frame.context.rbp = (void*)f;
  frame.context.r12 = a0;
  frame.context.r13 = a1;
  frame.context.r14 = a2;
  frame.context.r15 = a3;
#ifndef NDEBUG
  // Fill initial SSE registers with recognizable values
  frame.context.xmm6lo  = (void*)0xEEEEEE61EEEEEE60ULL;
  frame.context.xmm6hi  = (void*)0xEEEEEE61EEEEEE60ULL;
  frame.context.xmm7lo  = (void*)0xEEEEEE71EEEEEE70ULL;
  frame.context.xmm7hi  = (void*)0xEEEEEE71EEEEEE70ULL;
  frame.context.xmm8lo  = (void*)0xEEEEEE81EEEEEE80ULL;
  frame.context.xmm8hi  = (void*)0xEEEEEE81EEEEEE80ULL;
  frame.context.xmm9lo  = (void*)0xEEEEEE91EEEEEE90ULL;
  frame.context.xmm9hi  = (void*)0xEEEEEE91EEEEEE90ULL;
  frame.context.xmm10lo = (void*)0xEEEEEEA1EEEEEEA0ULL;
  frame.context.xmm10hi = (void*)0xEEEEEEA1EEEEEEA0ULL;
  frame.context.xmm11lo = (void*)0xEEEEEEB1EEEEEEB0ULL;
  frame.context.xmm11hi = (void*)0xEEEEEEB1EEEEEEB0ULL;
  frame.context.xmm12lo = (void*)0xEEEEEEC1EEEEEEC0ULL;
  frame.context.xmm12hi = (void*)0xEEEEEEC1EEEEEEC0ULL;
  frame.context.xmm13lo = (void*)0xEEEEEED1EEEEEED0ULL;
  frame.context.xmm13hi = (void*)0xEEEEEED1EEEEEED0ULL;
  frame.context.xmm14lo = (void*)0xEEEEEEE1EEEEEEE0ULL;
  frame.context.xmm14hi = (void*)0xEEEEEEE1EEEEEEE0ULL;
  frame.context.xmm15lo = (void*)0xEEEEEEF1EEEEEEF0ULL;
  frame.context.xmm15hi = (void*)0xEEEEEEF1EEEEEEF0ULL;
#endif
  rsp = &frame.context;
  state = Task::CREATED;
}

void FastCoroutine::Task::SwitchTo(Task &outgoing_task)
{
  outgoing_task.GetTibStackRange();
  SetTibStackRange();
  // Call assembly code:
  SwitchToTask(rsp, &outgoing_task.rsp);
}

FastCoroutine::CoroutineCanceled::CoroutineCanceled()
  : std::exception()
{
}

const char *FastCoroutine::CoroutineCanceled::what()
{
  return "Coroutine canceled";
}

// ============================================================================
// YieldBuffer<T>
// ============================================================================

template<typename Y>
FastCoroutine::YieldBuffer<Y>::YieldBuffer(Enumerator<Y> &owner)
  : owner(owner)
{
}

template<typename Y> template<typename R>
typename std::enable_if<std::is_convertible<R,Y>::value>::type
FastCoroutine::YieldBuffer<Y>::YieldReturn(R &&result)
{
  buffer = std::forward<R>(result);
  owner.ReturnToOwner();
}

// ============================================================================
// Enumerator<Y>
// ============================================================================

// Thunk converts C interface back to object reference
template<typename Y>
void FastCoroutine::Enumerator<Y>::StartupThunk(void *a, void *, void *, void *)
{
  Enumerator &self = *reinterpret_cast<Enumerator*>(a);
  try
  {
    self.routine(self.buffer);
  }
  catch (CoroutineCanceled)
  {
  }
  self.done = true;
  self.ReturnToOwner();
}

template<typename Y>
template<typename R>
FastCoroutine::Enumerator<Y>::Enumerator(R &&routine,
  typename std::enable_if<
    std::is_convertible<R,RoutineType>::value
  >::type *)
  : buffer(*this)
  , routine(std::forward<R>(routine))
  , started(false)
  , done(false)
  , cancel(false)
  , workerTask(0)
  , selfTask(&workerTask)
{
  // Create a task for the routine
  // We pass a function pointer to the compiler generated thunk for this yield type
  workerTask.CreateContext(StartupThunk, this, nullptr, nullptr, nullptr);
}

template<typename Y>
FastCoroutine::Enumerator<Y>::~Enumerator()
{
  if (!done && started)
  {
    // Need to force coroutine to exit

    // Setting the cancel flag causes an exception to be thrown
    // in the coroutine context.
    cancel = true;

    // The CoroutineCanceled exception is caught and execution is
    // resumed and the ReturnToCoroutine returns
    ReturnToCoroutine();
  }
}

template<typename Y>
void FastCoroutine::Enumerator<Y>::ReturnToOwner()
{
  selfTask.SwitchTo(workerTask);

  if (cancel)
    throw CoroutineCanceled();
}

template<typename Y>
void FastCoroutine::Enumerator<Y>::ReturnToCoroutine()
{
  workerTask.SwitchTo(selfTask);
}

// Returns true if the coroutine is NOT finished
template<typename Y>
bool FastCoroutine::Enumerator<Y>::Next()
{
  // Switch to the coroutine and execute
  // until it yields something or returns
  ReturnToCoroutine();
  return !done;
}

template<typename Y>
Y &FastCoroutine::Enumerator<Y>::GetYield()
{
  return buffer.buffer;
}

// ============================================================================
// Tests
// ============================================================================

using namespace FastCoroutine;

void EmptyCoroutine(YieldBuffer<int> &)
{
}

void TypicalCoroutine(YieldBuffer<int> &out)
{
  int i = 1;
  do
  {
    i <<= 1;
    out.YieldReturn(i);
  }
  while (i < (1<<20));
}

void ThrowCatchCoroutine(YieldBuffer<int> &)
{
  try
  {
    throw std::logic_error("Expected exception for testing");
  }
  catch (const std::logic_error &e)
  {
    std::cout << "Exception caught" << std::endl;
  }
}

void YieldFloatsCoroutine(YieldBuffer<float> &out)
{
  out.YieldReturn(1.0f);
  out.YieldReturn(2.0f);
}

void NestedCoroutine(YieldBuffer<int> &out)
{
  for (Enumerator<float> enumerator(YieldFloatsCoroutine); enumerator.Next(); )
  {
    out.YieldReturn((int)enumerator.GetYield());
  }
}

template<typename Y>
void TestRunToCompletion(void (*coroutine)(YieldBuffer<Y>&))
{
  for (Enumerator<Y> enumerator(coroutine); enumerator.Next(); )
  {
    std::cout << enumerator.GetYield() << std::endl;
  }
}

template<typename Y>
void TestAbandon(void (*coroutine)(YieldBuffer<Y>&))
{
  for (Enumerator<Y> enumerator(coroutine); enumerator.Next(); )
  {
    std::cout << enumerator.GetYield() << std::endl;
    break;
  }
}

template<typename Y>
void Test(void (*coroutine)(YieldBuffer<Y>&))
{
  TestRunToCompletion(coroutine);
  TestAbandon(coroutine);
}

int main()
{
  Test(EmptyCoroutine);
  Test(TypicalCoroutine);
  Test(YieldFloatsCoroutine);
  Test(NestedCoroutine);
  return 0;
}

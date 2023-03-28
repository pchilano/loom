/*
 * Copyright (c) 2018, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "oops/method.inline.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiThreadState.inline.hpp"
#include "runtime/continuation.hpp"
#include "runtime/continuationEntry.inline.hpp"
#include "runtime/continuationHelper.inline.hpp"
#include "runtime/continuationJavaClasses.inline.hpp"
#include "runtime/continuationWrapper.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/osThread.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframe_hp.hpp"

// defined in continuationFreezeThaw.cpp
extern "C" jint JNICALL CONT_isPinned0(JNIEnv* env, jobject cont_scope);

JVM_ENTRY(void, CONT_pin(JNIEnv* env, jclass cls)) {
  if (!Continuation::pin(JavaThread::thread_from_jni_environment(env))) {
     THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "pin overflow");
  }
}
JVM_END

JVM_ENTRY(void, CONT_unpin(JNIEnv* env, jclass cls)) {
  if (!Continuation::unpin(JavaThread::thread_from_jni_environment(env))) {
     THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "pin underflow");
  }
}
JVM_END

class PreemptHandshake : public AllocatingHandshakeClosure {
  JvmtiSampledObjectAllocEventCollector _jsoaec;
  Handle _cont;
  int _result;

public:
  PreemptHandshake(Handle cont) : AllocatingHandshakeClosure("PreemptHandshake"),
    _jsoaec(true),  // initialize if needed eagerly before the handshake since it might safepoint
    _cont(cont),
    _result(freeze_not_mounted) {}

  void do_thread(Thread* thr) {
    JavaThread* target = JavaThread::cast(thr);
    _result = Continuation::try_preempt(target, _cont);
  }
  int result() { return _result; }
};

JVM_ENTRY(jint, CONT_tryPreempt0(JNIEnv* env, jobject jcont, jobject jthread)) {
  JavaThread* current = thread;
  assert(current == JavaThread::current(), "should be");
  jint result = freeze_not_mounted;

  ThreadsListHandle tlh(current);
  JavaThread* target = NULL;
  bool is_alive = tlh.cv_internal_thread_to_JavaThread(jthread, &target, NULL);

  if (is_alive) {
    Handle conth(current, JNIHandles::resolve_non_null(jcont));
    PreemptHandshake ph(conth);
    Handshake::execute(&ph, target);
    result = ph.result();
  }
  return result;
}
JVM_END

#if INCLUDE_JVMTI
class JvmtiUnmountBeginMark : public StackObj {
  JavaThread* _target;
  int _preempt_result;
  bool _do_VTMS_transition;
  bool _transition_succeded;
  bool _is_vthread;

 public:
  JvmtiUnmountBeginMark(JavaThread* t, bool is_vthread) :
    _target(t), _preempt_result(freeze_pinned_native), _do_VTMS_transition(false), _transition_succeded(true), _is_vthread(is_vthread) {
    assert(!_target->is_in_VTMS_transition(), "must be");
    assert(!_target->is_suspended(), "must be");

    if (!_is_vthread) return;

    _do_VTMS_transition = java_lang_VirtualThread::notify_jvmti_events();
    if (_do_VTMS_transition) {
      _transition_succeded = JvmtiVTMSTransitionDisabler::start_VTMS_transition(JavaThread::current(), _target, _target->vthread(), /* is_mount */ false);
    }
  }
  ~JvmtiUnmountBeginMark() {
    assert(!_target->is_suspended(), "must be");

    if (!_is_vthread) return;

    if (_do_VTMS_transition) {
      if (_preempt_result == freeze_ok) {
        assert(_target->is_in_VTMS_transition(), "must be");
        _target->rebind_to_jvmti_thread_state_of(_target->threadObj());
      } else if (_transition_succeded) {
        // Undo transition
        JvmtiVTMSTransitionDisabler::finish_VTMS_transition(JavaThread::current(), _target, _target->vthread(), false);
      }
    }
  }
  bool transition_succeded()  { return _transition_succeded; }
  void set_preempt_result(int res) { _preempt_result = res; }
};

static bool is_safe_vthread_to_preempt_for_jvmti(JavaThread* target, oop vthread, oop cont) {
  assert(!target->has_pending_popframe(), "should be true; no support for vthreads yet");
  JvmtiThreadState* state = target->jvmti_thread_state();
  assert(state == nullptr || !state->is_earlyret_pending(), "should be true; no support for vthreads yet");

  if (!java_lang_VirtualThread::notify_jvmti_events()) {
    return true;
  }
  if (target->is_in_VTMS_transition()) {
    // We caught target at the end of a mount transition.
    return false;
  }
  if (target->is_suspended()) {
    // If we preempt while target is suspended, the resumer will later block in the
    // JvmtiVTMSTransitionDisabler waiting for the target to call finish_VTMS_transition(), while the
    // target in turn will be waiting for the resumer to resume it.
    // Target suspended implies mounted vthread suspended (see JvmtiEnvBase::suspend_thread) and we
    // would like to assert that. But the resumer could have just resumed the vthread and be now
    // waiting to handshake the target to resume it.
    return false;
  }
  return true;
}
#endif

static bool is_safe_vthread_to_preempt(JavaThread* target, oop cont) {
  oop vthread = target->vthread();
  assert(vthread != NULL, "vthread should be always set");
  if (java_lang_VirtualThread::state(vthread) != java_lang_VirtualThread::RUNNING ||  // in unmounting transition
      !java_lang_VirtualThread::is_instance(vthread) ||                               // in mounting transition after voluntary yield
      java_lang_VirtualThread::is_preemption_disabled(vthread)) {                     // temp switch to carrier thread or at jvmti_mount_end in thaw_slow()
    return false;
  }
  assert(java_lang_VirtualThread::continuation(vthread) == cont, "invalid continuation");
  return JVMTI_ONLY(is_safe_vthread_to_preempt_for_jvmti(target, vthread, cont)) NOT_JVMTI(true);
}

static bool is_safe_pc_to_preempt(address pc, JavaThread* target) {
  if (Interpreter::contains(pc)) {
    InterpreterCodelet* codelet = Interpreter::codelet_containing(pc);
    if (codelet == nullptr) {
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: no codelet (unsafe)");
      return false;
    }
    // We allow preemption only when at a safepoint codelet or a return byteocde
    if (codelet->bytecode() >= 0 && Bytecodes::is_return(codelet->bytecode())) {
      assert(codelet->kind() == InterpreterCodelet::codelet_bytecode, "");
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: safe bytecode: %s", Bytecodes::name(codelet->bytecode()));
      return true;
    } else if (codelet->kind() == InterpreterCodelet::codelet_safepoint_entry) {
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: safepoint entry: %s", codelet->description());
      return true;
    } else {
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: %s (unsafe)", codelet->description());
      return false;
    }
  } else {
    CodeBlob* cb = CodeCache::find_blob(pc);
    if (cb->is_safepoint_stub()) {
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: safepoint stub. Return poll: %d", !target->is_at_poll_safepoint());
      return true;
    } else {
      log_trace(continuations, preempt)("is_safe_pc_to_preempt: not safepoint stub");
      return false;
    }
  }
}

static bool is_safe_to_preempt(JavaThread* target, oop continuation, bool is_vthread) {
  if (target->preempting()) {
    return false;
  }
  if (!target->has_last_Java_frame()) {
    return false;
  }
  if (target->has_pending_exception()) {
    return false;
  }
  if (!is_safe_pc_to_preempt(target->last_Java_pc(), target)) {
    return false;
  }
  if (is_vthread && !is_safe_vthread_to_preempt(target, continuation)) {
    return false;
  }
  return true;
}

typedef int (*FreezeContFnT)(JavaThread*, intptr_t*);
static uint64_t fail_counter = 0;

int Continuation::try_preempt(JavaThread* target, Handle continuation) {
  ContinuationEntry* ce = target->last_continuation();
  if (ce == nullptr) {
    return freeze_not_mounted;
  }
  oop mounted_cont = ce->cont_oop(target);
  if (mounted_cont != continuation() || is_continuation_done(mounted_cont)) {
    return freeze_not_mounted;
  }

  bool is_vthread = Continuation::continuation_scope(mounted_cont) == java_lang_VirtualThread::vthread_scope();

  // Continuation is mounted and it's not done so check if it's safe to preempt.
  if (!is_safe_to_preempt(target, mounted_cont, is_vthread)) {
    return freeze_pinned_native;
  }
  assert(!is_continuation_preempted(mounted_cont), "shouldn't be");

#if INCLUDE_JVMTI
  JvmtiUnmountBeginMark jubm(target, is_vthread);
  if (!jubm.transition_succeded()) {
    return freeze_pinned_native;
  }
#endif
  target->set_preempting(true);
  int res = CAST_TO_FN_PTR(FreezeContFnT, freeze_preempt_entry())(target, target->last_Java_sp());
  log_trace(continuations, preempt)("try_preempt: %d", res);
  JVMTI_ONLY(jubm.set_preempt_result(res);)
  if (res != freeze_ok) {
    target->set_preempting(false);
    fail_counter++;
    if (fail_counter % 500 == 0) {
      //printf("preemption failed at time: " INT64_FORMAT, (int64_t)os::javaTimeNanos());
    }
  } else {
    //printf("preemption succeded at time " INT64_FORMAT, (int64_t)os::javaTimeNanos());
  }
  return res;
}

bool Continuation::is_continuation_preempted(oop cont) {
  return jdk_internal_vm_Continuation::is_preempted(cont);
}

bool Continuation::is_continuation_done(oop cont) {
  return jdk_internal_vm_Continuation::done(cont);
}

#ifdef ASSERT
bool Continuation::verify_preemption(JavaThread* thread) {
  ContinuationEntry* cont_entry = thread->last_continuation();
  assert(cont_entry != nullptr, "");
  oop mounted_cont = cont_entry->cont_oop(thread);
  assert(is_continuation_preempted(mounted_cont), "continuation not marked preempted");
  assert(thread->last_Java_sp() == cont_entry->entry_sp(), "wrong anchor change");
  assert(!thread->has_pending_exception(), "should not have pending exception after preemption");
  assert(!thread->has_pending_popframe(), "should not have popframe condition after preemption");
  JvmtiThreadState* state = thread->jvmti_thread_state();
  assert(state == nullptr || !state->is_earlyret_pending(), "should not have earlyret condition after preemption");
  return true;
}
#endif

#ifndef PRODUCT
static jlong java_tid(JavaThread* thread) {
  return java_lang_Thread::thread_id(thread->threadObj());
}
#endif

ContinuationEntry* Continuation::get_continuation_entry_for_continuation(JavaThread* thread, oop continuation) {
  if (thread == nullptr || continuation == nullptr) {
    return nullptr;
  }

  for (ContinuationEntry* entry = thread->last_continuation(); entry != nullptr; entry = entry->parent()) {
    if (continuation == entry->cont_oop(thread)) {
      return entry;
    }
  }
  return nullptr;
}

static bool is_on_stack(JavaThread* thread, const ContinuationEntry* entry) {
  if (entry == nullptr) {
    return false;
  }

  assert(thread->is_in_full_stack((address)entry), "");
  return true;
  // return false if called when transitioning to Java on return from freeze
  // return !thread->has_last_Java_frame() || thread->last_Java_sp() < cont->entry_sp();
}

bool Continuation::is_continuation_mounted(JavaThread* thread, oop continuation) {
  return is_on_stack(thread, get_continuation_entry_for_continuation(thread, continuation));
}

// When walking the virtual stack, this method returns true
// iff the frame is a thawed continuation frame whose
// caller is still frozen on the h-stack.
// The continuation object can be extracted from the thread.
bool Continuation::is_cont_barrier_frame(const frame& f) {
  assert(f.is_interpreted_frame() || f.cb() != nullptr, "");
  if (!Continuations::enabled()) return false;
  return is_return_barrier_entry(f.is_interpreted_frame() ? ContinuationHelper::InterpretedFrame::return_pc(f)
                                                          : ContinuationHelper::CompiledFrame::return_pc(f));
}

bool Continuation::is_return_barrier_entry(const address pc) {
  if (!Continuations::enabled()) return false;
  return pc == StubRoutines::cont_returnBarrier();
}

bool Continuation::is_continuation_enterSpecial(const frame& f) {
  if (f.cb() == nullptr || !f.cb()->is_compiled()) {
    return false;
  }
  Method* m = f.cb()->as_compiled_method()->method();
  return (m != nullptr && m->is_continuation_enter_intrinsic());
}

bool Continuation::is_continuation_entry_frame(const frame& f, const RegisterMap *map) {
  // we can do this because the entry frame is never inlined
  Method* m = (map != nullptr && map->in_cont() && f.is_interpreted_frame())
                  ? map->stack_chunk()->interpreter_frame_method(f)
                  : ContinuationHelper::Frame::frame_method(f);
  return m != nullptr && m->intrinsic_id() == vmIntrinsics::_Continuation_enter;
}

// The parameter `sp` should be the actual sp and not the unextended sp because at
// least on PPC64 unextended_sp < sp is possible as interpreted frames are trimmed
// to the actual size of the expression stack before calls. The problem there is
// that even unextended_sp < entry_sp < sp is possible for an interpreted frame.
static inline bool is_sp_in_continuation(const ContinuationEntry* entry, intptr_t* const sp) {
  // entry_sp() returns the unextended_sp which is always greater or equal to the actual sp
  return entry->entry_sp() > sp;
}

bool Continuation::is_frame_in_continuation(const ContinuationEntry* entry, const frame& f) {
  return is_sp_in_continuation(entry, f.sp());
}

ContinuationEntry* Continuation::get_continuation_entry_for_sp(JavaThread* thread, intptr_t* const sp) {
  assert(thread != nullptr, "");
  ContinuationEntry* entry = thread->last_continuation();
  while (entry != nullptr && !is_sp_in_continuation(entry, sp)) {
    entry = entry->parent();
  }
  return entry;
}

ContinuationEntry* Continuation::get_continuation_entry_for_entry_frame(JavaThread* thread, const frame& f) {
  assert(is_continuation_enterSpecial(f), "");
  ContinuationEntry* entry = (ContinuationEntry*)f.unextended_sp();
  assert(entry == get_continuation_entry_for_sp(thread, f.sp()-2), "mismatched entry");
  return entry;
}

bool Continuation::is_frame_in_continuation(JavaThread* thread, const frame& f) {
  return f.is_heap_frame() || (get_continuation_entry_for_sp(thread, f.sp()) != nullptr);
}

static frame continuation_top_frame(const ContinuationWrapper& cont, RegisterMap* map) {
  stackChunkOop chunk = cont.last_nonempty_chunk();
  map->set_stack_chunk(chunk);
  return chunk != nullptr ? chunk->top_frame(map) : frame();
}

bool Continuation::has_last_Java_frame(oop continuation, frame* frame, RegisterMap* map) {
  ContinuationWrapper cont(continuation);
  if (!cont.is_empty()) {
    *frame = continuation_top_frame(cont, map);
    return true;
  } else {
    return false;
  }
}

frame Continuation::last_frame(oop continuation, RegisterMap *map) {
  assert(map != nullptr, "a map must be given");
  return continuation_top_frame(ContinuationWrapper(continuation), map);
}

frame Continuation::top_frame(const frame& callee, RegisterMap* map) {
  assert(map != nullptr, "");
  ContinuationEntry* ce = get_continuation_entry_for_sp(map->thread(), callee.sp());
  assert(ce != nullptr, "");
  oop continuation = ce->cont_oop(map->thread());
  ContinuationWrapper cont(continuation);
  return continuation_top_frame(cont, map);
}

javaVFrame* Continuation::last_java_vframe(Handle continuation, RegisterMap *map) {
  assert(map != nullptr, "a map must be given");
  if (!ContinuationWrapper(continuation()).is_empty()) {
    frame f = last_frame(continuation(), map);
    for (vframe* vf = vframe::new_vframe(&f, map, nullptr); vf; vf = vf->sender()) {
      if (vf->is_java_frame()) {
        return javaVFrame::cast(vf);
      }
    }
  }
  return nullptr;
}

frame Continuation::continuation_parent_frame(RegisterMap* map) {
  assert(map->in_cont(), "");
  ContinuationWrapper cont(map);
  assert(map->thread() != nullptr || !cont.is_mounted(), "");

  log_develop_trace(continuations)("continuation_parent_frame");
  if (map->update_map()) {
    // we need to register the link address for the entry frame
    if (cont.entry() != nullptr) {
      cont.entry()->update_register_map(map);
    } else {
      map->clear();
    }
  }

  if (!cont.is_mounted()) { // When we're walking an unmounted continuation and reached the end
    oop parent = jdk_internal_vm_Continuation::parent(cont.continuation());
    stackChunkOop chunk = parent != nullptr ? ContinuationWrapper(parent).last_nonempty_chunk() : nullptr;
    if (chunk != nullptr) {
      return chunk->top_frame(map);
    }

    map->set_stack_chunk(nullptr);
    return frame();
  }

  map->set_stack_chunk(nullptr);

#if (defined(X86) || defined(AARCH64) || defined(RISCV64) || defined(PPC64)) && !defined(ZERO)
  frame sender(cont.entrySP(), cont.entryFP(), cont.entryPC());
#else
  frame sender = frame();
  Unimplemented();
#endif

  return sender;
}

oop Continuation::continuation_scope(oop continuation) {
  return continuation != nullptr ? jdk_internal_vm_Continuation::scope(continuation) : nullptr;
}

bool Continuation::is_scope_bottom(oop cont_scope, const frame& f, const RegisterMap* map) {
  if (cont_scope == nullptr || !is_continuation_entry_frame(f, map)) {
    return false;
  }

  oop continuation;
  if (map->in_cont()) {
    continuation = map->cont();
  } else {
    ContinuationEntry* ce = get_continuation_entry_for_sp(map->thread(), f.sp());
    if (ce == nullptr) {
      return false;
    }
    continuation = ce->cont_oop(map->thread());
  }
  if (continuation == nullptr) {
    return false;
  }

  oop sc = continuation_scope(continuation);
  assert(sc != nullptr, "");
  return sc == cont_scope;
}

bool Continuation::is_in_usable_stack(address addr, const RegisterMap* map) {
  ContinuationWrapper cont(map);
  stackChunkOop chunk = cont.find_chunk_by_address(addr);
  return chunk != nullptr ? chunk->is_usable_in_chunk(addr) : false;
}

bool Continuation::pin(JavaThread* current) {
  ContinuationEntry* ce = current->last_continuation();
  if (ce == nullptr) {
    return true; // no continuation mounted
  }
  return ce->pin();
}

bool Continuation::unpin(JavaThread* current) {
  ContinuationEntry* ce = current->last_continuation();
  if (ce == nullptr) {
    return true; // no continuation mounted
  }
  return ce->unpin();
}

frame Continuation::continuation_bottom_sender(JavaThread* thread, const frame& callee, intptr_t* sender_sp) {
  assert (thread != nullptr, "");
  ContinuationEntry* ce = get_continuation_entry_for_sp(thread, callee.sp());
  assert(ce != nullptr, "callee.sp(): " INTPTR_FORMAT, p2i(callee.sp()));

  log_develop_debug(continuations)("continuation_bottom_sender: [" JLONG_FORMAT "] [%d] callee: " INTPTR_FORMAT
    " sender_sp: " INTPTR_FORMAT,
    java_tid(thread), thread->osthread()->thread_id(), p2i(callee.sp()), p2i(sender_sp));

  frame entry = ce->to_frame();
  if (callee.is_interpreted_frame()) {
    entry.set_sp(sender_sp); // sp != unextended_sp
  }
  return entry;
}

address Continuation::get_top_return_pc_post_barrier(JavaThread* thread, address pc) {
  ContinuationEntry* ce;
  if (thread != nullptr && is_return_barrier_entry(pc) && (ce = thread->last_continuation()) != nullptr) {
    return ce->entry_pc();
  }
  return pc;
}

void Continuation::set_cont_fastpath_thread_state(JavaThread* thread) {
  assert(thread != nullptr, "");
  bool fast = !thread->is_interp_only_mode();
  thread->set_cont_fastpath_thread_state(fast);
}

void Continuation::notify_deopt(JavaThread* thread, intptr_t* sp) {
  ContinuationEntry* entry = thread->last_continuation();

  if (entry == nullptr) {
    return;
  }

  if (is_sp_in_continuation(entry, sp)) {
    thread->push_cont_fastpath(sp);
    return;
  }

  ContinuationEntry* prev;
  do {
    prev = entry;
    entry = entry->parent();
  } while (entry != nullptr && !is_sp_in_continuation(entry, sp));

  if (entry == nullptr) {
    return;
  }
  assert(is_sp_in_continuation(entry, sp), "");
  if (sp > prev->parent_cont_fastpath()) {
    prev->set_parent_cont_fastpath(sp);
  }
}

#ifndef PRODUCT
void Continuation::describe(FrameValues &values) {
  JavaThread* thread = JavaThread::active();
  if (thread != nullptr) {
    for (ContinuationEntry* ce = thread->last_continuation(); ce != nullptr; ce = ce->parent()) {
      intptr_t* bottom = ce->entry_sp();
      if (bottom != nullptr) {
        values.describe(-1, bottom, "continuation entry");
      }
    }
  }
}
#endif

#ifdef ASSERT
void Continuation::debug_verify_continuation(oop contOop) {
  if (!VerifyContinuations) {
    return;
  }
  assert(contOop != nullptr, "");
  assert(oopDesc::is_oop(contOop), "");
  ContinuationWrapper cont(contOop);

  assert(oopDesc::is_oop_or_null(cont.tail()), "");
  assert(cont.chunk_invariant(), "");

  bool nonempty_chunk = false;
  size_t max_size = 0;
  int num_chunks = 0;
  int num_frames = 0;
  int num_interpreted_frames = 0;
  int num_oops = 0;

  for (stackChunkOop chunk = cont.tail(); chunk != nullptr; chunk = chunk->parent()) {
    log_develop_trace(continuations)("debug_verify_continuation chunk %d", num_chunks);
    chunk->verify(&max_size, &num_oops, &num_frames, &num_interpreted_frames);
    if (!chunk->is_empty()) {
      nonempty_chunk = true;
    }
    num_chunks++;
  }

  const bool is_empty = cont.is_empty();
  assert(!nonempty_chunk || !is_empty, "");
  assert(is_empty == (!nonempty_chunk && cont.last_frame().is_empty()), "");
}

void Continuation::print(oop continuation) { print_on(tty, continuation); }

void Continuation::print_on(outputStream* st, oop continuation) {
  ContinuationWrapper cont(continuation);

  st->print_cr("CONTINUATION: " PTR_FORMAT " done: %d",
    continuation->identity_hash(), jdk_internal_vm_Continuation::done(continuation));
  st->print_cr("CHUNKS:");
  for (stackChunkOop chunk = cont.tail(); chunk != nullptr; chunk = chunk->parent()) {
    st->print("* ");
    chunk->print_on(true, st);
  }
}
#endif // ASSERT


void continuations_init() { Continuations::init(); }

void Continuations::init() {
  Continuation::init();
}

bool Continuations::enabled() {
  return VMContinuations;
}

#define CC (char*)  /*cast a literal from (const char*)*/
#define FN_PTR(f) CAST_FROM_FN_PTR(void*, &f)

static JNINativeMethod CONT_methods[] = {
    {CC"pin",              CC"()V",                                    FN_PTR(CONT_pin)},
    {CC"unpin",            CC"()V",                                    FN_PTR(CONT_unpin)},
    {CC"isPinned0",        CC"(Ljdk/internal/vm/ContinuationScope;)I", FN_PTR(CONT_isPinned0)},
    {CC"tryPreempt0",      CC"(Ljava/lang/Thread;)I",                  FN_PTR(CONT_tryPreempt0)},
};

void CONT_RegisterNativeMethods(JNIEnv *env, jclass cls) {
    JavaThread* thread = JavaThread::current();
    ThreadToNativeFromVM trans(thread);
    int status = env->RegisterNatives(cls, CONT_methods, sizeof(CONT_methods)/sizeof(JNINativeMethod));
    guarantee(status == JNI_OK, "register jdk.internal.vm.Continuation natives");
    guarantee(!env->ExceptionOccurred(), "register jdk.internal.vm.Continuation natives");
}

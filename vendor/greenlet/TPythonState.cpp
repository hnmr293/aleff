#ifndef GREENLET_PYTHON_STATE_CPP
#define GREENLET_PYTHON_STATE_CPP

#include <frameobject.h>

#include <Python.h>
#include "TGreenlet.hpp"

namespace greenlet {

PythonState::PythonState()
    : _top_frame()
#if GREENLET_USE_CFRAME
    ,cframe(nullptr)
    ,use_tracing(0)
#endif
#if GREENLET_PY314
    ,py_recursion_depth(0)
    ,cloned_current_frame_stacktop(-1)
    ,current_executor(nullptr)
    ,stackpointer(nullptr)
    #ifdef Py_GIL_DISABLED
    ,c_stack_refs(nullptr)
    #endif
#elif GREENLET_PY312
    ,py_recursion_depth(0)
    ,c_recursion_depth(0)
    ,cloned_current_frame_stacktop(-1)
#else
    ,recursion_depth(0)
#endif
#if GREENLET_PY313
    ,delete_later(nullptr)
    ,critical_section(0)
#else
    ,trash_delete_nesting(0)
#endif
#if GREENLET_PY311
    ,current_frame(nullptr)
    ,datastack_chunk(nullptr)
    ,datastack_top(nullptr)
    ,datastack_limit(nullptr)
#endif
{
#if GREENLET_USE_CFRAME
    /*
      The PyThreadState->cframe pointer usually points to memory on
      the stack, alloceted in a call into PyEval_EvalFrameDefault.

      Initially, before any evaluation begins, it points to the
      initial PyThreadState object's ``root_cframe`` object, which is
      statically allocated for the lifetime of the thread.

      A greenlet can last for longer than a call to
      PyEval_EvalFrameDefault, so we can't set its ``cframe`` pointer
      to be the current ``PyThreadState->cframe``; nor could we use
      one from the greenlet parent for the same reason. Yet a further
      no: we can't allocate one scoped to the greenlet and then
      destroy it when the greenlet is deallocated, because inside the
      interpreter the _PyCFrame objects form a linked list, and that too
      can result in accessing memory beyond its dynamic lifetime (if
      the greenlet doesn't actually finish before it dies, its entry
      could still be in the list).

      Using the ``root_cframe`` is problematic, though, because its
      members are never modified by the interpreter and are set to 0,
      meaning that its ``use_tracing`` flag is never updated. We don't
      want to modify that value in the ``root_cframe`` ourself: it
      *shouldn't* matter much because we should probably never get
      back to the point where that's the only cframe on the stack;
      even if it did matter, the major consequence of an incorrect
      value for ``use_tracing`` is that if its true the interpreter
      does some extra work --- however, it's just good code hygiene.

      Our solution: before a greenlet runs, after its initial
      creation, it uses the ``root_cframe`` just to have something to
      put there. However, once the greenlet is actually switched to
      for the first time, ``g_initialstub`` (which doesn't actually
      "return" while the greenlet is running) stores a new _PyCFrame on
      its local stack, and copies the appropriate values from the
      currently running _PyCFrame; this is then made the _PyCFrame for the
      newly-minted greenlet. ``g_initialstub`` then proceeds to call
      ``glet.run()``, which results in ``PyEval_...`` adding the
      _PyCFrame to the list. Switches continue as normal. Finally, when
      the greenlet finishes, the call to ``glet.run()`` returns and
      the _PyCFrame is taken out of the linked list and the stack value
      is now unused and free to expire.

      XXX: I think we can do better. If we're deallocing in the same
      thread, can't we traverse the list and unlink our frame?
      Can we just keep a reference to the thread state in case we
      dealloc in another thread? (Is that even possible if we're still
      running and haven't returned from g_initialstub?)
    */
    this->cframe = &PyThreadState_GET()->root_cframe;
#endif
}

int
PythonState::clone_from(
    const PythonState& other,
    const StackState& source_stack,
    StackState& target_stack
) noexcept
{
#if !GREENLET_PY312
    (void)other;
    (void)source_stack;
    (void)target_stack;
    return -1;
#else
    this->_context = other._context;
#if GREENLET_USE_CFRAME
    this->cframe = other.cframe;
    this->use_tracing = other.use_tracing;
#endif
    this->py_recursion_depth = other.py_recursion_depth;
#if GREENLET_PY314
    this->current_executor = other.current_executor;
#else
    this->c_recursion_depth = other.c_recursion_depth;
#endif
#if GREENLET_PY313
    this->delete_later = Py_XNewRef(other.delete_later);
    this->critical_section = other.critical_section;
#else
    this->trash_delete_nesting = other.trash_delete_nesting;
#endif

    struct ChunkPair {
        _PyStackChunk* source;
        _PyStackChunk* clone;
    };
    struct FramePair {
        PyFrameObject* source;
        PyFrameObject* clone;
        _PyInterpreterFrame* source_iframe;
        _PyInterpreterFrame* clone_iframe;
    };

    size_t chunk_count = 0;
    for (_PyStackChunk* chunk = other.datastack_chunk;
         chunk != nullptr;
         chunk = chunk->previous) {
        ++chunk_count;
    }

    size_t frame_count = 0;
    _PyInterpreterFrame* source_iframe = other.current_frame;
    while (source_iframe != nullptr) {
        _PyInterpreterFrame iframe_copy;
        source_stack.copy_from_stack(&iframe_copy, source_iframe, sizeof(iframe_copy));
        ++frame_count;
        source_iframe = iframe_copy.previous;
    }

    ChunkPair* chunks = static_cast<ChunkPair*>(
        PyMem_Calloc(chunk_count, sizeof(*chunks)));
    FramePair* frames = static_cast<FramePair*>(
        PyMem_Calloc(frame_count, sizeof(*frames)));
    if ((chunk_count && !chunks) || (frame_count && !frames)) {
        PyMem_Free(chunks);
        PyMem_Free(frames);
        PyErr_NoMemory();
        return -1;
    }

    PyObjectArenaAllocator allocator;
    PyObject_GetArenaAllocator(&allocator);
    size_t chunk_index = 0;
    for (_PyStackChunk* chunk = other.datastack_chunk;
         chunk != nullptr;
         chunk = chunk->previous) {
        chunks[chunk_index].source = chunk;
        chunks[chunk_index].clone = static_cast<_PyStackChunk*>(
            allocator.alloc(allocator.ctx, chunk->size));
        if (!chunks[chunk_index].clone) {
            while (chunk_index) {
                --chunk_index;
                allocator.free(
                    allocator.ctx,
                    chunks[chunk_index].clone,
                    chunks[chunk_index].source->size
                );
            }
            PyMem_Free(chunks);
            PyMem_Free(frames);
            PyErr_NoMemory();
            return -1;
        }
        memcpy(chunks[chunk_index].clone, chunk, chunk->size);
        ++chunk_index;
    }
    for (chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        chunks[chunk_index].clone->previous =
            chunk_index + 1 < chunk_count
                ? chunks[chunk_index + 1].clone
                : nullptr;
    }

    auto relocate = [chunks, chunk_count](const void* pointer) -> void* {
        const char* address = static_cast<const char*>(pointer);
        for (size_t index = 0; index < chunk_count; ++index) {
            const char* begin = reinterpret_cast<const char*>(chunks[index].source);
            const char* end = begin + chunks[index].source->size;
            if (address >= begin && address < end) {
                return reinterpret_cast<char*>(chunks[index].clone)
                    + (address - begin);
            }
        }
        return const_cast<void*>(pointer);
    };

    this->datastack_chunk = chunk_count ? chunks[0].clone : nullptr;
    this->current_frame = static_cast<_PyInterpreterFrame*>(
        relocate(other.current_frame));
    this->datastack_top = static_cast<PyObject**>(relocate(other.datastack_top));
    this->datastack_limit = static_cast<PyObject**>(relocate(other.datastack_limit));

    auto find_frame = [frames, frame_count](PyFrameObject* source) -> PyFrameObject* {
        for (size_t index = 0; index < frame_count; ++index) {
            if (frames[index].source == source) {
                return frames[index].clone;
            }
        }
        return nullptr;
    };

    auto free_chunks = [&]() {
        _PyStackChunk* chunk = this->datastack_chunk;
        while (chunk != nullptr) {
            _PyStackChunk* previous = chunk->previous;
            chunk->previous = nullptr;
            allocator.free(allocator.ctx, chunk, chunk->size);
            chunk = previous;
        }
        this->datastack_chunk = nullptr;
        this->datastack_top = nullptr;
        this->datastack_limit = nullptr;
        this->current_frame = nullptr;
    };

    auto clear_frames = [&](size_t count) {
        for (size_t index = 0; index < count; ++index) {
            _PyInterpreterFrame* iframe = frames[index].clone_iframe;
#if GREENLET_PY314
            const int iframe_stacktop = static_cast<int>(
                iframe->stackpointer - iframe->localsplus);
            for (int slot = 0; slot < iframe_stacktop; ++slot) {
                PyStackRef_XCLOSE(iframe->localsplus[slot]);
                iframe->localsplus[slot] = PyStackRef_NULL;
            }
#else
            for (int slot = 0; slot < iframe->stacktop; ++slot) {
                Py_XDECREF(iframe->localsplus[slot]);
                iframe->localsplus[slot] = nullptr;
            }
#endif
            Py_XDECREF(iframe->f_locals);
#if GREENLET_PY314
            PyStackRef_XCLOSE(iframe->f_funcobj);
            PyStackRef_XCLOSE(iframe->f_executable);
#else
            Py_XDECREF(iframe->f_funcobj);
#if GREENLET_PY313
            Py_XDECREF(iframe->f_executable);
#else
            Py_XDECREF(iframe->f_code);
#endif
#endif
            Py_XDECREF(iframe->frame_obj);
            iframe->f_locals = nullptr;
#if GREENLET_PY314
            iframe->f_funcobj = PyStackRef_NULL;
            iframe->f_executable = PyStackRef_NULL;
#else
            iframe->f_funcobj = nullptr;
#if GREENLET_PY313
            iframe->f_executable = nullptr;
#else
            iframe->f_code = nullptr;
#endif
#endif
            iframe->frame_obj = nullptr;
        }
    };

    size_t frame_index = 0;
    source_iframe = other.current_frame;
    while (source_iframe != nullptr) {
        _PyInterpreterFrame source_copy;
        source_stack.copy_from_stack(&source_copy, source_iframe, sizeof(source_copy));
        _PyInterpreterFrame* clone_iframe = static_cast<_PyInterpreterFrame*>(
            relocate(source_iframe));
        if (clone_iframe != source_iframe) {
#if GREENLET_PY314
            PyCodeObject* source_code = _PyFrame_GetCode(&source_copy);
#elif GREENLET_PY313
            PyCodeObject* source_code = _PyFrame_GetCode(&source_copy);
#else
            PyCodeObject* source_code = source_copy.f_code;
#endif
            const int frame_slots = _PyFrame_NumSlotsForCodeObject(source_code);
#if GREENLET_PY314
            int live_stacktop = source_copy.stackpointer == nullptr
                ? -1
                : static_cast<int>(source_copy.stackpointer - source_iframe->localsplus);
#else
            int live_stacktop = source_copy.stacktop;
#endif
            if (live_stacktop < 0) {
                const void* stack_pointer = source_stack.find_pointer_in_range(
                    source_iframe->localsplus + source_code->co_nlocalsplus,
                    source_iframe->localsplus + frame_slots
                );
                if (!stack_pointer) {
                    clear_frames(frame_index);
                    free_chunks();
                    PyMem_Free(chunks);
                    PyMem_Free(frames);
                    PyErr_SetString(
                        PyExc_RuntimeError,
                        "cannot locate the active Python value-stack pointer"
                    );
                    return -1;
                }
                live_stacktop = static_cast<int>(
#if GREENLET_PY314
                    static_cast<_PyStackRef const*>(stack_pointer)
                    - source_iframe->localsplus
#else
                    static_cast<PyObject* const*>(stack_pointer)
                    - source_iframe->localsplus
#endif
                );
            }
            frames[frame_index].source = nullptr;
            frames[frame_index].clone = nullptr;
            frames[frame_index].source_iframe = source_iframe;
            frames[frame_index].clone_iframe = clone_iframe;
            clone_iframe->previous = static_cast<_PyInterpreterFrame*>(
                relocate(source_copy.previous));
#if GREENLET_PY314
            clone_iframe->visited = 0;
#endif
#if GREENLET_PY314
            clone_iframe->f_executable = PyStackRef_DUP(source_copy.f_executable);
#elif GREENLET_PY313
            clone_iframe->f_executable = Py_XNewRef(source_copy.f_executable);
#else
            clone_iframe->f_code = reinterpret_cast<PyCodeObject*>(
                Py_XNewRef(reinterpret_cast<PyObject*>(source_copy.f_code)));
#endif
#if GREENLET_PY314
            clone_iframe->f_funcobj = PyStackRef_DUP(source_copy.f_funcobj);
#else
            clone_iframe->f_funcobj = Py_XNewRef(source_copy.f_funcobj);
#endif
            clone_iframe->f_locals = Py_XNewRef(source_copy.f_locals);
            clone_iframe->frame_obj = nullptr;
            for (int slot = 0; slot < live_stacktop; ++slot) {
#if GREENLET_PY314
                _PyStackRef source_value;
                source_stack.copy_from_stack(
                    &source_value,
                    source_iframe->localsplus + slot,
                    sizeof(source_value)
                );
                clone_iframe->localsplus[slot] = PyStackRef_DUP(source_value);
#else
                PyObject* source_value;
                source_stack.copy_from_stack(
                    &source_value,
                    source_iframe->localsplus + slot,
                    sizeof(source_value)
                );
                clone_iframe->localsplus[slot] = Py_XNewRef(source_value);
#endif
            }
            for (int slot = live_stacktop; slot < frame_slots; ++slot) {
#if GREENLET_PY314
                clone_iframe->localsplus[slot] = PyStackRef_NULL;
#else
                clone_iframe->localsplus[slot] = nullptr;
#endif
            }
#if GREENLET_PY314
            clone_iframe->stackpointer = clone_iframe->localsplus + live_stacktop;
            if (source_copy.stackpointer == nullptr) {
                this->cloned_current_frame_stacktop = live_stacktop;
            }
#else
            if (source_copy.stacktop < 0) {
                clone_iframe->stacktop = live_stacktop;
                this->cloned_current_frame_stacktop = live_stacktop;
            }
#endif
            if (source_copy.frame_obj) {
                PyFrameObject* clone_frame = PyFrame_New(
                    PyThreadState_GET(),
                    source_code,
                    clone_iframe->f_globals,
                    clone_iframe->f_locals
                );
                if (!clone_frame) {
                    clear_frames(frame_index + 1);
                    free_chunks();
                    PyMem_Free(chunks);
                    PyMem_Free(frames);
                    return -1;
                }
                _PyInterpreterFrame* unused = clone_frame->f_frame;
#if GREENLET_PY314
                PyStackRef_XCLOSE(unused->f_funcobj);
                PyStackRef_XCLOSE(unused->f_executable);
#else
                Py_XDECREF(unused->f_funcobj);
#if GREENLET_PY313
                Py_XDECREF(unused->f_executable);
#else
                Py_XDECREF(unused->f_code);
#endif
                unused->f_funcobj = nullptr;
#endif
                Py_XDECREF(unused->f_locals);
#if GREENLET_PY314
                unused->f_funcobj = PyStackRef_NULL;
                unused->f_executable = PyStackRef_NULL;
#else
#if GREENLET_PY313
                unused->f_executable = nullptr;
#else
                unused->f_code = nullptr;
#endif
#endif
                unused->f_locals = nullptr;
                unused->frame_obj = nullptr;
                clone_frame->f_frame = clone_iframe;
                clone_frame->f_trace = Py_XNewRef(source_copy.frame_obj->f_trace);
                clone_frame->f_trace_lines = source_copy.frame_obj->f_trace_lines;
                clone_frame->f_trace_opcodes = source_copy.frame_obj->f_trace_opcodes;
#if !GREENLET_PY313
                clone_frame->f_fast_as_locals = source_copy.frame_obj->f_fast_as_locals;
#else
                clone_frame->f_extra_locals =
                    Py_XNewRef(source_copy.frame_obj->f_extra_locals);
                clone_frame->f_locals_cache =
                    Py_XNewRef(source_copy.frame_obj->f_locals_cache);
#if GREENLET_PY314
                clone_frame->f_overwritten_fast_locals =
                    Py_XNewRef(source_copy.frame_obj->f_overwritten_fast_locals);
#endif
#endif
                clone_frame->f_lineno = source_copy.frame_obj->f_lineno;
                // expose_frames() temporarily stores the interpreter frame's
                // original previous pointer in the frame object's spare inline
                // storage.  The cloned frame object must carry that saved link;
                // otherwise unexpose_frames() restores a null previous pointer
                // and RETURN_VALUE resumes through a null caller frame.
                void* saved_previous = nullptr;
                memcpy(
                    &saved_previous,
                    &source_copy.frame_obj->_f_frame_data[0],
                    sizeof(saved_previous)
                );
                saved_previous = relocate(saved_previous);
                memcpy(
                    &clone_frame->_f_frame_data[0],
                    &saved_previous,
                    sizeof(saved_previous)
                );
                frames[frame_index].source = source_copy.frame_obj;
                frames[frame_index].clone = clone_frame;
                clone_iframe->frame_obj = clone_frame;
            }
            ++frame_index;
        }
        source_iframe = source_copy.previous;
    }

    for (size_t index = 0; index < frame_index; ++index) {
        if (!frames[index].source) {
            continue;
        }
        PyFrameObject* back = find_frame(frames[index].source->f_back);
        if (back) {
            Py_INCREF(back);
        }
        // expose_frames() deliberately cuts the suspended greenlet off from
        // frames owned by the switching origin. Never retain an outer/source
        // frame from the materialized source chain in an independent clone.
        frames[index].clone->f_back = back;
        if (frames[index].source) {
            target_stack.relocate_pointer(
                frames[index].source,
                frames[index].clone
            );
        }
    }

    if (other._top_frame) {
        PyFrameObject* clone_top = find_frame(other._top_frame.borrow());
        if (!clone_top) {
            clear_frames(frame_index);
            free_chunks();
            PyMem_Free(chunks);
            PyMem_Free(frames);
            PyErr_SetString(PyExc_RuntimeError, "cannot clone an incomplete frame chain");
            return -1;
        }
        this->_top_frame = OwnedFrame::owning(clone_top);
    }

    for (size_t index = 0; index < chunk_count; ++index) {
        target_stack.relocate_stack_range(
            chunks[index].source,
            chunks[index].source->size,
            chunks[index].clone
        );
    }
    PyMem_Free(chunks);
    PyMem_Free(frames);
    return 0;
#endif
}

void
PythonState::clear_cloned_state(const StackState& stack) noexcept
{
#if GREENLET_PY312
    auto in_data_stack = [this](const void* pointer) {
        const char* address = static_cast<const char*>(pointer);
        for (_PyStackChunk* chunk = this->datastack_chunk;
             chunk != nullptr;
             chunk = chunk->previous) {
            const char* begin = reinterpret_cast<const char*>(chunk);
            if (address >= begin && address < begin + chunk->size) {
                return true;
            }
        }
        return false;
    };

    _PyInterpreterFrame* source_iframe = this->current_frame;
    while (source_iframe != nullptr) {
        _PyInterpreterFrame iframe_copy;
        stack.copy_from_stack(&iframe_copy, source_iframe, sizeof(iframe_copy));
        _PyInterpreterFrame* iframe = in_data_stack(source_iframe)
            ? source_iframe
            : nullptr;
        if (iframe) {
#if GREENLET_PY314
            const int iframe_stacktop = static_cast<int>(
                iframe_copy.stackpointer - source_iframe->localsplus);
            for (int slot = 0; slot < iframe_stacktop; ++slot) {
                PyStackRef_XCLOSE(iframe->localsplus[slot]);
                iframe->localsplus[slot] = PyStackRef_NULL;
            }
#else
            for (int slot = 0; slot < iframe_copy.stacktop; ++slot) {
                Py_XDECREF(iframe->localsplus[slot]);
                iframe->localsplus[slot] = nullptr;
            }
#endif
            Py_XDECREF(iframe->f_locals);
#if GREENLET_PY314
            PyStackRef_XCLOSE(iframe->f_funcobj);
            PyStackRef_XCLOSE(iframe->f_executable);
#else
            Py_XDECREF(iframe->f_funcobj);
#if GREENLET_PY313
            Py_XDECREF(iframe->f_executable);
#else
            Py_XDECREF(iframe->f_code);
#endif
#endif
            Py_XDECREF(iframe->frame_obj);
            iframe->f_locals = nullptr;
#if GREENLET_PY314
            iframe->f_funcobj = PyStackRef_NULL;
            iframe->f_executable = PyStackRef_NULL;
#else
            iframe->f_funcobj = nullptr;
#if GREENLET_PY313
            iframe->f_executable = nullptr;
#else
            iframe->f_code = nullptr;
#endif
#endif
            iframe->frame_obj = nullptr;
        }
        source_iframe = iframe_copy.previous;
    }
    this->_top_frame.CLEAR();
#if GREENLET_PY313
    Py_CLEAR(this->delete_later);
#endif
#else
    (void)stack;
#endif
}

#if GREENLET_PY314 && defined(Py_GIL_DISABLED)
void PythonState::capture_c_stack_refs(const PyThreadState* tstate) noexcept
{
    // Runs from operator<< while our C stack is still live and coherent, so we
    // can walk tstate's _PyCStackRef list and take a strong reference to every
    // object it holds. tp_traverse visits these once we're suspended, because
    // by then the nodes themselves have been relocated into the heap stack copy
    // and the saved list head no longer points at them. Strong references (not
    // _Py_VISIT_STACKREF, whose _PyGC_VisitStackRef isn't exported before 3.15);
    // a std::vector rather than a Python list/tuple because operator<< must not
    // allocate a GC-tracked object mid-switch. Rebuilt from scratch each time;
    // the list is empty at a typical switch, so this is usually just an empty
    // loop.
    this->c_stack_ref_snapshot.clear();
    for (const _PyCStackRef* node = ((_PyThreadStateImpl*)tstate)->c_stack_refs;
         node != nullptr; node = node->next) {
        if (!PyStackRef_IsNullOrInt(node->ref)) {
            this->c_stack_ref_snapshot.push_back(
                OwnedObject::owning(PyStackRef_AsPyObjectBorrow(node->ref)));
        }
    }
}
#endif


inline void PythonState::may_switch_away() noexcept
{
#if GREENLET_PY311
    // PyThreadState_GetFrame is probably going to have to allocate a
    // new frame object. That may trigger garbage collection. Because
    // we call this during the early phases of a switch (it doesn't
    // matter to which greenlet, as this has a global effect), if a GC
    // triggers a switch away, two things can happen, both bad:
    // - We might not get switched back to, halting forward progress.
    //   this is pathological, but possible.
    // - We might get switched back to with a different set of
    //   arguments or a throw instead of a switch. That would corrupt
    //   our state (specifically, PyErr_Occurred() and this->args()
    //   would no longer agree).
    //
    // Thus, when we call this API, we need to have GC disabled.
    // This method serves as a bottleneck we call when maybe beginning
    // a switch. In this way, it is always safe -- no risk of GC -- to
    // use ``_GetFrame()`` whenever we need to, just as it was in
    // <=3.10 (because subsequent calls will be cached and not
    // allocate memory).

    GCDisabledGuard no_gc;
    Py_XDECREF(PyThreadState_GetFrame(PyThreadState_GET()));
#endif
}

void PythonState::operator<<(const PyThreadState *const tstate) noexcept
{
    this->_context.steal(tstate->context);
#if GREENLET_USE_CFRAME
    /*
      IMPORTANT: ``cframe`` is a pointer into the STACK. Thus, because
      the call to ``slp_switch()`` changes the contents of the stack,
      you cannot read from ``ts_current->cframe`` after that call and
      necessarily get the same values you get from reading it here.
      Anything you need to restore from now to then must be saved in a
      global/threadlocal variable (because we can't use stack
      variables here either). For things that need to persist across
      the switch, use `will_switch_from`.
    */
    this->cframe = tstate->cframe;
  #if !GREENLET_PY312
    this->use_tracing = tstate->cframe->use_tracing;
  #endif
#endif // GREENLET_USE_CFRAME
#if GREENLET_PY311
  #if GREENLET_PY314
    this->py_recursion_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    this->current_executor = tstate->current_executor;
    #ifdef Py_GIL_DISABLED
    this->c_stack_refs = ((_PyThreadStateImpl*)tstate)->c_stack_refs;
    // Capture the deferred references now, while our C stack is still live, so
    // tp_traverse can keep them from being collected while we're suspended.
    this->capture_c_stack_refs(tstate);
    #endif
  #elif GREENLET_PY312
    this->py_recursion_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    this->c_recursion_depth = Py_C_RECURSION_LIMIT - tstate->c_recursion_remaining;
  #else // not 312
    this->recursion_depth = tstate->recursion_limit - tstate->recursion_remaining;
  #endif // GREENLET_PY312
  #if GREENLET_PY313
    this->current_frame = tstate->current_frame;
  #elif GREENLET_USE_CFRAME
    this->current_frame = tstate->cframe->current_frame;
  #endif
    this->datastack_chunk = tstate->datastack_chunk;
    this->datastack_top = tstate->datastack_top;
    this->datastack_limit = tstate->datastack_limit;

    PyFrameObject *frame = PyThreadState_GetFrame((PyThreadState *)tstate);
    Py_XDECREF(frame);  // PyThreadState_GetFrame gives us a new
                        // reference.
    this->_top_frame.steal(frame);
  #if GREENLET_PY314
    if (this->top_frame()) {
        this->stackpointer = this->_top_frame->f_frame->stackpointer;
    }
    else {
        this->stackpointer = nullptr;
    }
  #endif
  #if GREENLET_PY313
    // By contract of _PyTrash_thread_deposit_object,
    // the ``delete_later`` object has a refcount of 0.
    // We take a strong reference to it.
    //
    // Now, ``delete_later`` is managed as a
    // linked list whose objects are unconditionally deallocated
    // WITHOUT calling DECREF on them, so it's not clear what that is
    // actually accomplishing. That is, if another object is pushed on
    // the list and then the list is deallocated, this object will
    // still be deallocated. This strong reference serves as a form of
    // resurrection, meaning that when operator>> DECREFs it, we might
    // enter its ``tp_dealloc`` function again.
    //
    // In practice, it's quite difficult to arrange for this to be
    // a non-null value during a greenlet switch.
    // ``greenlet.tests.test_greenlet_trash`` tries, but under 3.14,
    // at least, fails to do so.
    this->delete_later = Py_XNewRef(tstate->delete_later);
#ifdef Py_GIL_DISABLED
    // Switching greenlets swaps C stacks, which to the free-threaded runtime is
    // the same predicament as detaching the thread: the PyCriticalSection nodes
    // chained off tstate->critical_section live on the stack we're leaving, and
    // their PyMutexes would stay locked behind our back. The greenlet we switch
    // to could then block forever taking one of those same locks -- e.g. an
    // asyncio event dispatched onto another fiber re-enters a Task/Future that
    // the suspended fiber is mid-step on. So drop the locks here the way
    // _PyThreadState_Detach() does and let operator>> re-take them on resume.
    if (tstate->critical_section != 0) {
        _PyCriticalSection_SuspendAll(const_cast<PyThreadState*>(tstate));
    }
#endif
    this->critical_section = tstate->critical_section;
  #elif GREENLET_PY312
    this->trash_delete_nesting = tstate->trash.delete_nesting;
  #else // not 312 or 3.13+
    this->trash_delete_nesting = tstate->trash_delete_nesting;
  #endif // GREENLET_PY312
#else // Not 311
    this->recursion_depth = tstate->recursion_depth;
    this->_top_frame.steal(tstate->frame);
    this->trash_delete_nesting = tstate->trash_delete_nesting;
#endif // GREENLET_PY311
}

#if GREENLET_PY312
void GREENLET_NOINLINE(PythonState::unexpose_frames)()
{
    if (!this->top_frame()) {
        return;
    }

    // See GreenletState::expose_frames() and the comment on frames_were_exposed
    // for more information about this logic.
    _PyInterpreterFrame *iframe = this->_top_frame->f_frame;
    while (iframe != nullptr) {
        _PyInterpreterFrame *prev_exposed = iframe->previous;
        assert(iframe->frame_obj);
        memcpy(&iframe->previous, &iframe->frame_obj->_f_frame_data[0],
               sizeof(void *));
        iframe = prev_exposed;
    }
}
#else
void PythonState::unexpose_frames()
{}
#endif

void PythonState::operator>>(PyThreadState *const tstate) noexcept
{
    tstate->context = this->_context.relinquish_ownership();
    /* Incrementing this value invalidates the contextvars cache,
       which would otherwise remain valid across switches */
    tstate->context_ver++;
#if GREENLET_USE_CFRAME
    tstate->cframe = this->cframe;
    /*
      If we were tracing, we need to keep tracing.
      There should never be the possibility of hitting the
      root_cframe here. See note above about why we can't
      just copy this from ``origin->cframe->use_tracing``.
    */
  #if !GREENLET_PY312
    tstate->cframe->use_tracing = this->use_tracing;
  #endif
#endif // GREENLET_USE_CFRAME
#if GREENLET_PY311
  #if GREENLET_PY314
    tstate->py_recursion_remaining = tstate->py_recursion_limit - this->py_recursion_depth;
    tstate->current_executor = this->current_executor;
    if (this->cloned_current_frame_stacktop >= 0 && this->current_frame) {
#ifndef NDEBUG
        this->current_frame->stackpointer = nullptr;
#endif
        this->cloned_current_frame_stacktop = -1;
    }
    #ifdef Py_GIL_DISABLED
    ((_PyThreadStateImpl*)tstate)->c_stack_refs = this->c_stack_refs;
    // We're the running greenlet again: our C-stack refs live in the thread
    // state now and gc_visit_thread_stacks() covers them, so drop the strong
    // references tp_traverse held on our behalf while we were suspended.
    this->c_stack_ref_snapshot.clear();
    #endif
    this->unexpose_frames();
  #elif GREENLET_PY312
    tstate->py_recursion_remaining = tstate->py_recursion_limit - this->py_recursion_depth;
    tstate->c_recursion_remaining = Py_C_RECURSION_LIMIT - this->c_recursion_depth;
    if (this->cloned_current_frame_stacktop >= 0 && this->current_frame) {
        this->current_frame->stacktop = -1;
        this->cloned_current_frame_stacktop = -1;
    }
    this->unexpose_frames();
  #else // \/ 3.11
    tstate->recursion_remaining = tstate->recursion_limit - this->recursion_depth;
  #endif // GREENLET_PY312
  #if GREENLET_PY313
    tstate->current_frame = this->current_frame;
  #elif GREENLET_USE_CFRAME
    tstate->cframe->current_frame = this->current_frame;
  #endif
    tstate->datastack_chunk = this->datastack_chunk;
    tstate->datastack_top = this->datastack_top;
    tstate->datastack_limit = this->datastack_limit;
#if GREENLET_PY314 && defined(Py_GIL_DISABLED)
    if (this->top_frame()) {
        this->_top_frame->f_frame->stackpointer = this->stackpointer;
    }
#endif
    this->_top_frame.relinquish_ownership();
  #if GREENLET_PY313
    // See comments in operator<<. We own a strong reference to
    // this->delete_later, which may or may not be the same object as
    // tstate->delete_later (depending if something pushed an object
    // onto the trashcan). Again, because ``delete_later`` is managed
    // as a linked list, it's not clear that saving and restoring the
    // value, especially without ever setting it to NULL, accomplishes
    // much...but the code was added by a core dev, so assume correct.
    //
    // Recall that tstate->delete_later is supposed to have a refcount
    // of 0, because objects are added there from their ``tp_dealloc``
    // method. So we should only need to DECREF it if we're the ones
    // that INCREF'd it in operator<<. (This is different than the
    // core dev's original code which always did this.)
    if (this->delete_later == tstate->delete_later) {
        Py_XDECREF(tstate->delete_later);
        tstate->delete_later = this->delete_later;
        this->delete_later = nullptr;
    }
    else {
        // it got switched behind our back. So the reference we own
        // needs to be explicitly cleared.
        tstate->delete_later = this->delete_later;
        Py_CLEAR(this->delete_later);
    }
    tstate->critical_section = this->critical_section;
#ifdef Py_GIL_DISABLED
    // Re-acquire whatever operator<< suspended when this greenlet last yielded.
    // A no-op for a greenlet that held no locks, and for a brand-new one whose
    // chain starts empty. Mirrors the resume in _PyThreadState_Attach(); note
    // _PyCriticalSection_Resume() dereferences the head, so the != 0 guard is
    // load-bearing, not just a fast path.
    if (tstate->critical_section != 0) {
        _PyCriticalSection_Resume(tstate);
    }
#endif

  #elif GREENLET_PY312
    tstate->trash.delete_nesting = this->trash_delete_nesting;
  #else // not 3.12
    tstate->trash_delete_nesting = this->trash_delete_nesting;
  #endif // GREENLET_PY312
#else // not 3.11
    tstate->frame = this->_top_frame.relinquish_ownership();
    tstate->recursion_depth = this->recursion_depth;
    tstate->trash_delete_nesting = this->trash_delete_nesting;
#endif // GREENLET_PY311
}

inline void PythonState::will_switch_from(PyThreadState *const origin_tstate) noexcept
{
#if GREENLET_USE_CFRAME && !GREENLET_PY312
    // The weird thing is, we don't actually save this for an
    // effect on the current greenlet, it's saved for an
    // effect on the target greenlet. That is, we want
    // continuity of this setting across the greenlet switch.
    this->use_tracing = origin_tstate->cframe->use_tracing;
#endif
}

void PythonState::set_initial_state(const PyThreadState* const tstate) noexcept
{
    this->_top_frame = nullptr;
#if GREENLET_PY314
    this->py_recursion_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    this->current_executor = tstate->current_executor;
    #ifdef Py_GIL_DISABLED
    // Start with an empty C-stack-ref list, the way a brand-new thread does;
    // do NOT copy the parent thread state's head. Those _PyCStackRef nodes sit
    // on the parent greenlet's C stack, so once we start running on our own
    // stack and overwrite that region, following them reads garbage. The
    // free-threaded collector walks c_stack_refs for every thread in
    // gc_visit_thread_stacks(), so leaving the stale head here crashed it.
    // See https://github.com/python-greenlet/greenlet/issues/515.
    this->c_stack_refs = nullptr;
    #endif
    // this->stackpointer is left null because this->_top_frame is
    // null so there is no value to copy.
#elif GREENLET_PY312
    this->py_recursion_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
#if GREENLET_PY314
    this->c_recursion_depth = 0; // unused on 3.14
#else
    this->c_recursion_depth = Py_C_RECURSION_LIMIT - tstate->c_recursion_remaining;
#endif
#elif GREENLET_PY311
    this->recursion_depth = tstate->recursion_limit - tstate->recursion_remaining;
#else
    this->recursion_depth = tstate->recursion_depth;
#endif
}
// TODO: Better state management about when we own the top frame.
int PythonState::tp_traverse(visitproc visit, void* arg, bool visit_top_frame) noexcept
{
    Py_VISIT(this->_context.borrow());
    if (visit_top_frame) {
        Py_VISIT(this->_top_frame.borrow());
    }
#if GREENLET_PY315
    // Visit the references held by our suspended frames.
    // This is important specially on free-threading where the
    // the suspended frames may contain deferred references to
    // objects, and if they are not traversed then the interpreter
    // can free objects early causing a use-after-free crash
    // at runtime exit.
    if (this->_top_frame) {
        for (_PyInterpreterFrame* iframe = this->_top_frame->f_frame;
             iframe != nullptr; iframe = iframe->previous) {
            // Skip generator/coroutine frames; their object's traverse
            // already visits them (gen_traverse), so we'd double-count.
            // expose_frames leaves them in the ->previous chain.
            if (iframe->owner != FRAME_OWNED_BY_THREAD) {
                continue;
            }
            Py_VISIT(iframe->frame_obj);
            Py_VISIT(iframe->f_locals);
            _Py_VISIT_STACKREF(iframe->f_funcobj);
            _Py_VISIT_STACKREF(iframe->f_executable);
            int frame_result = _PyGC_VisitFrameStack(iframe, visit, arg);
            if (frame_result) {
                return frame_result;
            }
        }
    }
#endif
#if GREENLET_PY314 && defined(Py_GIL_DISABLED)
    // Visit the objects this greenlet's C-stack refs were holding when it
    // suspended (captured by capture_c_stack_refs). The free-threaded collector
    // only walks the running thread's _PyCStackRef list in
    // gc_visit_thread_stacks(), so without this a collection could free an
    // object reachable only through a suspended greenlet's C-stack ref and we'd
    // use it after free once the greenlet resumed. The snapshot is empty while
    // we're the running greenlet, so this is a no-op there.
    for (const OwnedObject& ref : this->c_stack_ref_snapshot) {
        Py_VISIT(ref.borrow());
    }
#endif
    // Note that we DO NOT visit ``delete_later``. Even if it's
    // non-null and we technically own a reference to it, its
    // reference count already went to 0 once and it was in the
    // process of being deallocated. The trash can mechanism linked it
    // into a list that will be cleaned at some later time, and it has
    // become untracked by the GC.
    return 0;
}

void PythonState::tp_clear(bool own_top_frame) noexcept
{
    PythonStateContext::tp_clear();
#if GREENLET_PY314 && defined(Py_GIL_DISABLED)
    this->c_stack_ref_snapshot.clear();
#endif
    // If we get here owning a frame,
    // we got dealloc'd without being finished. We may or may not be
    // in the same thread.
    if (own_top_frame) {
#if GREENLET_PY315
        // Release the references held by our suspended frames.
        // this->top_frame gets implicitly cleared by the Py_CLEAR(iframe->frame_obj)
        // of the first complete frame, so in the end we relinquish ownership of it.
        if (this->_top_frame) {
            for (_PyInterpreterFrame* iframe = this->_top_frame->f_frame;
                 iframe != nullptr; iframe = iframe->previous) {
                if (iframe->owner != FRAME_OWNED_BY_THREAD) {
                    continue;
                }
                // Clear the references held by this frame's evaluation stack.
                _PyStackRef* locals = iframe->localsplus;
                _PyStackRef* sp = iframe->stackpointer;
                if (sp) {
                    while (sp > locals) {
                        sp--;
                        PyStackRef_CLEAR(*sp);
                    }
                    iframe->stackpointer = locals;
                }
                Py_CLEAR(iframe->f_locals);
                Py_CLEAR(iframe->frame_obj);
                PyStackRef_CLEAR(iframe->f_funcobj);
                PyStackRef_CLEAR(iframe->f_executable);
            }
        }
        this->_top_frame.relinquish_ownership();
#else
        this->_top_frame.CLEAR();
#endif
    }
}

#if GREENLET_USE_CFRAME
void PythonState::set_new_cframe(_PyCFrame& frame) noexcept
{
    frame = *PyThreadState_GET()->cframe;
    /* Make the target greenlet refer to the stack value. */
    this->cframe = &frame;
    /*
      And restore the link to the previous frame so this one gets
      unliked appropriately.
    */
    this->cframe->previous = &PyThreadState_GET()->root_cframe;
}
#endif

const PythonState::OwnedFrame& PythonState::top_frame() const noexcept
{
    return this->_top_frame;
}

void PythonState::did_finish(PyThreadState* tstate) noexcept
{
#if GREENLET_PY311
    // See https://github.com/gevent/gevent/issues/1924 and
    // https://github.com/python-greenlet/greenlet/issues/328. In
    // short, Python 3.11 allocates memory for frames as a sort of
    // linked list that's kept as part of PyThreadState in the
    // ``datastack_chunk`` member and friends. These are saved and
    // restored as part of switching greenlets.
    //
    // When we initially switch to a greenlet, we set those to NULL.
    // That causes the frame management code to treat this like a
    // brand new thread and start a fresh list of chunks, beginning
    // with a new "root" chunk. As we make calls in this greenlet,
    // those chunks get added, and as calls return, they get popped.
    // But the frame code (pystate.c) is careful to make sure that the
    // root chunk never gets popped.
    //
    // Thus, when a greenlet exits for the last time, there will be at
    // least a single root chunk that we must be responsible for
    // deallocating.
    //
    // The complex part is that these chunks are allocated and freed
    // using ``_PyObject_VirtualAlloc``/``Free``. Those aren't public
    // functions, and they aren't exported for linking. It so happens
    // that we know they are just thin wrappers around the Arena
    // allocator, so we can use that directly to deallocate in a
    // compatible way.
    //
    // CAUTION: Check this implementation detail on every major version.
    //
    // It might be nice to be able to do this in our destructor, but
    // can we be sure that no one else is using that memory? Plus, as
    // described below, our pointers may not even be valid anymore. As
    // a special case, there is one time that we know we can do this,
    // and that's from the destructor of the associated UserGreenlet
    // (NOT main greenlet)
    PyObjectArenaAllocator alloc;
    _PyStackChunk* chunk = nullptr;
    if (tstate) {
        // We really did finish, we can never be switched to again.
        chunk = tstate->datastack_chunk;
        // Unfortunately, we can't do much sanity checking. Our
        // this->datastack_chunk pointer is out of date (evaluation may
        // have popped down through it already) so we can't verify that
        // we deallocate it. I don't think we can even check datastack_top
        // for the same reason.

        PyObject_GetArenaAllocator(&alloc);
        tstate->datastack_chunk = nullptr;
        tstate->datastack_limit = nullptr;
        tstate->datastack_top = nullptr;

    }
    else if (this->datastack_chunk) {
        // The UserGreenlet (NOT the main greenlet!) is being deallocated. If we're
        // still holding a stack chunk, it's garbage because we know
        // we can never switch back to let cPython clean it up.
        // Because the last time we got switched away from, and we
        // haven't run since then, we know our chain is valid and can
        // be dealloced.
        chunk = this->datastack_chunk;
        PyObject_GetArenaAllocator(&alloc);
    }

    if (alloc.free && chunk) {
        // In case the arena mechanism has been torn down already.
        while (chunk) {
            _PyStackChunk *prev = chunk->previous;
            chunk->previous = nullptr;
            alloc.free(alloc.ctx, chunk, chunk->size);
            chunk = prev;
        }
    }

    this->datastack_chunk = nullptr;
    this->datastack_limit = nullptr;
    this->datastack_top = nullptr;
#endif
}


}; // namespace greenlet

#endif // GREENLET_PYTHON_STATE_CPP

Effects & Handlers
==================

Prohibited callback contexts
----------------------------

.. warning::

   Effects must not be performed, and :class:`Resume` and
   :class:`ResumeAsync` continuations must not be invoked, from any of the
   following callback contexts:

   * audit hooks;
   * tracing, profiling, or ``sys.monitoring`` callbacks;
   * signal handlers;
   * weak-reference callbacks;
   * garbage-collector callbacks;
   * ``__del__`` methods or other finalizers;
   * ``atexit`` callbacks; or
   * GUI callbacks, event-loop callbacks, or callbacks running on another
     thread.

def effect
----------

.. autofunction:: aleff._multishot.v1.effects.effect

class Effect
------------

.. autoclass:: aleff._multishot.v1.intf.Effect
   :members:

class Resume
------------

.. autoclass:: aleff._multishot.v1.intf.Resume
   :members:

class ResumeAsync
-----------------

.. autoclass:: aleff._multishot.v1.intf.ResumeAsync
   :members:

class Handler
-------------

.. autoclass:: aleff._multishot.v1.intf.Handler
   :members:
   :special-members: __call__

class AsyncHandler
------------------

.. autoclass:: aleff._multishot.v1.intf.AsyncHandler
   :members:
   :special-members: __call__

def create_handler
------------------

.. autofunction:: aleff._multishot.v1.handlers.create_handler

def create_async_handler
------------------------

.. autofunction:: aleff._multishot.v1.handlers.create_async_handler

class EffectNotHandledError
---------------------------

.. autoclass:: aleff._multishot.v1.intf.EffectNotHandledError
   :members:

def effects
-----------

.. autofunction:: aleff._multishot.v1.utils.effects

def unhandled_effects
---------------------

.. autofunction:: aleff._multishot.v1.utils.unhandled_effects

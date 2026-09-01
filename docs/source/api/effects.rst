Effects & Handlers
==================

Prohibited callback contexts
----------------------------

.. warning::

   Effects must not be performed, and :class:`Resume` and
   :class:`ResumeAsync` continuations must not be invoked, from any of the
   following callback contexts:

   * audit hooks;
   * the ``array.array`` constructor or methods of ``array.array`` instances;
   * tracing, profiling, or ``sys.monitoring`` callbacks;
   * signal handlers;
   * weak-reference callbacks;
   * garbage-collector callbacks;
   * ``__del__`` methods or other finalizers;
   * callbacks invoked by ``dict.setdefault``;
   * calls to ``str.format`` or ``str.format_map``;
   * calls to ``itertools.tee`` or iteration through an ``itertools._tee``;
   * codec error handlers registered with ``codecs.register_error``;
   * adapters and converters invoked by ``sqlite3.adapt``,
     ``sqlite3.Cursor.execute``, or ``sqlite3.Cursor.executemany``;
   * ``sqlite3.Connection.text_factory``, ``Connection.row_factory``, and
     ``Cursor.row_factory`` callbacks;
   * callbacks registered through ``sqlite3.Connection.create_function``,
     ``create_aggregate``, ``create_window_function``, ``create_collation``,
     ``set_authorizer``, ``set_progress_handler``, or ``set_trace_callback``;
   * parser-target callbacks invoked by ``xml.etree.ElementTree.XMLParser.feed``
     or ``XMLParser.close``, and Expat handlers invoked by
     ``xml.parsers.expat.xmlparser.Parse`` or ``ParseFile``;
   * SNI and message callbacks invoked during ``ssl.SSLObject`` or
     ``ssl.SSLSocket`` handshake, read, write, or shutdown operations;
   * callbacks installed through ``readline.set_completer``,
     ``set_completion_display_matches_hook``, ``set_pre_input_hook``, or
     ``set_startup_hook``;
   * callables invoked by ``contextvars.Context.run``;
   * ``__fspath__`` or ``__index__`` methods invoked by public C functions in
     ``os``, ``posix``, ``nt``, ``fcntl``, or ``termios``;
   * ``fileno`` methods invoked by ``select.select`` or by the ``register``,
     ``modify``, and ``unregister`` methods of ``poll``, ``epoll``, ``devpoll``,
     and ``kqueue`` objects;
   * ``__index__`` or buffer-protocol methods invoked by ``socket.socket`` or
     the ``send``, ``sendall``, ``sendto``, ``sendmsg``, ``recv_into``,
     ``recvfrom_into``, ``recvmsg_into``, and ``setsockopt`` methods of socket
     objects;
   * ``__index__`` or buffer-protocol methods invoked by the ``mmap.mmap``
     constructor or ``mmap.write``;
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

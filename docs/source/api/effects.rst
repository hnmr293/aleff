Effects & Handlers
==================

Prohibited callback contexts
----------------------------

.. warning::

   Effects must not be performed, and :class:`Resume` and
   :class:`ResumeAsync` continuations must not be invoked, from any of the
   following callback contexts:

   * Python data model:

     * ``__del__``

   * built-in types:

     * ``dict.setdefault``
     * ``str.format``
     * ``str.format_map``

   * ``sys``:

     * ``sys.addaudithook``
     * ``sys.settrace``
     * ``sys.setprofile``
     * ``sys.monitoring.register_callback``

   * ``threading``:

     * ``threading.settrace``
     * ``threading.settrace_all_threads``
     * ``threading.setprofile``
     * ``threading.setprofile_all_threads``

   * ``signal``:

     * ``signal.signal``

   * ``weakref``:

     * ``weakref.ref``
     * ``weakref.proxy``
     * ``weakref.WeakMethod``
     * ``weakref.finalize``

   * ``gc``:

     * ``gc.callbacks``

   * ``array``:

     * ``array.array``

   * ``itertools``:

     * ``itertools.tee``
     * ``itertools._tee.__next__``

   * ``codecs``:

     * ``codecs.register_error``

   * ``sqlite3``:

     * ``sqlite3.adapt``
     * ``sqlite3.Cursor.execute``
     * ``sqlite3.Cursor.executemany``
     * ``sqlite3.Cursor.fetchone``
     * ``sqlite3.Cursor.fetchmany``
     * ``sqlite3.Cursor.fetchall``
     * ``sqlite3.Cursor.__next__``
     * ``sqlite3.Connection.text_factory``
     * ``sqlite3.Connection.row_factory``
     * ``sqlite3.Cursor.row_factory``
     * ``sqlite3.Connection.create_function``
     * ``sqlite3.Connection.create_aggregate``
     * ``sqlite3.Connection.create_window_function``
     * ``sqlite3.Connection.create_collation``
     * ``sqlite3.Connection.set_authorizer``
     * ``sqlite3.Connection.set_progress_handler``
     * ``sqlite3.Connection.set_trace_callback``

   * ``xml.etree.ElementTree``:

     * ``xml.etree.ElementTree.XMLParser.feed``
     * ``xml.etree.ElementTree.XMLParser.close``

   * ``xml.parsers.expat``:

     * ``xml.parsers.expat.xmlparser.Parse``
     * ``xml.parsers.expat.xmlparser.ParseFile``

   * ``ssl``:

     * ``ssl.SSLContext.wrap_socket``
     * ``ssl.SSLSocket.accept``
     * ``ssl.SSLSocket.connect``
     * ``ssl.SSLSocket.connect_ex``
     * ``ssl.SSLSocket.do_handshake``
     * ``ssl.SSLSocket.read``
     * ``ssl.SSLSocket.recv``
     * ``ssl.SSLSocket.recv_into``
     * ``ssl.SSLSocket.send``
     * ``ssl.SSLSocket.sendall``
     * ``ssl.SSLSocket.unwrap``
     * ``ssl.SSLSocket.write``
     * ``ssl.SSLObject.do_handshake``
     * ``ssl.SSLObject.read``
     * ``ssl.SSLObject.unwrap``
     * ``ssl.SSLObject.write``

   * ``readline``:

     * ``readline.set_completer``
     * ``readline.set_completion_display_matches_hook``
     * ``readline.set_pre_input_hook``
     * ``readline.set_startup_hook``

   * ``contextvars``:

     * ``contextvars.Context.run``

   * ``os`` / ``posix`` / ``nt``:

     * ``os._exit``
     * ``os.access``
     * ``os.add_dll_directory``
     * ``os.chdir``
     * ``os.chflags``
     * ``os.chmod``
     * ``os.chown``
     * ``os.chroot``
     * ``os.close``
     * ``os.closerange``
     * ``os.confstr``
     * ``os.copy_file_range``
     * ``os.device_encoding``
     * ``os.dup``
     * ``os.dup2``
     * ``os.eventfd``
     * ``os.eventfd_read``
     * ``os.eventfd_write``
     * ``os.execv``
     * ``os.execve``
     * ``os.fchdir``
     * ``os.fchmod``
     * ``os.fchown``
     * ``os.fdatasync``
     * ``os.fpathconf``
     * ``os.fspath``
     * ``os.fstat``
     * ``os.fstatvfs``
     * ``os.fsync``
     * ``os.ftruncate``
     * ``os.get_blocking``
     * ``os.get_handle_inheritable``
     * ``os.get_inheritable``
     * ``os.get_terminal_size``
     * ``os.getgrouplist``
     * ``os.getpgid``
     * ``os.getpriority``
     * ``os.getrandom``
     * ``os.getsid``
     * ``os.getxattr``
     * ``os.grantpt``
     * ``os.initgroups``
     * ``os.isatty``
     * ``os.kill``
     * ``os.killpg``
     * ``os.lchflags``
     * ``os.lchmod``
     * ``os.lchown``
     * ``os.link``
     * ``os.listdir``
     * ``os.listmounts``
     * ``os.listxattr``
     * ``os.lockf``
     * ``os.login_tty``
     * ``os.lseek``
     * ``os.lstat``
     * ``os.major``
     * ``os.makedev``
     * ``os.memfd_create``
     * ``os.minor``
     * ``os.mkdir``
     * ``os.mkfifo``
     * ``os.mknod``
     * ``os.nice``
     * ``os.open``
     * ``os.pathconf``
     * ``os.pidfd_open``
     * ``os.pipe2``
     * ``os.plock``
     * ``os.posix_fadvise``
     * ``os.posix_fallocate``
     * ``os.posix_openpt``
     * ``os.posix_spawn``
     * ``os.posix_spawnp``
     * ``os.pread``
     * ``os.preadv``
     * ``os.ptsname``
     * ``os.pwrite``
     * ``os.pwritev``
     * ``os.read``
     * ``os.readinto``
     * ``os.readlink``
     * ``os.readv``
     * ``os.remove``
     * ``os.removexattr``
     * ``os.rename``
     * ``os.replace``
     * ``os.rmdir``
     * ``os.scandir``
     * ``os.sched_get_priority_max``
     * ``os.sched_get_priority_min``
     * ``os.sched_getaffinity``
     * ``os.sched_getparam``
     * ``os.sched_getscheduler``
     * ``os.sched_rr_get_interval``
     * ``os.sched_setaffinity``
     * ``os.sched_setparam``
     * ``os.sched_setscheduler``
     * ``os.sendfile``
     * ``os.set_blocking``
     * ``os.set_handle_inheritable``
     * ``os.set_inheritable``
     * ``os.setegid``
     * ``os.seteuid``
     * ``os.setgid``
     * ``os.setns``
     * ``os.setpgid``
     * ``os.setpriority``
     * ``os.setregid``
     * ``os.setresgid``
     * ``os.setresuid``
     * ``os.setreuid``
     * ``os.setuid``
     * ``os.setxattr``
     * ``os.spawnv``
     * ``os.spawnve``
     * ``os.splice``
     * ``os.startfile``
     * ``os.stat``
     * ``os.statvfs``
     * ``os.strerror``
     * ``os.symlink``
     * ``os.sysconf``
     * ``os.tcgetpgrp``
     * ``os.tcsetpgrp``
     * ``os.timerfd_create``
     * ``os.timerfd_gettime``
     * ``os.timerfd_gettime_ns``
     * ``os.timerfd_settime``
     * ``os.timerfd_settime_ns``
     * ``os.truncate``
     * ``os.ttyname``
     * ``os.umask``
     * ``os.unlink``
     * ``os.unlockpt``
     * ``os.unshare``
     * ``os.urandom``
     * ``os.utime``
     * ``os.wait3``
     * ``os.wait4``
     * ``os.waitid``
     * ``os.waitpid``
     * ``os.WCOREDUMP``
     * ``os.WEXITSTATUS``
     * ``os.WIFCONTINUED``
     * ``os.WIFEXITED``
     * ``os.WIFSIGNALED``
     * ``os.WIFSTOPPED``
     * ``os.WSTOPSIG``
     * ``os.WTERMSIG``
     * ``os.write``
     * ``os.writev``

   * ``fcntl``:

     * ``fcntl.fcntl``
     * ``fcntl.ioctl``
     * ``fcntl.flock``
     * ``fcntl.lockf``

   * ``termios``:

     * ``termios.tcgetattr``
     * ``termios.tcsetattr``
     * ``termios.tcsendbreak``
     * ``termios.tcdrain``
     * ``termios.tcflush``
     * ``termios.tcflow``
     * ``termios.tcgetwinsize``
     * ``termios.tcsetwinsize``

   * ``select``:

     * ``select.select``
     * ``select.poll.register``
     * ``select.poll.modify``
     * ``select.poll.unregister``
     * ``select.epoll.register``
     * ``select.epoll.modify``
     * ``select.epoll.unregister``
     * ``select.devpoll.register``
     * ``select.devpoll.modify``
     * ``select.devpoll.unregister``
     * ``select.kevent``

   * ``socket``:

     * ``socket.socket``
     * ``socket.socket.send``
     * ``socket.socket.sendall``
     * ``socket.socket.sendto``
     * ``socket.socket.sendmsg``
     * ``socket.socket.recv_into``
     * ``socket.socket.recvfrom_into``
     * ``socket.socket.recvmsg_into``
     * ``socket.socket.setsockopt``

   * ``mmap``:

     * ``mmap.mmap``
     * ``mmap.mmap.write``

   * ``atexit``:

     * ``atexit.register``

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

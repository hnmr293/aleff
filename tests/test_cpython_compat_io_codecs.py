"""CPython observable compatibility tests for I/O and codec boundaries."""

from __future__ import annotations

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def _check(source: str) -> None:
    assert_cpython_compatible(dedent(source), timeout=15)


def test_io_open_opener_and_iobase_methods_match_cpython() -> None:
    _check(
        """
        import io
        import os
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "sample")
            with open(path, "wb") as stream:
                stream.write(b"opener")
            calls = []

            def opener(opener_path, flags):
                calls.append((os.path.basename(opener_path), flags & os.O_ACCMODE))
                return os.open(opener_path, flags)

            with io.open(path, "rb", opener=opener) as stream:
                print("io.open", stream.read(), len(calls), calls[0][0])

        class Lines(io.IOBase):
            def __init__(self, values):
                self.values = iter(values)

            def readline(self, size=-1):
                return next(self.values, "")

        stream = Lines(["next\\n"])
        print("iter", iter(stream) is stream, next(stream))
        print("readlines", Lines(["one\\n", "two\\n", ""]).readlines())

        class Sink(io.IOBase):
            def __init__(self):
                self.values = []

            def write(self, value):
                self.values.append(value)
                return len(value)

        sink = Sink()
        print("writelines", sink.writelines(["a", "b"]), sink.values)

        class Broken(io.IOBase):
            def readline(self, size=-1):
                raise RuntimeError("readline failed")

            def write(self, value):
                raise RuntimeError("write failed")

        for operation in (lambda: next(Broken()), lambda: Broken().readlines(), lambda: Broken().writelines(["x"])):
            try:
                operation()
            except Exception as exc:
                print("error", type(exc).__name__, str(exc))
        """
    )


def test_buffered_reader_writer_random_and_pair_match_cpython() -> None:
    _check(
        """
        import io

        class Raw(io.RawIOBase):
            def __init__(self, data=b"read\\n"):
                self.data = bytearray(data)
                self.position = 0
                self.closed_by_raw = False

            def readable(self):
                return True

            def writable(self):
                return True

            def seekable(self):
                return True

            def readinto(self, buffer):
                chunk = self.data[self.position:self.position + len(buffer)]
                buffer[:len(chunk)] = chunk
                self.position += len(chunk)
                return len(chunk)

            def write(self, value):
                self.data[self.position:self.position + len(value)] = value
                self.position += len(value)
                return len(value)

            def seek(self, offset, whence=0):
                if whence == 0:
                    self.position = offset
                elif whence == 1:
                    self.position += offset
                else:
                    self.position = len(self.data) + offset
                return self.position

            def tell(self):
                return self.position

            def flush(self):
                return None

            def close(self):
                self.closed_by_raw = True
                super().close()

        raw = Raw()
        reader = io.BufferedReader(raw, 2)
        destination = bytearray(2)
        print("reader", reader.read(2), reader.read1(1), reader.seek(0), reader.tell(), reader.readinto(destination), bytes(destination))
        reader.close()
        print("reader close", raw.closed_by_raw)

        raw = Raw(b"")
        writer = io.BufferedWriter(raw, 2)
        print("writer", writer.write(b"ab"), writer.writelines([b"c"]), writer.flush(), writer.tell())
        writer.close()
        print("writer close", bytes(raw.data), raw.closed_by_raw)

        raw = Raw(b"random")
        random = io.BufferedRandom(raw, 2)
        print("random", random.read(2), random.seek(1), random.tell(), random.write(b"Z"), random.flush(), random.tell())
        random.close()
        print("random close", raw.closed_by_raw)

        left = Raw(b"pair")
        right = Raw(b"")
        pair = io.BufferedRWPair(left, right, 2)
        print("pair", pair.read(2), pair.write(b"xy"), pair.flush())
        pair.close()
        print("pair close", left.closed_by_raw, right.closed_by_raw)

        class Invalid(io.RawIOBase):
            def readable(self):
                return True

            def readinto(self, buffer):
                raise ValueError("read failed")

        try:
            io.BufferedReader(Invalid()).read(1)
        except Exception as exc:
            print("buffer error", type(exc).__name__, str(exc))
        """
    )


def test_text_iowrapper_methods_and_errors_match_cpython() -> None:
    _check(
        """
        import io

        reader = io.TextIOWrapper(io.BytesIO(b"alpha\\nbeta\\n"), encoding="utf-8")
        print("read", reader.read(2))
        print("readline", repr(reader.readline()))
        print("tell", reader.tell())
        print("seek", reader.seek(0), reader.readline())
        reader.flush()
        reader.close()
        print("closed", reader.closed)

        buffer = io.BytesIO()
        writer = io.TextIOWrapper(buffer, encoding="utf-8", write_through=True)
        print("write", writer.write("é"), writer.flush(), buffer.getvalue())
        writer.close()

        class Broken(io.BytesIO):
            fail_flush = True

            def read1(self, size=-1):
                raise RuntimeError("read1 failed")

            def flush(self):
                if self.fail_flush:
                    raise RuntimeError("flush failed")
                return super().flush()

        wrappers = (
            io.TextIOWrapper(Broken(b"x"), encoding="ascii"),
            io.TextIOWrapper(Broken(), encoding="ascii"),
        )
        for operation in (
            lambda: wrappers[0].read(1),
            lambda: wrappers[1].flush(),
        ):
            try:
                operation()
            except Exception as exc:
                print("text error", type(exc).__name__, str(exc))

        for wrapper in wrappers:
            wrapper.buffer.fail_flush = False
            wrapper.close()
        """
    )


def test_codecs_lookup_encode_decode_and_search_match_cpython() -> None:
    _check(
        """
        import codecs

        for name in ("UTF-8", "utf_8", "ascii"):
            info = codecs.lookup(name)
            print("lookup", name, info.name, info.__class__.__name__)
        print("encode", codecs.encode("café", "utf-8"))
        print("decode", codecs.decode(b"caf\\xc3\\xa9", "utf-8"))

        search_calls = []
        custom_name = "issue56_custom_codec"

        def encode(value, errors="strict"):
            return value.upper().encode("ascii"), len(value)

        def decode(value, errors="strict"):
            return value.decode("ascii").lower(), len(value)

        info = codecs.CodecInfo(encode=encode, decode=decode, name="issue56-custom")

        def search(encoding):
            search_calls.append(encoding)
            return info if encoding == custom_name else None

        codecs.register(search)
        print("custom", codecs.lookup(custom_name).name, codecs.encode("ab", custom_name), codecs.decode(b"CD", custom_name))
        print("search calls", search_calls)

        normalized = []
        def record_normalized(encoding):
            normalized.append(encoding)
            return None
        codecs.register(record_normalized)
        for spelling in ("--Issue 56..Codec!!Name--", "  MIXED---case  "):
            try:
                codecs.lookup(spelling)
            except LookupError:
                pass
        print("normalized", normalized)
        codecs.unregister(record_normalized)

        for operation in (
            lambda: codecs.lookup("issue56_missing_codec"),
            lambda: codecs.encode("x", "issue56_missing_codec"),
            lambda: codecs.decode(b"x", "issue56_missing_codec"),
        ):
            try:
                operation()
            except Exception as exc:
                print("codec error", type(exc).__name__, str(exc))
        """
    )


def test_codec_registry_operations_are_thread_safe() -> None:
    _check(
        """
        import codecs
        import threading

        thread_count = 4
        registered = threading.Barrier(thread_count)
        looked_up = threading.Barrier(thread_count)
        outcomes = []

        def worker(index):
            name = f"issue56_thread_codec_{index}"

            def encode(value, errors="strict"):
                return value.encode("ascii"), len(value)

            def decode(value, errors="strict"):
                return value.decode("ascii"), len(value)

            info = codecs.CodecInfo(encode=encode, decode=decode, name=name)

            def search(encoding):
                return info if encoding == name else None

            try:
                codecs.register(search)
                registered.wait()
                outcomes.append((index, codecs.lookup(name).name))
                looked_up.wait()
                codecs.unregister(search)
            except BaseException as exc:
                outcomes.append((index, type(exc).__name__, str(exc)))

        threads = [threading.Thread(target=worker, args=(index,)) for index in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        print(sorted(outcomes))
        """
    )


def test_registered_codec_error_handlers_match_cpython() -> None:
    _check(
        """
        import codecs

        def replacement(exc):
            return ("?", exc.end)

        def bytes_replacement(exc):
            return (b"?", exc.end)

        codecs.register_error("issue56_text_errors", replacement)
        codecs.register_error("issue56_bytes_errors", bytes_replacement)
        print("encode errors", "éX".encode("ascii", "issue56_text_errors"))
        print("decode errors", b"\\xffX".decode("ascii", "issue56_text_errors"))
        print("bytes errors", "éX".encode("ascii", "issue56_bytes_errors"))

        for operation in (
            lambda: "é".encode("ascii", "strict"),
            lambda: b"\\xff".decode("ascii", "strict"),
            lambda: codecs.lookup_error("issue56_missing_error"),
        ):
            try:
                operation()
            except Exception as exc:
                print("handler error", type(exc).__name__, str(exc))
        """
    )

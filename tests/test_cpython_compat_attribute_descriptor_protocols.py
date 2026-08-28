"""Differential regression tests for attribute and descriptor protocols."""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def test_builtin_attribute_functions_keep_cpython_metadata_and_behavior() -> None:
    assert_cpython_compatible(
        dedent(
            """
            import builtins
            import inspect


            def show(label, operation):
                try:
                    print(label, "ok", operation())
                except Exception as exc:
                    print(label, "error", type(exc).__name__, exc.args)


            def metadata(function):
                module = function.__module__
                module = module if isinstance(module, str) else type(module).__name__
                try:
                    signature = str(inspect.signature(function))
                except Exception as exc:
                    signature = ("error", type(exc).__name__)
                try:
                    text_signature = function.__text_signature__
                except Exception as exc:
                    text_signature = ("error", type(exc).__name__)
                return function.__name__, module, signature, text_signature


            for name in ("getattr", "hasattr", "setattr", "delattr"):
                function = builtins.__dict__[name]
                show(
                    "metadata:" + name,
                    lambda function=function: metadata(function),
                )


            class Target:
                value = 3

                def __getattribute__(self, name):
                    if name == "broken":
                        raise RuntimeError("boom")
                    return object.__getattribute__(self, name)


            target = Target()
            show("getattr:value", lambda: builtins.getattr(target, "value"))
            show("getattr:missing", lambda: builtins.getattr(target, "missing"))
            show(
                "getattr:default",
                lambda: builtins.getattr(target, "missing", "fallback"),
            )
            show(
                "getattr:non_attribute_error",
                lambda: builtins.getattr(target, "broken", "fallback"),
            )
            show("hasattr:value", lambda: builtins.hasattr(target, "value"))
            show("hasattr:missing", lambda: builtins.hasattr(target, "missing"))
            show(
                "hasattr:non_attribute_error",
                lambda: builtins.hasattr(target, "broken"),
            )
            show(
                "setattr:return_and_value",
                lambda: (
                    builtins.setattr(target, "added", 4),
                    target.added,
                ),
            )
            show(
                "delattr:return_and_absence",
                lambda: (
                    builtins.delattr(target, "added"),
                    "added" in target.__dict__,
                ),
            )
            show("getattr:non_string_name", lambda: builtins.getattr(target, 1))
            show("hasattr:non_string_name", lambda: builtins.hasattr(target, 1))
            show("setattr:non_string_name", lambda: builtins.setattr(target, 1, 2))
            show("delattr:non_string_name", lambda: builtins.delattr(target, 1))
            """
        )
    )


def test_descriptor_precedence_binding_and_exception_protocols() -> None:
    assert_cpython_compatible(
        dedent(
            """
            events = []


            def show(label, operation):
                try:
                    print(label, "ok", operation())
                except Exception as exc:
                    print(label, "error", type(exc).__name__, exc.args)


            class DataDescriptor:
                def __get__(self, instance, owner):
                    events.append(("data.get", instance is None, owner.__name__))
                    if instance is not None and instance.fail:
                        raise AttributeError("data failure")
                    return "class-data" if instance is None else "instance-data"

                def __set__(self, instance, value):
                    events.append(("data.set", value))
                    instance.__dict__["stored"] = value

                def __delete__(self, instance):
                    events.append(("data.delete",))
                    instance.__dict__.pop("stored", None)


            class NonDataDescriptor:
                def __get__(self, instance, owner):
                    events.append(("nondata.get", instance is None, owner.__name__))
                    return "class-nondata" if instance is None else "instance-nondata"


            class Target:
                data = DataDescriptor()
                nondata = NonDataDescriptor()

                def __init__(self):
                    self.nondata = "instance-value"
                    self.fail = False

                def __getattr__(self, name):
                    events.append(("getattr", name))
                    if name == "fallback":
                        return "fallback-value"
                    raise AttributeError(name)


            target = Target()
            show("data:instance", lambda: target.data)
            show("data:class", lambda: Target.data)
            show("nondata:instance_precedence", lambda: target.nondata)
            show("data:set", lambda: (setattr(target, "data", 7), target.stored))
            show("data:delete", lambda: (delattr(target, "data"), "stored" in target.__dict__))
            show("getattr:fallback", lambda: getattr(target, "fallback"))
            show("getattr:default_after_attribute_error", lambda: getattr(target, "missing", "default"))

            target.fail = True
            show("data:attribute_error", lambda: target.data)
            show("data:attribute_error_default", lambda: getattr(target, "data", "default"))
            show("hasattr:data_attribute_error", lambda: hasattr(target, "data"))


            class Explosive:
                def __get__(self, instance, owner):
                    raise KeyError("descriptor-key")


            class ErrorTarget:
                value = Explosive()


            show("descriptor:non_attribute_error", lambda: ErrorTarget().value)


            class PropertyTarget:
                @property
                def value(self):
                    return "property-value"

                @property
                def broken(self):
                    raise ValueError("property-value-error")


            property_target = PropertyTarget()
            show("property:get", lambda: property_target.value)
            show("property:set_readonly", lambda: setattr(property_target, "value", 1))
            show("property:get_error", lambda: property_target.broken)
            print("events", events)
            """
        )
    )


def test_attribute_hooks_slots_and_descriptor_set_name_protocols() -> None:
    assert_cpython_compatible(
        dedent(
            """
            events = []


            def show(label, operation):
                try:
                    print(label, "ok", operation())
                except Exception as exc:
                    print(label, "error", type(exc).__name__, exc.args)


            class Hooked:
                def __getattribute__(self, name):
                    events.append(("getattribute", name))
                    if name == "virtual":
                        return "virtual-value"
                    return object.__getattribute__(self, name)

                def __getattr__(self, name):
                    events.append(("getattr", name))
                    if name == "fallback":
                        return "fallback-value"
                    raise AttributeError(name)

                def __setattr__(self, name, value):
                    events.append(("setattr", name, value))
                    object.__setattr__(self, "saved", (name, value))

                def __delattr__(self, name):
                    events.append(("delattr", name))
                    object.__setattr__(self, "deleted", name)


            hooked = Hooked()
            show("hook:getattribute", lambda: hooked.virtual)
            show("hook:getattr", lambda: hooked.fallback)
            show("hook:setattr", lambda: (setattr(hooked, "value", 8), hooked.saved))
            show("hook:delattr", lambda: (delattr(hooked, "value"), hooked.deleted))
            show("hook:missing", lambda: hooked.missing)


            class Slotted:
                __slots__ = ("value",)


            slotted = Slotted()
            show("slots:set_get", lambda: (setattr(slotted, "value", 5), slotted.value))
            show("slots:missing", lambda: getattr(slotted, "missing"))
            show("slots:delete", lambda: (delattr(slotted, "value"), hasattr(slotted, "value")))
            show("slots:delete_missing", lambda: delattr(slotted, "value"))


            class Named:
                def __init__(self, label):
                    self.label = label

                def __set_name__(self, owner, name):
                    events.append(("set_name", self.label, owner.__name__, name))


            class Base:
                def __init_subclass__(cls, **kwargs):
                    events.append(("init_subclass", cls.__name__, kwargs))
                    super().__init_subclass__(**kwargs)


            class Child(Base):
                first = Named("first")
                second = Named("second")


            dynamic = type("Dynamic", (Base,), {"value": Named("dynamic")})
            show("set_name:class_and_type", lambda: (Child.__name__, dynamic.__name__))


            class BadNamed:
                def __set_name__(self, owner, name):
                    raise RuntimeError("set-name-error")


            show("set_name:error", lambda: type("Broken", (), {"value": BadNamed()}))
            print("events", events)
            """
        )
    )


def test_operator_accessor_metadata_types_behavior_and_errors() -> None:
    assert_cpython_compatible(
        dedent(
            """
            import inspect
            import operator


            def show(label, operation):
                try:
                    print(label, "ok", operation())
                except Exception as exc:
                    print(label, "error", type(exc).__name__, exc.args)


            def metadata(function):
                module = function.__module__
                module = module if isinstance(module, str) else type(module).__name__
                try:
                    signature = str(inspect.signature(function))
                except Exception as exc:
                    return function.__name__, module, type(exc).__name__
                return function.__name__, module, signature


            class Nested:
                value = 9


            class Target:
                nested = Nested()
                label = "tail"

                def method(self, value, *, flag):
                    return (value, flag)


            target = Target()
            factories = (
                ("attrgetter", ("nested.value", "label")),
                ("itemgetter", ("first", "second")),
                ("methodcaller", ("method", 5)),
            )
            for name, args in factories:
                factory = operator.__dict__[name]
                show(
                    "factory:" + name,
                    lambda factory=factory: metadata(factory),
                )
                show(
                    "callable:" + name,
                    lambda factory=factory, args=args: (
                        type(factory(*args)).__module__,
                        type(factory(*args)).__qualname__,
                        type(factory(*args).__reduce__()[0]).__name__,
                        factory(*args).__reduce__()[1],
                    ),
                )

            show(
                "attrgetter:normal",
                lambda: operator.attrgetter("nested.value", "label")(target),
            )
            show(
                "itemgetter:normal",
                lambda: operator.itemgetter("first", "second")(
                    {"first": 1, "second": "two"}
                ),
            )
            show(
                "methodcaller:normal",
                lambda: operator.methodcaller("method", 5, flag=True)(target),
            )
            show(
                "attrgetter:missing",
                lambda: operator.attrgetter("nested.missing")(target),
            )
            show(
                "itemgetter:missing",
                lambda: operator.itemgetter("missing")({}),
            )
            show(
                "methodcaller:missing",
                lambda: operator.methodcaller("missing")(target),
            )
            show("attrgetter:no_arguments", lambda: operator.attrgetter())
            show("itemgetter:no_arguments", lambda: operator.itemgetter())
            show("methodcaller:no_arguments", lambda: operator.methodcaller())
            """
        )
    )

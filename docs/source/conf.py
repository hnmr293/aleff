import os
import sys

sys.path.insert(0, os.path.abspath("../../src"))

project = "aleff"
copyright = "2026, hnmr"
author = "hnmr"
release = "0.3.1"

extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.intersphinx",
    "myst_parser",
]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

templates_path = ["_templates"]
exclude_patterns = []
language = "en"

# -- HTML output
html_theme = "sphinx_rtd_theme"
html_static_path = ["_static"]
html_css_files = ["custom.css"]

# -- autodoc
add_module_names = False
autodoc_member_order = "bysource"
autodoc_typehints = "signature"
autodoc_default_options = {
    "members": True,
    "undoc-members": False,
    "show-inheritance": True,
}
maximum_signature_line_length = 80
python_maximum_signature_line_length = 80

# -- napoleon (Google-style docstrings)
napoleon_google_docstring = True

# -- intersphinx
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
}


# -- Convert markdown code blocks in docstrings to reST
import re

_CODE_BLOCK_RE = re.compile(
    r"^(?P<indent>\s*)```(?P<lang>\w*)\s*\n(?P<code>.*?)^(?P=indent)```\s*$",
    re.MULTILINE | re.DOTALL,
)


def _md_codeblock_to_rst(app, what, name, obj, options, lines):
    """Convert markdown fenced code blocks to reST in docstrings."""
    text = "\n".join(lines)
    if "```" not in text:
        return

    def replace(m):
        indent = m.group("indent")
        lang = m.group("lang")
        code = m.group("code")
        directive = f"{indent}.. code-block:: {lang}\n\n" if lang else f"{indent}::\n\n"
        indented = "\n".join(f"{indent}    {line}" if line.strip() else "" for line in code.splitlines())
        return f"{directive}{indented}\n"

    new_text = _CODE_BLOCK_RE.sub(replace, text)
    lines[:] = new_text.splitlines()


# -- Callable → arrow notation (applied as HTML post-processing)


def _callable_to_arrow(s):
    """Convert Callable[[A, B], R] to (A, B) → R in a type string."""
    result = []
    i = 0
    while i < len(s):
        callable_match = None
        for prefix in ("~typing.Callable[", "~collections.abc.Callable[", "Callable["):
            if s[i:].startswith(prefix):
                callable_match = prefix
                break
        if callable_match:
            i += len(callable_match)
            if i < len(s) and s[i] == "[":
                # Callable[[params], R]
                i += 1
                depth = 1
                params_start = i
                while i < len(s) and depth > 0:
                    if s[i] == "[":
                        depth += 1
                    elif s[i] == "]":
                        depth -= 1
                    i += 1
                params = s[params_start : i - 1]
                if i < len(s) and s[i] == ",":
                    i += 1
                while i < len(s) and s[i] == " ":
                    i += 1
                depth = 1
                ret_start = i
                while i < len(s) and depth > 0:
                    if s[i] == "[":
                        depth += 1
                    elif s[i] == "]":
                        depth -= 1
                    i += 1
                ret = s[ret_start : i - 1]
                result.append(f"({_callable_to_arrow(params)}) \u2192 {_callable_to_arrow(ret)}")
            elif s[i:].startswith("..."):
                # Callable[..., R]
                i += 3
                if i < len(s) and s[i] == ",":
                    i += 1
                while i < len(s) and s[i] == " ":
                    i += 1
                depth = 1
                ret_start = i
                while i < len(s) and depth > 0:
                    if s[i] == "[":
                        depth += 1
                    elif s[i] == "]":
                        depth -= 1
                    i += 1
                ret = s[ret_start : i - 1]
                result.append(f"(...) \u2192 {_callable_to_arrow(ret)}")
            else:
                # Callable[P, R] (ParamSpec)
                depth = 1
                first_start = i
                comma_pos = None
                j = i
                while j < len(s) and depth > 0:
                    if s[j] == "[":
                        depth += 1
                    elif s[j] == "]":
                        depth -= 1
                        if depth == 0:
                            break
                    elif s[j] == "," and depth == 1:
                        comma_pos = j
                    j += 1
                if comma_pos is not None:
                    param = s[first_start:comma_pos].strip()
                    ret = s[comma_pos + 1 : j].strip()
                    i = j + 1
                    result.append(f"{_callable_to_arrow(param)} \u2192 {_callable_to_arrow(ret)}")
                else:
                    result.append(callable_match)
        else:
            result.append(s[i])
            i += 1
    return "".join(result)


def _caller_to_arrow(s):
    """Convert Caller[V] to () → V in a type string."""
    result = []
    i = 0
    while i < len(s):
        if s[i:].startswith("Caller["):
            i += len("Caller[")
            depth = 1
            start = i
            while i < len(s) and depth > 0:
                if s[i] == "[":
                    depth += 1
                elif s[i] == "]":
                    depth -= 1
                i += 1
            inner = s[start : i - 1]
            result.append(f"() \u2192 {_caller_to_arrow(inner)}")
        elif s[i:].startswith("Caller"):
            # Bare Caller without []
            i += len("Caller")
            result.append("() \u2192 V")
        else:
            result.append(s[i])
            i += 1
    return "".join(result)


def _rewrite_callable_in_html(app, exception):
    """Post-process built HTML to replace Callable/Caller with arrow notation.

    Sphinx renders type annotations as sequences of <span> tags, so
    Callable[[], T] becomes multiple tags like:
        <span>Callable</span><span>[</span><span>[</span>...
    We strip tags within annotation containers, apply the arrow conversion
    on the plain text, then wrap the result back in a single <span>.
    """
    from pathlib import Path

    if exception:
        return

    def _transform_annotation_span(m):
        """Transform a <span class="n">...</span> block containing Callable/Caller."""
        full = m.group(0)
        if "Callable" not in full and "Caller" not in full:
            return full
        plain = re.sub(r"<[^>]+>", "", full)
        converted = _transform_all(plain)
        if converted == plain:
            return full
        return f'<span class="n"><span class="pre">{converted}</span></span>'

    def _transform_all(plain):
        """Apply both Callable and Caller arrow conversions."""
        result = _callable_to_arrow(plain)
        result = _caller_to_arrow(result)
        return result

    outdir = Path(app.outdir)
    for html_file in outdir.rglob("*.html"):
        text = html_file.read_text(encoding="utf-8")
        if "Callable" not in text and "Caller" not in text:
            continue

        # Match return type blocks: <span class="sig-return-typehint">...Callable...</span></span>
        def _transform_return_type(m):
            full = m.group(0)
            if "Callable" not in full and "Caller" not in full:
                return full
            plain = re.sub(r"<[^>]+>", "", full)
            converted = _transform_all(plain)
            if converted == plain:
                return full
            return f'<span class="sig-return-typehint"><span class="pre">{converted}</span></span>'

        new_text = re.sub(
            r'<span class="sig-return-typehint">(?:(?!</dt>).)*?</span></span>(?=</span>)',
            _transform_return_type,
            text,
            flags=re.DOTALL,
        )

        # Match parameter annotations inside <em class="sig-param">:
        # The <span class="n"> containing Callable/Caller may not be the first
        # child (e.g. *args has <span class="o">*</span> before it).
        def _transform_param(m):
            full = m.group(0)
            prefix = m.group(1)
            annotation = m.group(2)
            if "Callable" not in annotation and "Caller" not in annotation:
                return full
            plain = re.sub(r"<[^>]+>", "", annotation)
            converted = _transform_all(plain)
            if converted == plain:
                return full
            return f'{prefix}<span class="n"><span class="pre">{converted}</span></span>'

        new_text = re.sub(
            r'(<em class="sig-param">(?:(?!</em>).)*?)(<span class="n">(?:(?!</em>).)*?(?:Callable|Caller).*?</span>)(?=</em>)',
            _transform_param,
            new_text,
            flags=re.DOTALL,
        )
        if new_text != text:
            html_file.write_text(new_text, encoding="utf-8")


def _style_heading_keywords(app, exception):
    """Wrap 'def'/'class' in headings with a span for styling."""
    from pathlib import Path

    if exception:
        return

    outdir = Path(app.outdir)
    for html_file in outdir.rglob("*.html"):
        text = html_file.read_text(encoding="utf-8")
        new_text = re.sub(
            r"(<h[1-6]>)(def |class )",
            r'\1<span class="heading-keyword">\2</span>',
            text,
        )
        if new_text != text:
            html_file.write_text(new_text, encoding="utf-8")


# -- PEP 695 type parameters for class signatures


def _add_type_params_to_class_signature(app, what, name, obj, options, signature, return_annotation):
    """Replace Protocol (*args, **kwargs) with PEP 695 type parameters."""
    if what != "class":
        return None
    type_params = getattr(obj, "__type_params__", None)
    if not type_params:
        return None

    import typing

    parts = []
    for tp in type_params:
        if isinstance(tp, typing.ParamSpec):
            parts.append(f"**{tp.__name__}")
        elif isinstance(tp, typing.TypeVarTuple):
            parts.append(f"*{tp.__name__}")
        else:
            parts.append(tp.__name__)

    type_param_str = "[" + ", ".join(parts) + "]"
    return (type_param_str, None)


def setup(app):
    app.connect("autodoc-process-docstring", _md_codeblock_to_rst)
    app.connect("autodoc-process-signature", _add_type_params_to_class_signature)
    app.connect("build-finished", _rewrite_callable_in_html)
    app.connect("build-finished", _style_heading_keywords)

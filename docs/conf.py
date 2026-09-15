"""Sphinx configuration for the nind Python package documentation."""
import os
import re
import sys

DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(DOCS_DIR)

# Import the pure-Python package directly from source, without building the
# nind._native extension. Several src/py/nind/*.py modules do import it, but
# only to delegate at call time (constructors, not module import) - each
# guards that import with try/except ImportError (falling back to
# native = None) precisely so autodoc can still introspect everything here.
sys.path.insert(0, os.path.join(REPO_ROOT, "src", "py"))

with open(os.path.join(REPO_ROOT, "pyproject.toml"), encoding="utf-8") as f:
    _pyproject_text = f.read()
_version_match = re.search(r'^version = "([^"]+)"$', _pyproject_text, re.MULTILINE)

project = "nind"
copyright = "2014-2026 LATEJCON, CEA LIST/DIASI/LVIC"
author = "Jean-Yves Sage, CEA LIST/DIASI/LVIC, and contributors"
release = _version_match.group(1) if _version_match else "0.0.0"
version = release

extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinx.ext.intersphinx",
    "myst_parser",
]

myst_enable_extensions = ["fieldlist", "deflist"]
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

autodoc_member_order = "bysource"
autodoc_default_options = {
    "members": True,
    "undoc-members": False,
    "show-inheritance": True,
}
autoclass_content = "both"
add_module_names = False

intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
}

templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

html_theme = "pydata_sphinx_theme"
html_title = f"nind {version}"
html_static_path = []
html_theme_options = {
    "github_url": "https://github.com/aymara/nind",
    "use_edit_page_button": True,
    "show_toc_level": 2,
    "navigation_with_keys": True,
    "footer_start": ["copyright"],
    "footer_end": [],
}
html_context = {
    "github_user": "aymara",
    "github_repo": "nind",
    "github_version": "master",
    "doc_path": "docs",
}

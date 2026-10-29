from n64_docs import setup_project

setup_project(globals(),
  project="Pyrite64",
  author="Max Bebök",
  copyright="2026, Max Bebök",
  cpp_api={
    "name": "pyrite64",
    "input": "../n64/engine/include",
    "out_dir": "docs/manual/api",
    "intro": "Reference for the public engine API in ``engine/include/``.",
  },
)

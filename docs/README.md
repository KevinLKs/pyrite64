# Pyrite64 Docs

This project uses [Sphinx](https://www.sphinx-doc.org) to generate the documentation.<br>
The shared setup (theme, styling, C++ API generation, build tools) lives in [n64-docs](https://github.com/HailToDodongo/n64-docs),
this directory only contains the content and config.

## Building Docs

### Setup

Make sure you have python3 installed.<br>
After that, navigate to the directory this readme is in, and create a venv:

```sh
python3 -m venv .venv
source .venv/bin/activate
```

Followed by installing all the dependencies:

```sh
pip install -r requirements.txt
```

### Building

To generate the HTML docs, run:

```sh
n64-docs build
```
The final static website is then generated under `_build/html`.

To build, serve and automatically rebuild on changes, run:
```sh
n64-docs serve
```
This lets you open the page on http://localhost:8000/index.html<br>
By default the C++ API is skipped to keep builds fast, use `n64-docs serve --full` to include it.

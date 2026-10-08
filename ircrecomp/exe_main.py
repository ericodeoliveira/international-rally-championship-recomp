"""Entry point of the packaged IRC-Recompilador.exe.

Without arguments it opens the window. The window runs the recompilation by starting the
same .exe again with command-line arguments (`build CUE --out DIR`), exactly like
`python -m ircrecomp` from a source checkout, and reads its output through a pipe.
"""
import io
import os
import sys

CLI_COMMANDS = {"build", "check", "lift", "analyze"}


def _console_streams():
    """A windowed executable may start without stdout/stderr; use the pipe handles the
    window passed in, or discard the output when there are none."""
    for name, fd in (("stdout", 1), ("stderr", 2)):
        if getattr(sys, name) is None:
            try:
                stream = io.TextIOWrapper(os.fdopen(fd, "wb", buffering=0), encoding="utf-8",
                                          errors="replace", line_buffering=True, write_through=True)
            except OSError:
                stream = open(os.devnull, "w", encoding="utf-8")
            setattr(sys, name, stream)


def main():
    if len(sys.argv) > 1 and sys.argv[1] in CLI_COMMANDS:
        _console_streams()
        from ircrecomp.__main__ import main as cli
        cli()
    else:
        from ircrecomp.gui import main as gui
        gui()


if __name__ == "__main__":
    main()

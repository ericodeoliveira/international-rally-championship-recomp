"""Janela do ircrecomp: escolher a imagem do CD, recompilar, configurar e jogar.

Abre com `IRC Recompilador.bat` (ou `pythonw -m ircrecomp.gui`). Executa o mesmo pipeline
da linha de comando (`python -m ircrecomp build`) e mostra o progresso.

O visual imita os menus do jogo (fundo magenta, painel azul-marinho, letras itálicas
amarelas). O logotipo e as fotos de rali vêm da cópia do jogo do próprio jogador
(veja uiassets.py); sem elas a janela usa só as cores. Textos em português, inglês e
espanhol (i18n.py), escolhidos pelas bandeiras no canto do painel.
"""
import json
import os
import platform
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import i18n, installcheck

ROOT = Path(__file__).resolve().parent.parent
STATE_FILE = ROOT / "ircrecomp_gui.json"
N_STEPS = 5
NO_WINDOW = 0x08000000 if platform.system() == "Windows" else 0

# cores dos menus do jogo
NAVY = "#0b0b4e"        # painel central
DEEP = "#05052e"        # campos e log
EDGE = "#2c2ca8"        # bordas do painel
BTN = "#1a1a72"
YELLOW = "#ffe81a"      # itens de menu
RED = "#ff2a2a"         # item selecionado
MAGENTA = "#d8148a"
PINK = "#ff3c9c"
WHITE = "#ffffff"
LAVENDER = "#b4b4f0"
GREY = "#5c5c94"
GREEN = "#3ee070"
SHADOW = "#1a0428"
HEADER_H = 184
MARGIN = 16


def console_python():
    """python.exe next to the running interpreter (pythonw has no stdout)."""
    exe = Path(sys.executable)
    cand = exe.with_name("python.exe" if platform.system() == "Windows" else "python3")
    return str(cand if cand.exists() else exe)


# ------------------------------------------------------------- ini helpers
INI_KEYS = ["fullscreen", "aspect", "render_scale", "smooth", "fps_limit", "direct3d", "texture_filter", "scale"]


def read_ini(path):
    vals = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(.*?)\s*$", line)
            if m and not line.lstrip().startswith(";"):
                vals[m.group(1).lower()] = m.group(2)
    return vals


def write_ini(path, updates):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines() if path.exists() else []
    done = set()
    for i, line in enumerate(lines):
        m = re.match(r"\s*([A-Za-z_]\w*)\s*=", line)
        if m and not line.lstrip().startswith(";") and m.group(1).lower() in updates:
            k = m.group(1).lower()
            lines[i] = f"{k}={updates[k]}"
            done.add(k)
    for k, v in updates.items():
        if k not in done:
            lines.append(f"{k}={v}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- style
def mix(a, b, t):
    a = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(x + (y - x) * t) for x, y in zip(a, b))


def dark_title_bar(win):
    """Barra de título azul-marinho no Windows 11 (ignorado em outros sistemas)."""
    if platform.system() != "Windows":
        return
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
        dwm = ctypes.windll.dwmapi
        on = ctypes.c_int(1)
        dwm.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), ctypes.sizeof(on))          # modo escuro
        color = ctypes.c_int(0x4E0B0B)                                                     # NAVY em COLORREF
        dwm.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(color), ctypes.sizeof(color))    # cor da barra
    except (OSError, AttributeError):
        pass


class Fonts:
    def __init__(self, root):
        fams = set(tkfont.families(root))
        heavy = "Arial Black" if "Arial Black" in fams else "Arial"
        ui = "Segoe UI" if "Segoe UI" in fams else "Arial"
        self.head = (heavy, 13, "italic")
        self.big = (heavy, 19, "italic")
        self.title = (heavy, 15, "italic")
        self.button = ("Arial", 11, "bold italic")
        self.text = (ui, 10)
        self.small = (ui, 9)
        self.mono = ("Consolas", 9) if "Consolas" in fams else ("Courier", 9)


class GameButton(tk.Label):
    """Botão no estilo dos itens de menu: amarelo itálico, destacado em magenta."""
    KINDS = {
        "normal": dict(bg=BTN, fg=YELLOW, hover_bg=MAGENTA, hover_fg=WHITE, border=EDGE, off_bg=BTN),
        "play": dict(bg=MAGENTA, fg=WHITE, hover_bg=PINK, hover_fg=WHITE, border="#ff7cc0", off_bg="#3a1048"),
        "danger": dict(bg="#4a0c26", fg="#ff9a9a", hover_bg=RED, hover_fg=WHITE, border="#a0204a", off_bg=BTN),
    }

    def __init__(self, parent, text, command, kind="normal", font=None, padx=14, pady=6):
        self.k = self.KINDS[kind]
        super().__init__(parent, text=text, font=font, bg=self.k["bg"], fg=self.k["fg"], padx=padx, pady=pady,
                         cursor="hand2", highlightthickness=2, highlightbackground=self.k["border"])
        self.command = command
        self.enabled = True
        self.bind("<Enter>", lambda _e: self.paint(True))
        self.bind("<Leave>", lambda _e: self.paint(False))
        self.bind("<ButtonRelease-1>", self.click)

    def paint(self, hover):
        if not self.enabled:
            self.configure(bg=self.k["off_bg"], fg=GREY, highlightbackground="#24245c", cursor="")
        elif hover:
            self.configure(bg=self.k["hover_bg"], fg=self.k["hover_fg"], highlightbackground=WHITE, cursor="hand2")
        else:
            self.configure(bg=self.k["bg"], fg=self.k["fg"], highlightbackground=self.k["border"], cursor="hand2")

    def click(self, e):
        inside = 0 <= e.x < self.winfo_width() and 0 <= e.y < self.winfo_height()
        if self.enabled and inside:
            self.command()

    def set_enabled(self, on):
        self.enabled = on
        self.paint(False)


def style_ttk(root):
    st = ttk.Style(root)
    st.theme_use("clam")
    st.configure("Game.Horizontal.TProgressbar", troughcolor=DEEP, background=YELLOW, bordercolor=EDGE,
                 lightcolor="#fff27a", darkcolor="#d8b800", thickness=16)
    st.configure("Game.TCombobox", fieldbackground=DEEP, background=BTN, foreground=WHITE, arrowcolor=YELLOW,
                 bordercolor=EDGE, lightcolor=EDGE, darkcolor=EDGE, selectbackground=MAGENTA, selectforeground=WHITE)
    st.map("Game.TCombobox", fieldbackground=[("readonly", DEEP)], foreground=[("readonly", WHITE)],
           selectbackground=[("readonly", DEEP)], background=[("active", MAGENTA)])
    st.configure("Game.Vertical.TScrollbar", background=BTN, troughcolor=DEEP, bordercolor=DEEP,
                 arrowcolor=YELLOW, lightcolor=BTN, darkcolor=BTN, gripcount=0)
    st.map("Game.Vertical.TScrollbar", background=[("active", MAGENTA)])
    root.option_add("*TCombobox*Listbox.background", DEEP)
    root.option_add("*TCombobox*Listbox.foreground", WHITE)
    root.option_add("*TCombobox*Listbox.selectBackground", MAGENTA)
    root.option_add("*TCombobox*Listbox.selectForeground", WHITE)


def draw_flag(cv, x, y, w, h, lang):
    """Bandeira pequena desenhada com primitivas (Brasil, Reino Unido, Espanha)."""
    if lang == "pt":
        cv.create_rectangle(x, y, x + w, y + h, fill="#009b3a", outline="")
        cv.create_polygon(x + w * 0.5, y + h * 0.12, x + w * 0.92, y + h * 0.5, x + w * 0.5, y + h * 0.88,
                          x + w * 0.08, y + h * 0.5, fill="#fedf00", outline="")
        r = h * 0.25
        cv.create_oval(x + w * 0.5 - r, y + h * 0.5 - r, x + w * 0.5 + r, y + h * 0.5 + r, fill="#002776", outline="")
    elif lang == "en":
        cv.create_rectangle(x, y, x + w, y + h, fill="#012169", outline="")
        for a, b, c, d in ((x, y, x + w, y + h), (x, y + h, x + w, y)):
            cv.create_line(a, b, c, d, fill=WHITE, width=4)
            cv.create_line(a, b, c, d, fill="#c8102e", width=1)
        cv.create_rectangle(x + w * 0.5 - 3, y, x + w * 0.5 + 3, y + h, fill=WHITE, outline="")
        cv.create_rectangle(x, y + h * 0.5 - 3, x + w, y + h * 0.5 + 3, fill=WHITE, outline="")
        cv.create_rectangle(x + w * 0.5 - 1.5, y, x + w * 0.5 + 1.5, y + h, fill="#c8102e", outline="")
        cv.create_rectangle(x, y + h * 0.5 - 1.5, x + w, y + h * 0.5 + 1.5, fill="#c8102e", outline="")
    elif lang == "es":
        cv.create_rectangle(x, y, x + w, y + h, fill="#aa151b", outline="")
        cv.create_rectangle(x, y + h * 0.25, x + w, y + h * 0.75, fill="#f1bf00", outline="")


class FlagBar(tk.Canvas):
    """As bandeiras dos idiomas; a do idioma atual fica com moldura amarela."""
    FW, FH, GAP = 26, 17, 8

    def __init__(self, parent, current, on_pick):
        n = len(i18n.LANGS)
        super().__init__(parent, width=n * (self.FW + self.GAP), height=self.FH + 6, bg=NAVY,
                         highlightthickness=0, cursor="hand2")
        self.current, self.on_pick, self.hover = current, on_pick, None
        self.bind("<Button-1>", self.click)
        self.bind("<Motion>", lambda e: self.set_hover(self.lang_at(e.x)))
        self.bind("<Leave>", lambda _e: self.set_hover(None))
        self.draw()

    def lang_at(self, x):
        i = int(x // (self.FW + self.GAP))
        return i18n.LANGS[i] if 0 <= i < len(i18n.LANGS) else None

    def set_hover(self, lang):
        if lang != self.hover:
            self.hover = lang
            self.draw()

    def draw(self):
        self.delete("all")
        for i, lang in enumerate(i18n.LANGS):
            x, y = self.GAP / 2 + i * (self.FW + self.GAP), 3
            sel = lang == self.current
            frame = YELLOW if sel else WHITE if lang == self.hover else "#34347c"
            self.create_rectangle(x - 2, y - 2, x + self.FW + 1, y + self.FH + 1, outline=frame, width=2)
            draw_flag(self, x, y, self.FW, self.FH, lang)
            if not sel and lang != self.hover:   # apagadas, para não chamar atenção
                self.create_rectangle(x, y, x + self.FW, y + self.FH, fill=NAVY, stipple="gray50", outline="")

    def click(self, e):
        lang = self.lang_at(e.x)
        if lang and lang != self.current:
            self.current = lang
            self.draw()
            self.on_pick(lang)


# ------------------------------------------------------------------ app
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.state_data = self.load_state()
        self.lang = self.state_data.get("lang", "pt")
        if self.lang not in i18n.LANGS:
            self.lang = "pt"
        self.texts = []                 # (widget, função que devolve o texto no idioma atual)
        self.status_msg = ("st_choose", {}, WHITE)
        self.title(self.T("title"))
        self.minsize(780, 720)
        self.geometry("920x820")
        self.configure(bg=NAVY)
        self.proc = None
        self.task = None                # "check" / "delete" running in the background
        self.lines = queue.Queue()
        self.f = Fonts(self)
        style_ttk(self)
        self.photos, self.photo_i, self.logo = [], 0, None
        self.assets_busy = False

        # fundo magenta com o cabeçalho (logo + foto de rali) desenhado num Canvas
        self.cv = tk.Canvas(self, bg=MAGENTA, highlightthickness=0)
        self.cv.pack(fill="both", expand=True)
        self.cv.bind("<Configure>", self.layout)

        # painel azul-marinho com o conteúdo
        self.panel = tk.Frame(self.cv, bg=NAVY, highlightthickness=3, highlightbackground=EDGE)
        self.panel_id = self.cv.create_window(MARGIN, HEADER_H, anchor="nw", window=self.panel)
        body = tk.Frame(self.panel, bg=NAVY)
        body.pack(fill="both", expand=True, padx=16, pady=10)

        # --- 1. origem e destino
        row = self.heading(body, "h_cd", 1)
        self.flags = FlagBar(row, self.lang, self.set_language)
        self.flags.pack(side="right")
        paths = tk.Frame(body, bg=NAVY)
        paths.pack(fill="x", pady=(2, 4))
        self.cue = tk.StringVar(value=self.state_data.get("cue", str(self.guess_cue() or "")))
        self.out = tk.StringVar(value=self.state_data.get("out", str(ROOT / "dist" / "IRC")))
        self.path_row(paths, "lbl_cue", self.cue, self.pick_cue, 0)
        self.path_row(paths, "lbl_out", self.out, self.pick_out, 1)
        paths.columnconfigure(1, weight=1)
        self.req = tk.Frame(body, bg=NAVY)
        self.req.pack(fill="x", pady=(0, 6))
        self.req_labels, self.req_checks = [], []

        # --- 2. recompilar
        self.heading(body, "h_build", 2)
        top = tk.Frame(body, bg=NAVY)
        top.pack(fill="x", pady=(2, 6))
        self.btn_build = GameButton(top, self.T("build"), self.start_build, font=self.f.button)
        self.btn_build.pack(side="left")
        self.btn_check = self.tr(GameButton(top, "", self.start_check, font=self.f.button), lambda: self.T("check"))
        self.btn_check.pack(side="left", padx=(8, 0))
        self.btn_delete = self.tr(GameButton(top, "", self.start_delete, kind="danger", font=self.f.button),
                                  lambda: self.T("delete"))
        self.btn_delete.pack(side="left", padx=(8, 0))
        self.status = tk.Label(body, text="", bg=NAVY, fg=WHITE, font=self.f.text, anchor="w", justify="left")
        self.status.pack(fill="x", pady=(0, 4))
        self.progress = ttk.Progressbar(body, maximum=N_STEPS * 100, style="Game.Horizontal.TProgressbar")
        self.progress.pack(fill="x")
        steps = tk.Frame(body, bg=NAVY)
        steps.pack(fill="x", pady=(3, 4))
        self.step_labels = []
        for i in range(N_STEPS):
            lbl = self.tr(tk.Label(steps, bg=NAVY, fg=GREY, font=self.f.small, anchor="w"),
                          lambda i=i: f"{i + 1}. {self.T('steps_short')[i]}")
            lbl.grid(row=0, column=i, sticky="w")
            steps.columnconfigure(i, weight=1, uniform="step")
            self.step_labels.append(lbl)
        logf = tk.Frame(body, bg=DEEP, highlightthickness=2, highlightbackground=EDGE)
        logf.pack(fill="both", expand=True, pady=(2, 8))
        bar = ttk.Scrollbar(logf, orient="vertical", style="Game.Vertical.TScrollbar")
        bar.pack(side="right", fill="y")
        self.log = tk.Text(logf, height=4, font=self.f.mono, state="disabled", wrap="none", bg=DEEP,
                           fg=LAVENDER, insertbackground=WHITE, relief="flat", borderwidth=0,
                           selectbackground=MAGENTA, padx=6, pady=4, yscrollcommand=bar.set)
        bar.configure(command=self.log.yview)
        self.log.pack(fill="both", expand=True)
        self.log.tag_configure("step", foreground=YELLOW, font=(self.f.mono[0], 9, "bold"))
        self.log.tag_configure("ok", foreground=GREEN, font=(self.f.mono[0], 10, "bold"))
        self.log.tag_configure("err", foreground=RED)
        self.log.tag_configure("warn", foreground="#ffb347")

        # --- 3. jogar
        tk.Frame(body, bg=MAGENTA, height=2).pack(fill="x", pady=(2, 10))
        play = tk.Frame(body, bg=NAVY)
        play.pack(fill="x")
        self.btn_play = GameButton(play, "", self.play, kind="play", font=self.f.big, padx=26, pady=4)
        self.btn_play.pack(side="left")
        side = tk.Frame(play, bg=NAVY)
        side.pack(side="left", padx=14)
        self.btn_cfg = GameButton(side, "", self.settings, font=self.f.button, padx=10, pady=3)
        self.btn_link = GameButton(side, "", self.shortcut, font=self.f.button, padx=10, pady=3)
        self.btn_open = GameButton(side, "", self.open_folder, font=self.f.button, padx=10, pady=3)
        for b, key in ((self.btn_play, "play"), (self.btn_cfg, "settings"), (self.btn_link, "shortcut"),
                       (self.btn_open, "open")):
            self.tr(b, lambda key=key: self.T(key))
        for b in (self.btn_cfg, self.btn_link, self.btn_open):
            b.pack(side="left", padx=(0, 8))
        self.tr(tk.Label(body, bg=NAVY, fg=GREY, font=self.f.small, anchor="w"), lambda: self.T("controls")
                ).pack(fill="x", pady=(8, 0))

        self.out.trace_add("write", lambda *_: self.on_out_change())
        self.cue.trace_add("write", lambda *_: self.load_assets())
        self.check_requirements()
        self.refresh()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        dark_title_bar(self)
        self.load_assets()
        self.after(100, self.pump)
        self.after(6000, self.next_photo)

    # ------------------------------------------------------------ idioma
    def T(self, key):
        return i18n.text(key, self.lang)

    def tr(self, widget, fn):
        """Registra um widget cujo texto acompanha o idioma."""
        widget.configure(text=fn())
        self.texts.append((widget, fn))
        return widget

    def set_language(self, lang):
        self.lang = lang
        self.save_state()
        self.title(self.T("title"))
        for widget, fn in self.texts:
            widget.configure(text=fn())
        self.render_requirements()
        self.refresh()
        self.set_status(*self.status_msg[:1], color=self.status_msg[2], **self.status_msg[1])
        self.draw_header()

    def set_status(self, key, color=WHITE, **kw):
        self.status_msg = (key, kw, color)
        self.status.configure(text=self.T(key).format(**kw), fg=color)

    # ------------------------------------------------------------ widgets
    def heading(self, parent, key, n):
        row = tk.Frame(parent, bg=NAVY)
        row.pack(fill="x", pady=(6, 2))
        tk.Label(row, text=f"{n}", bg=NAVY, fg=YELLOW, font=self.f.head).pack(side="left")
        self.tr(tk.Label(row, bg=NAVY, fg=WHITE, font=self.f.head), lambda: f"–  {self.T(key)}  –"
                ).pack(side="left", padx=(10, 0))
        return row

    def path_row(self, parent, key, var, cmd, row):
        self.tr(tk.Label(parent, bg=NAVY, fg=LAVENDER, font=self.f.text), lambda: self.T(key)
                ).grid(row=row, column=0, sticky="w", pady=3)
        tk.Entry(parent, textvariable=var, bg=DEEP, fg=WHITE, insertbackground=WHITE, relief="flat",
                 font=self.f.text, highlightthickness=2, highlightbackground=EDGE, highlightcolor=YELLOW,
                 selectbackground=MAGENTA).grid(row=row, column=1, sticky="ew", padx=10, pady=3, ipady=3)
        self.tr(GameButton(parent, "", cmd, font=self.f.button, padx=10, pady=2), lambda: self.T("choose")
                ).grid(row=row, column=2, pady=3)

    def append_log(self, text, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n", tag or ())
        self.log.see("end")
        self.log.configure(state="disabled")

    # ------------------------------------------------- background / header
    def layout(self, _e=None):
        w, h = self.cv.winfo_width(), self.cv.winfo_height()
        self.cv.delete("bg")
        # degradê diagonal rosa -> roxo, como o fundo dos menus
        stops = [(0.0, "#f0189a"), (0.45, "#b0148e"), (1.0, "#3c0c6c")]
        step = 10
        for c in range(0, w + h + step, step):
            t = min(c / (w + h), 1.0)
            for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
                if t <= t1:
                    color = mix(c0, c1, (t - t0) / (t1 - t0))
                    break
            self.cv.create_polygon(c, 0, c + step + 1, 0, c + step + 1 - h, h, c - h, h,
                                   fill=color, outline="", tags="bg")
        # grade fina e faixas inclinadas (as "lâminas" do menu)
        for x in range(0, w, 48):
            self.cv.create_line(x, 0, x, h, fill="#b8309c", tags="bg")
        for y in range(0, h, 48):
            self.cv.create_line(0, y, w, y, fill="#b8309c", tags="bg")
        for x0, wd in ((-150, 30), (w - 120, 34), (w - 60, 18)):
            self.cv.create_polygon(x0 + 140, 0, x0 + 140 + wd, 0, x0 + wd, h, x0, h,
                                   fill="#2a0638", stipple="gray50", outline="", tags="bg")
        self.cv.tag_lower("bg")
        self.cv.coords(self.panel_id, MARGIN, HEADER_H)
        self.cv.itemconfigure(self.panel_id, width=w - 2 * MARGIN, height=h - HEADER_H - MARGIN)
        self.draw_header()

    def shadow_text(self, x, y, text, font, color, anchor="center", depth=2):
        self.cv.create_text(x + depth, y + depth, text=text, font=font, fill=SHADOW, anchor=anchor, tags="hdr")
        self.cv.create_text(x, y, text=text, font=font, fill=color, anchor=anchor, tags="hdr")

    def draw_header(self):
        w = self.cv.winfo_width()
        self.cv.delete("hdr")
        right = w - MARGIN
        if self.photos:
            img = self.photos[self.photo_i % len(self.photos)]
            pw, ph = img.width(), img.height()
            x, y = w - MARGIN - pw, 14
            self.cv.create_rectangle(x + 5, y + 5, x + pw + 7, y + ph + 7, fill=SHADOW, outline="", tags="hdr")
            self.cv.create_rectangle(x - 3, y - 3, x + pw + 2, y + ph + 2, fill=WHITE, outline="", tags="hdr")
            self.cv.create_image(x, y, image=img, anchor="nw", tags=("hdr", "photo"))
            right = x - 3
        if self.logo:
            from .uiassets import LOGO_PAD
            self.cv.create_image(MARGIN - LOGO_PAD, 12 - LOGO_PAD, image=self.logo, anchor="nw", tags="hdr")
            left = MARGIN + self.logo.width() - 2 * LOGO_PAD
        else:   # sem a arte do jogo: o logotipo em texto
            self.shadow_text(MARGIN + 8, 36, "International", self.f.title, RED, "w")
            self.shadow_text(MARGIN, 90, "RALLY", (self.f.big[0], 44, "italic"), WHITE, "w", 3)
            self.shadow_text(MARGIN + 60, 146, "Championship", (self.f.big[0], 17, "italic"), RED, "w")
            left = MARGIN + 300
        if right - left > 170:
            cx = (left + right) / 2
            self.shadow_text(cx, 76, self.T("sub1"), self.f.title, WHITE)
            self.shadow_text(cx, 108, self.T("sub2"), self.f.button, YELLOW)
        self.cv.tag_bind("photo", "<Button-1>", lambda _e: self.next_photo(manual=True))

    def next_photo(self, manual=False):
        if self.photos:
            self.photo_i += 1
            self.draw_header()
        if not manual:
            self.after(6000, self.next_photo)

    def load_assets(self):
        """Gera (em segundo plano) o logo e as fotos a partir da cópia do jogo do jogador."""
        if self.logo or self.assets_busy:
            return
        self.assets_busy = True
        game = Path(self.out.get()) / "game"
        cue = self.cue.get()

        def work():
            try:
                from .uiassets import ensure_assets
                folder = ensure_assets(game if game.is_dir() else None, cue)
            except Exception:  # sem arte, a janela continua só com as cores
                folder = None
            self.lines.put(("__assets__", folder))
        threading.Thread(target=work, daemon=True).start()

    def assets_ready(self, folder):
        self.assets_busy = False
        if not folder:
            return
        try:
            self.logo = tk.PhotoImage(file=str(Path(folder) / "logo.png"))
            self.photos = [tk.PhotoImage(file=str(p)).subsample(2) for p in sorted(Path(folder).glob("photo*.png"))]
            self.photo_i = (os.getpid() // 4) % max(len(self.photos), 1)   # uma foto diferente a cada abertura
            icon = Path(folder) / "icon.png"
            if icon.exists():
                self._icon = tk.PhotoImage(file=str(icon))
                self.iconphoto(True, self._icon)
        except tk.TclError:
            self.logo, self.photos = None, []
        self.draw_header()

    # ------------------------------------------------------------- state
    def load_state(self):
        try:
            return json.loads(STATE_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def save_state(self):
        try:
            STATE_FILE.write_text(json.dumps({"cue": self.cue.get(), "out": self.out.get(), "lang": self.lang}, indent=1), encoding="utf-8")
        except OSError:
            pass

    def guess_cue(self):
        found = sorted(ROOT.glob("*.cue"))
        return found[0] if found else None

    def game_exe(self):
        out = Path(self.out.get())
        exe = out / ("IRC.exe" if platform.system() == "Windows" else "IRC")
        return exe if exe.exists() else None

    def refresh(self):
        has_game = self.game_exe() is not None
        building = self.proc is not None or self.task is not None
        for b in (self.btn_play, self.btn_cfg, self.btn_link, self.btn_open, self.btn_check):
            b.set_enabled(has_game and not building)
        self.btn_delete.set_enabled(installcheck.is_build(self.out.get()) and not building)
        self.btn_build.set_enabled(not building)
        self.btn_build.configure(text=self.T("rebuild" if has_game else "build"))
        if not building and self.status_msg[0] in ("st_ready", "st_choose"):
            if has_game:
                self.set_status("st_ready", GREEN, out=self.out.get())
            else:
                self.set_status("st_choose", WHITE)

    def on_out_change(self):
        if self.proc is None and self.task is None:
            self.status_msg = ("st_choose", {}, WHITE)
        self.refresh()

    def mark_step(self, current, done=False):
        for i, lbl in enumerate(self.step_labels):
            lbl.configure(fg=GREEN if done or i < current else YELLOW if i == current else GREY)

    # ------------------------------------------------------- requirements
    def check_requirements(self):
        checks = []      # (ok, chave do texto, valores, link)
        try:
            import pefile  # noqa: F401
            import capstone  # noqa: F401
            checks.append((True, "req_py_ok", {}, None))
        except ImportError:
            checks.append((False, "req_py_missing", {}, None))
        need_gb, disk_key = 2, "req_disk"
        if platform.system() == "Windows":
            # sem o Visual Studio, o compilador portátil (llvm-mingw) é baixado na primeira recompilação
            from .__main__ import find_vcvars, windows_compiler
            from .toolchain import llvm_mingw_ready
            if windows_compiler(os.environ.get("IRC_COMPILER", "auto")) == "msvc":
                checks.append((find_vcvars() is not None, "req_vc_ok", {}, None))
            elif llvm_mingw_ready():
                checks.append((True, "req_cc_ok", {}, None))
            else:
                checks.append((True, "req_cc_auto", {}, None))
                need_gb, disk_key = 2.5, "req_disk_dl"
        else:
            ok = shutil.which("cmake") and (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang"))
            checks.append((bool(ok), "req_tools", {}, None))
        free = shutil.disk_usage(ROOT).free / 2**30
        checks.append((free > need_gb, disk_key, {"gb": free}, None))
        self.req_checks = checks
        self.req_ok = all(c[0] for c in checks)
        self.render_requirements()

    def render_requirements(self):
        for w in self.req_labels:
            w.destroy()
        self.req_labels = []
        for ok, key, kw, url in self.req_checks:
            text = self.T(key).format(**kw)
            lbl = tk.Label(self.req, text=("✔  " if ok else "✖  ") + text, fg=GREEN if ok else RED, bg=NAVY,
                           font=self.f.small + (("underline",) if url else ()), cursor="hand2" if url else "")
            if url:
                lbl.bind("<Button-1>", lambda _e, u=url: webbrowser.open(u))
            lbl.pack(anchor="w")
            self.req_labels.append(lbl)

    # -------------------------------------------------------------- pickers
    def pick_cue(self):
        p = filedialog.askopenfilename(title=self.T("dlg_cue"),
                                       filetypes=[(self.T("ft_cue"), "*.cue"), (self.T("ft_bin"), "*.bin"), (self.T("ft_all"), "*.*")],
                                       initialdir=str(Path(self.cue.get()).parent if self.cue.get() else ROOT))
        if not p:
            return
        path = Path(p)
        if path.suffix.lower() != ".cue":
            cue = path.with_suffix(".cue")
            if not cue.exists():
                cands = list(path.parent.glob(path.stem + "*.cue")) or list(path.parent.glob("*.cue"))
                cue = cands[0] if cands else None
            if not cue:
                messagebox.showerror(self.T("m_cue"), self.T("m_need_cue"))
                return
            path = cue
        self.cue.set(str(path))

    def pick_out(self):
        p = filedialog.askdirectory(title=self.T("dlg_out"), initialdir=self.out.get() or str(ROOT))
        if p:
            self.out.set(str(Path(p)))

    # ---------------------------------------------------------------- build
    def start_build(self):
        cue = Path(self.cue.get())
        if not cue.is_file():
            messagebox.showerror(self.T("m_cue"), self.T("m_pick_cue"))
            return
        self.check_requirements()
        if not self.req_ok and not messagebox.askyesno(self.T("m_req"), self.T("m_req_ask")):
            return
        self.save_state()
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.progress["value"] = 0
        self.mark_step(0)
        self.set_status("st_start", YELLOW)
        cmd = [console_python(), "-u", "-m", "ircrecomp", "build", str(cue), "--out", self.out.get()]
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        self.proc = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, creationflags=NO_WINDOW)
        self.refresh()
        threading.Thread(target=self.reader, args=(self.proc,), daemon=True).start()

    def reader(self, proc):
        for raw in proc.stdout:
            try:
                line = raw.decode("utf-8")
            except UnicodeDecodeError:
                line = raw.decode("cp850", errors="replace")
            self.lines.put(line.rstrip())
        proc.wait()
        self.lines.put(("__done__", proc.returncode))

    def pump(self):
        try:
            while True:
                item = self.lines.get_nowait()
                if isinstance(item, tuple):
                    if item[0] == "__assets__":
                        self.assets_ready(item[1])
                    elif item[0] == "__check__":
                        self.check_result(*item[1:])
                    elif item[0] == "__check_done__":
                        self.check_finished()
                    elif item[0] == "__deleted__":
                        self.delete_finished(item[1])
                    else:
                        self.finished(item[1])
                    continue
                if not item or "vswhere" in item or item.startswith("ou externo"):
                    continue
                tag = None
                m = re.match(r"\[(\d)/(\d)\]", item)
                if m:
                    step = int(m.group(1)) - 1
                    self.progress["value"] = step * 100
                    self.mark_step(step)
                    self.set_status("st_step", YELLOW, n=step + 1, total=N_STEPS, name=self.T("steps")[step])
                    tag = "step"
                elif re.match(r"\[\d+/\d+\] ", item):          # ninja compile progress
                    n, tot = map(int, re.match(r"\[(\d+)/(\d+)\]", item).groups())
                    self.progress["value"] = 200 + 100 * n / max(tot, 1)
                    continue
                elif self.progress["value"] % 100 < 90:
                    self.progress["value"] += 2
                if re.search(r"\berror\b|\berro\b|failed|falhou", item, re.I):
                    tag = "err"
                self.append_log(item, tag)
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def finished(self, code):
        self.proc = None
        self.check_requirements()          # o compilador pode ter sido baixado agora
        if code == 0:
            self.progress["value"] = N_STEPS * 100
            self.mark_step(N_STEPS, done=True)
            self.append_log("\n" + self.T("log_done"), "ok")
            self.set_status("st_done", GREEN, out=self.out.get())
            self.refresh()
            self.load_assets()
            if messagebox.askyesno(self.T("m_done"), self.T("m_done_ask")):
                self.play()
        else:
            self.set_status("st_fail", RED)
            self.refresh()
            messagebox.showerror(self.T("m_fail"), self.T("m_fail_msg"))

    # ------------------------------------------------------ validate / delete
    def clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def start_check(self):
        if not self.game_exe() or self.proc or self.task:
            return
        self.task = "check"
        self.check_results = []
        self.clear_log()
        self.progress["value"] = 0
        self.mark_step(-1)
        self.set_status("st_checking", YELLOW)
        self.append_log(self.T("chk_start"), "step")
        self.refresh()
        out = self.out.get()

        def work():
            for res in installcheck.check(out):
                self.lines.put(("__check__",) + res)
            self.lines.put(("__check_done__",))
        threading.Thread(target=work, daemon=True).start()

    def check_result(self, status, key, kw):
        text = self.T(key).format(**kw)
        if status == "info":
            self.append_log(text, "step")
            return
        self.check_results.append(status)
        mark, tag = {True: ("✔", "ok"), False: ("✖", "err"), None: ("⚠", "warn")}[status]
        self.append_log(f"{mark}  {text}", tag)
        self.progress["value"] = min(N_STEPS * 100, self.progress["value"] + N_STEPS * 25)

    def check_finished(self):
        self.task = None
        self.progress["value"] = N_STEPS * 100
        failed = False in self.check_results
        warned = None in self.check_results
        self.set_status("st_check_fail" if failed else "st_check_ok", RED if failed else GREEN)
        self.refresh()
        if failed:
            messagebox.showerror(self.T("m_check"), self.T("m_check_fail"))
        elif warned:
            messagebox.showwarning(self.T("m_check"), self.T("m_check_warn"))
        else:
            messagebox.showinfo(self.T("m_check"), self.T("m_check_ok"))

    def start_delete(self):
        out = self.out.get()
        if not installcheck.is_build(out) or self.proc or self.task:
            return
        if not messagebox.askyesno(self.T("m_del"), self.T("m_del_ask").format(out=out), icon="warning", default="no"):
            return
        self.task = "delete"
        self.set_status("st_deleting", YELLOW)
        self.refresh()
        threading.Thread(target=lambda: self.lines.put(("__deleted__", installcheck.remove(out))), daemon=True).start()

    def delete_finished(self, err):
        self.task = None
        if err:
            self.status_msg = ("st_choose", {}, WHITE)
            self.refresh()
            messagebox.showerror(self.T("m_del"), self.T(err))
            return
        self.clear_log()
        self.progress["value"] = 0
        self.mark_step(-1)
        self.set_status("st_deleted", WHITE)
        self.refresh()

    # ----------------------------------------------------------------- play
    def play(self):
        exe = self.game_exe()
        if exe:
            subprocess.Popen([str(exe)], cwd=str(exe.parent), creationflags=0)

    def open_folder(self):
        out = self.out.get()
        if platform.system() == "Windows":
            os.startfile(out)
        else:
            subprocess.Popen(["xdg-open", out])

    def shortcut(self):
        exe = self.game_exe()
        if not exe or platform.system() != "Windows":
            return
        ps = ("$d=[Environment]::GetFolderPath('Desktop');"
              "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $d 'International Rally Championship.lnk'));"
              "$s.TargetPath=$env:IRC_EXE;$s.WorkingDirectory=$env:IRC_DIR;$s.IconLocation=$env:IRC_EXE;$s.Save()")
        env = dict(os.environ, IRC_EXE=str(exe), IRC_DIR=str(exe.parent))
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], env=env, creationflags=NO_WINDOW,
                           capture_output=True, text=True)
        if r.returncode == 0:
            messagebox.showinfo(self.T("m_sc"), self.T("m_sc_ok"))
        else:
            messagebox.showerror(self.T("m_sc"), self.T("m_sc_err") + r.stderr[-500:])

    # ------------------------------------------------------------- settings
    def settings(self):
        ini = Path(self.out.get()) / "irc_native.ini"
        vals = read_ini(ini)
        win = tk.Toplevel(self, bg=NAVY)
        win.title(self.T("s_title"))
        win.transient(self)
        win.resizable(False, False)
        frm = tk.Frame(win, bg=NAVY, highlightthickness=3, highlightbackground=EDGE)
        frm.pack(fill="both")
        inner = tk.Frame(frm, bg=NAVY)
        inner.pack(fill="both", padx=18, pady=14)
        tk.Label(inner, text=self.T("s_head"), bg=NAVY, fg=WHITE, font=self.f.head
                 ).grid(row=0, column=0, columnspan=2, pady=(0, 10))

        def checkbox(label, key, default, row):
            v = tk.BooleanVar(value=vals.get(key, default) not in ("0", "", "false"))
            tk.Checkbutton(inner, text=label, variable=v, bg=NAVY, fg=YELLOW, selectcolor=DEEP, activebackground=NAVY,
                           activeforeground=WHITE, font=self.f.text, anchor="w", highlightthickness=0
                           ).grid(row=row, column=0, columnspan=2, sticky="w", pady=3)
            return v

        def combo(label, key, options, default, row):
            tk.Label(inner, text=label, bg=NAVY, fg=LAVENDER, font=self.f.text).grid(row=row, column=0, sticky="w", pady=4)
            cur = vals.get(key, default)
            names = [n for n, _ in options]
            current = next((n for n, val in options if val == cur), names[0])
            v = tk.StringVar(value=current)
            ttk.Combobox(inner, textvariable=v, values=names, state="readonly", width=34, style="Game.TCombobox",
                         font=self.f.text).grid(row=row, column=1, sticky="w", padx=8)
            return v, dict(options)

        T = self.T
        fullscreen = checkbox(T("s_full"), "fullscreen", "1", 1)
        aspect, aspect_map = combo(T("s_aspect"), "aspect", [(T("s_aspect_43"), "4:3"), (T("s_aspect_st"), "stretch")], "4:3", 2)
        rscale, rscale_map = combo(T("s_res"), "render_scale",
                                   [(T("s_res_auto"), "0"), (T("s_res_orig"), "1"), ("1280×960", "2"),
                                    ("1920×1440", "3"), (T("s_res_4"), "4")], "0", 3)
        fps, fps_map = combo(T("s_fps"), "fps_limit",
                             [(T("s_fps_mon"), "-1"), ("30 fps", "30"), ("60 fps", "60"), ("120 fps", "120"), (T("s_fps_off"), "0")], "-1", 4)
        d3d = checkbox(T("s_d3d"), "direct3d", "1", 5)
        texf = checkbox(T("s_texf"), "texture_filter", "1", 6)
        smooth = checkbox(T("s_smooth"), "smooth", "0", 7)
        tk.Label(inner, text=T("s_hint"),
                 bg=NAVY, fg=GREY, font=self.f.small, justify="left").grid(row=8, column=0, columnspan=2, sticky="w", pady=(10, 4))

        def save():
            write_ini(ini, {
                "fullscreen": "1" if fullscreen.get() else "0",
                "aspect": aspect_map[aspect.get()],
                "render_scale": rscale_map[rscale.get()],
                "fps_limit": fps_map[fps.get()],
                "direct3d": "1" if d3d.get() else "0",
                "texture_filter": "1" if texf.get() else "0",
                "smooth": "1" if smooth.get() else "0",
            })
            win.destroy()

        btns = tk.Frame(inner, bg=NAVY)
        btns.grid(row=9, column=0, columnspan=2, sticky="e", pady=(10, 0))
        GameButton(btns, self.T("s_cancel"), win.destroy, font=self.f.button, padx=12, pady=3).pack(side="right", padx=(8, 0))
        GameButton(btns, self.T("s_save"), save, kind="play", font=self.f.button, padx=16, pady=3).pack(side="right")
        dark_title_bar(win)
        win.grab_set()

    def on_close(self):
        if self.proc and not messagebox.askyesno(self.T("m_quit"), self.T("m_quit_ask")):
            return
        if self.proc:
            self.proc.kill()
        self.save_state()
        self.destroy()


def main():
    App().mainloop()


if __name__ == "__main__":
    main()

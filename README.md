# International Rally Championship — native recompilation

**[Português](#português) · [English](#english)**

> **Testado com / Tested with:** a versão do jogo disponível em / the version of the game available at
> <https://www.myabandonware.com/game/international-rally-championship-a66>
> (imagem de CD `.bin`/`.cue`, `RAL.EXE` 4.81 de 09/12/1998 / CD image, `RAL.EXE` 4.81 from 1998-12-09).
>
> O jogo **não** está incluído neste repositório; os direitos dele continuam com os seus detentores.
> The game is **not** included in this repository; its rights remain with their owners.

---

## Português

Recompilador estático (`ircrecomp`) que transforma o jogo **International Rally Championship**
(Magnetic Fields / Europress, 1997) num executável nativo de 64 bits para o Windows moderno — sem
emulador, sem DirectX antigo e sem precisar do CD.

O `RAL.EXE` original foi escrito em **assembly x86 à mão**. O `ircrecomp` traduz cada função para
C portável e o liga a um runtime novo que reimplementa as APIs do Windows 98 / DirectX 5 sobre o
[SDL3](https://libsdl.org). O código do jogo **não é distribuído**: o C é gerado na máquina de quem
tem o CD.

### Requisitos

- Windows 10/11 64 bits (testado no Windows 11), Python 3.10+ e a imagem do CD (`.cue` + `.bin`).
- Nada de Visual Studio: na primeira recompilação o compilador C
  ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw), clang + lld, ~180 MB) e o SDL3 são baixados
  automaticamente das páginas oficiais, com versão fixa e SHA-256 conferido, e ficam em
  `third_party\`. Se o Visual Studio Build Tools (C++) estiver instalado e o clang ainda não, ele é
  usado. Para escolher: `--compiler clang|msvc` ou a variável `IRC_COMPILER`.
- ~2,5 GB livres na primeira recompilação (compilador incluído); o jogo instalado ocupa ~370 MB.

### Como usar

#### Com janela (recomendado)

Dê dois cliques em **`IRC Recompilador.bat`** (na primeira vez ele prepara o ambiente Python sozinho):

1. **Escolher…** a imagem do CD (`.cue`; se escolher o `.bin`, o `.cue` ao lado é usado) e a pasta
   de instalação (pode ter acentos).
2. Confira os requisitos (verde = ok).
3. **Recompilar o jogo** — barra de progresso pelas 5 etapas e detalhes ao vivo.
4. **▶ Jogar**, **Configurações…** (tela cheia, proporção, resolução do 3D, limite de quadros,
   filtros), **Criar atalho** e **Abrir pasta**.

Ativos só quando já existe uma compilação na pasta escolhida:

- **Validar** — confere os arquivos essenciais e as 12 músicas, compara cada arquivo com o registro
  de integridade gravado pela recompilação (`irc_build.json`: tamanho e SHA-1; saves e configurações
  ficam de fora) e abre o jogo em janela por alguns segundos para ver se ele inicia e fecha normalmente.
- **Apagar** — depois de confirmar, remove tudo o que a recompilação criou (executável, dados,
  músicas, saves e o atalho que aponta para ele). A imagem do CD não é tocada.

A janela segue o visual dos menus do jogo. O logotipo e as fotos de rali do topo são lidos da imagem
do CD do próprio jogador e guardados em `build\ui_assets` — nenhuma arte do jogo vem com a
ferramenta. Ela está em português, inglês e espanhol (bandeiras no canto do painel; textos em
[`ircrecomp/i18n.py`](ircrecomp/i18n.py)).

#### Pela linha de comando

```bat
ircrecomp.bat caminho\para\IRC.cue
python -m ircrecomp build caminho\para\IRC.cue --out dist\IRC
python -m ircrecomp check --out dist\IRC
```

O resultado (`dist\IRC`):

| Item | Conteúdo |
|---|---|
| `IRC.exe` | o jogo nativo (64 bits) |
| `SDL3.dll` | biblioteca de plataforma |
| `game\` | dados do CD + músicas em FLAC sem perdas + saves |
| `irc_native.ini` | opções (tela cheia, proporção, resolução do 3D, filtros, limite de quadros) |
| `irc_build.json` | registro de integridade usado por Validar / `check` |

#### Tempo de recompilação

A recompilação se ajusta ao computador: as músicas são comprimidas em paralelo, em segundo plano e
com prioridade baixa, enquanto o jogo é traduzido e compilado (uma por núcleo físico, sem passar da
metade da memória livre); a compilação usa todos os núcleos disponíveis. Num Core i7-11700
(8 núcleos / 16 threads): **~26 s** com clang, ~42 s com MSVC. `IRC_JOBS=4` limita os núcleos usados.

### Controles

| Ação | Jogador 1 | Jogador 2 |
|---|---|---|
| Direção | `Z` / `X` | `←` / `→` |
| Acelerar / frear | `'` / `/` | `↑` / `↓` |
| Marchas (manual) | `;` / `.` | `PgUp` / `PgDn` |
| Câmera / pausa | `C` / `P` | |

As teclas podem ser redefinidas em Options › Settings. `Esc` abre o menu de pausa na corrida;
`Alt+Enter` alterna tela cheia / janela.

**Controles físicos (gamepad/volante): ainda não suportados.** O jogo tem um modo Joystick, mas com
controles modernos o carro fica freado, e os menus do jogo só aceitam teclado. A correção está em
andamento.

### O que funciona

- Abertura e vídeos FMV, menus, corridas, campeonato, replays e tela dividida para 2 jogadores.
- **Direct3D 5 reimplementado** com rasterizador próprio multi-thread: corridas em **alta
  resolução** (segue o monitor: 1080p → 1280×960, 1440p ou mais → 1920×1440, até 8× via
  `render_scale`), cor de 24 bits, filtro bilinear, transparências, neblina e sombras. O HUD e os
  menus 2D são combinados à imagem em alta resolução. O modo software 8 bits original continua
  disponível em Settings.
- Música do CD em FLAC e efeitos sonoros.
- Saves e configurações (`game\VAR`, `game\SAVEDATA`).
- Tela cheia sem bordas na resolução do monitor; `aspect=4:3` (padrão, barras laterais) ou `stretch`.
- Instalação em pastas com acentos.

Fora de escopo: multiplayer em rede ("Networked Rally" não conecta). Linux e macOS: o código foi
escrito para eles (`./ircrecomp.sh`), mas ainda não foi testado.

### Música

As faixas de áudio do CD são convertidas para **FLAC** com um codificador próprio
([`tools/flacenc`](tools/flacenc/flacenc.c)); cada faixa é decodificada de volta e comparada amostra
por amostra com o CD antes de ser aceita. 407 MB → 266 MB, sem perda nenhuma.

### Patch 4.80 da comunidade

O patch que circula na internet traz o `ral.exe` **4.80** (19/11/1998), mais antigo que o **4.81** do
próprio CD, vídeos substituídos por arquivos vazios, um `ral.zog` sem CD e um `ral.cfg` que força
ajustes de placas 3D de 1998. Nada disso é necessário: o executável recompilado já é a versão mais
nova, o CD é emulado e o Direct3D reimplementado oferece todos os recursos.

### Opções do `IRC.exe`

```
IRC.exe [--fullscreen|--window] [--stretch] [--smooth] [--fps N] [--render-scale N] [--no-d3d] [--data pasta]
```

Depuração: `--trace` (log detalhado), `--shots pasta` / `--shot-ms N` (capturas de tela),
`--keys "ms:Tecla/duração,..."` (teclas roteirizadas), `--exit-after ms`, `--record pasta`
(grava o áudio), `--peek endereço:tamanho` / `--poke endereço=byte@ms` (memória do jogo).

### Como funciona

```
IRC.cue ──► ingest (bincue + ISO 9660) ──► game\ + música FLAC
RAL.EXE ──► análise (pefile + capstone) ──► tradução x86 → C ──► clang/MSVC ──► IRC.exe
                                                     runtime\ (SDL3) ──┘
```

- [`ircrecomp/analysis.py`](ircrecomp/analysis.py) — descoberta de código por *recursive descent*,
  ponteiros relocados, tabelas de salto e detecção de funções sem retorno.
- [`ircrecomp/lifter.py`](ircrecomp/lifter.py) — uma função C por função x86; registradores e flags
  como variáveis locais, com análise de vivacidade de flags.
- [`runtime/`](runtime/) — memória do jogo numa janela de 4 GB endereçada por offsets de 32 bits,
  despacho de chamadas indiretas, threads e as APIs reimplementadas (`w32_*.c`, `dx_*.c`).

`RAL.EXE` 4.81: 1.921 funções, 87 mil instruções, ~364 mil linhas de C geradas.

### Verificação da tradução

[`tests/semtest`](tests/semtest/semtest.py) compara o C gerado com o emulador de CPU **Unicorn**:
cada forma de instrução usada pelo jogo e todos os blocos básicos reais rodam com estados aleatórios
nos dois lados, comparando registradores, flags e memória. Varredura completa: **379.250 casos
cobrindo todos os 35.784 blocos básicos, nenhum erro de tradução** (as únicas diferenças vêm de bits
de sistema do `popfd` que o jogo não usa e de um bug do próprio Unicorn).

Veja [PESQUISA.md](PESQUISA.md) para a pesquisa e o roteiro completo.

### Licença

O recompilador, o runtime e as ferramentas estão sob a [licença MIT](LICENSE).
[`dr_flac`](third_party/dr_libs/dr_flac.h) é de domínio público / MIT-0. O SDL3 (zlib) e o llvm-mingw
são baixados das páginas oficiais. International Rally Championship © Magnetic Fields / Europress.

---

## English

A static recompiler (`ircrecomp`) that turns **International Rally Championship** (Magnetic Fields /
Europress, 1997) into a native 64-bit executable for modern Windows — no emulator, no legacy DirectX
and no CD needed.

The original `RAL.EXE` was **hand-written x86 assembly**. `ircrecomp` translates every function into
portable C and links it with a new runtime that reimplements the Windows 98 / DirectX 5 APIs on top
of [SDL3](https://libsdl.org). The game's code is **not distributed**: the C is generated on the
machine of whoever owns the CD.

### Requirements

- Windows 10/11 64-bit (tested on Windows 11), Python 3.10+ and the CD image (`.cue` + `.bin`).
- No Visual Studio needed: on the first recompile the C compiler
  ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw), clang + lld, ~180 MB) and SDL3 are downloaded
  automatically from their official pages, with pinned versions and verified SHA-256, and kept in
  `third_party\`. If Visual Studio Build Tools (C++) is installed and clang is not there yet, it is
  used instead. To choose: `--compiler clang|msvc` or the `IRC_COMPILER` variable.
- ~2.5 GB free on the first recompile (compiler included); the installed game takes ~370 MB.

### Usage

#### With the window (recommended)

Double-click **`IRC Recompilador.bat`** (the first time, it sets up the Python environment itself):

1. **Browse…** for the CD image (`.cue`; if you pick the `.bin`, the `.cue` next to it is used) and the
   install folder (accented characters are fine).
2. Check the requirements (green = ok).
3. **Recompile the game** — a progress bar over the 5 steps and live details.
4. **▶ Play**, **Settings…** (fullscreen, aspect ratio, 3D resolution, frame limit, filters),
   **Create shortcut** and **Open folder**.

Only enabled when the chosen folder holds a build:

- **Verify** — checks the essential files and the 12 music tracks, compares every file with the
  integrity record written by the recompile (`irc_build.json`: size and SHA-1; saves and settings are
  excluded) and runs the game in a window for a few seconds to see it start and close normally.
- **Delete** — after confirmation, removes everything the recompile created (executable, data, music,
  saves and the shortcut pointing to it). The CD image is never touched.

The window follows the look of the game's menus. The logo and rally photos at the top are read from
the player's own CD image and cached in `build\ui_assets` — no game artwork ships with the tool. It is
available in Portuguese, English and Spanish (flags in the corner of the panel; texts in
[`ircrecomp/i18n.py`](ircrecomp/i18n.py)).

#### From the command line

```bat
ircrecomp.bat path\to\IRC.cue
python -m ircrecomp build path\to\IRC.cue --out dist\IRC
python -m ircrecomp check --out dist\IRC
```

The result (`dist\IRC`):

| Item | Contents |
|---|---|
| `IRC.exe` | the native game (64-bit) |
| `SDL3.dll` | platform library |
| `game\` | CD data + lossless FLAC music + saves |
| `irc_native.ini` | options (fullscreen, aspect, 3D resolution, filters, frame limit) |
| `irc_build.json` | integrity record used by Verify / `check` |

#### Recompile time

The recompile adapts to the computer: the music is compressed in parallel, in the background and at
low priority, while the game is translated and compiled (one track per physical core, never more than
half of the free memory); compiling uses every available core. On a Core i7-11700 (8 cores / 16
threads): **~26 s** with clang, ~42 s with MSVC. `IRC_JOBS=4` limits the cores used.

### Controls

| Action | Player 1 | Player 2 |
|---|---|---|
| Steer | `Z` / `X` | `←` / `→` |
| Accelerate / brake | `'` / `/` | `↑` / `↓` |
| Gears (manual) | `;` / `.` | `PgUp` / `PgDn` |
| Camera / pause | `C` / `P` | |

Keys can be redefined in Options › Settings. `Esc` opens the pause menu during a race; `Alt+Enter`
toggles fullscreen / window.

**Physical controllers (gamepad/wheel): not supported yet.** The game has a Joystick mode, but with
modern controllers the car stays braked, and the game's menus only accept the keyboard. A fix is in
progress.

### What works

- Intro and FMV videos, menus, races, championship, replays and 2-player split screen.
- **Direct3D 5 reimplemented** with a custom multithreaded rasterizer: races in **high resolution**
  (follows the monitor: 1080p → 1280×960, 1440p and up → 1920×1440, up to 8× with `render_scale`),
  24-bit color, bilinear filtering, transparency, fog and shadows. The 2D HUD and menus are merged
  into the high-resolution image. The original 8-bit software mode is still available in Settings.
- CD music as FLAC, and sound effects.
- Saves and settings (`game\VAR`, `game\SAVEDATA`).
- Borderless fullscreen at the monitor's resolution; `aspect=4:3` (default, side bars) or `stretch`.
- Installing into folders with accented characters.

Out of scope: network multiplayer ("Networked Rally" does not connect). Linux and macOS: the code was
written for them (`./ircrecomp.sh`) but has not been tested yet.

### Music

The CD audio tracks are converted to **FLAC** with a built-in encoder
([`tools/flacenc`](tools/flacenc/flacenc.c)); every track is decoded back and compared sample by
sample with the CD before it is accepted. 407 MB → 266 MB, with no loss at all.

### The community 4.80 patch

The patch found on the internet ships `ral.exe` **4.80** (1998-11-19), older than the **4.81** on the
CD itself, videos replaced by empty files, a CD-less `ral.zog` and a `ral.cfg` forcing settings for
1998 3D cards. None of it is needed: the recompiled executable is already the newest version, the CD
is emulated and the reimplemented Direct3D offers every feature.

### `IRC.exe` options

```
IRC.exe [--fullscreen|--window] [--stretch] [--smooth] [--fps N] [--render-scale N] [--no-d3d] [--data folder]
```

Debugging: `--trace` (detailed log), `--shots folder` / `--shot-ms N` (screenshots),
`--keys "ms:Key/duration,..."` (scripted keys), `--exit-after ms`, `--record folder` (records the
audio), `--peek address:size` / `--poke address=byte@ms` (game memory).

### How it works

```
IRC.cue ──► ingest (bincue + ISO 9660) ──► game\ + FLAC music
RAL.EXE ──► analysis (pefile + capstone) ──► x86 → C translation ──► clang/MSVC ──► IRC.exe
                                                       runtime\ (SDL3) ──┘
```

- [`ircrecomp/analysis.py`](ircrecomp/analysis.py) — recursive-descent code discovery, relocated
  pointers, jump tables and no-return function detection.
- [`ircrecomp/lifter.py`](ircrecomp/lifter.py) — one C function per x86 function; registers and flags
  as local variables, with flag liveness analysis.
- [`runtime/`](runtime/) — game memory in a 4 GB window addressed by 32-bit offsets, indirect call
  dispatch, threads and the reimplemented APIs (`w32_*.c`, `dx_*.c`).

`RAL.EXE` 4.81: 1,921 functions, 87k instructions, ~364k lines of generated C.

### Translation verification

[`tests/semtest`](tests/semtest/semtest.py) compares the generated C with the **Unicorn** CPU
emulator: every instruction form used by the game and every real basic block run from random states
on both sides, comparing registers, flags and memory. Full sweep: **379,250 cases covering all 35,784
basic blocks, no translation errors** (the only differences come from `popfd` system bits the game
never uses and from a bug in Unicorn itself).

See [PESQUISA.md](PESQUISA.md) (in Portuguese) for the research and the full roadmap.

### License

The recompiler, runtime and tools are under the [MIT license](LICENSE).
[`dr_flac`](third_party/dr_libs/dr_flac.h) is public domain / MIT-0. SDL3 (zlib) and llvm-mingw are
downloaded from their official pages. International Rally Championship © Magnetic Fields / Europress.

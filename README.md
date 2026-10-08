# International Rally Championship — versão nativa recompilada

Recompilador estático (`ircrecomp`) que transforma o jogo **International Rally Championship**
(Magnetic Fields / Europress, 1997, versão 4.81) num executável nativo moderno, sem emulador,
sem DirectX antigo e sem precisar do CD.

O `RAL.EXE` original foi escrito em **assembly x86 à mão**. O `ircrecomp` traduz cada função
para C portável e o liga a um runtime novo que reimplementa as APIs do Windows 98 / DirectX 5
sobre o [SDL3](https://libsdl.org).

## Como usar

Requisitos (Windows): só o Python 3.10+ e a imagem do CD. Nada de Visual Studio: na primeira
recompilação o compilador C ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw), clang + lld,
~180 MB) e o SDL3 são baixados automaticamente das páginas oficiais, com versão fixa e SHA-256
conferido, e ficam em `third_party\` para as próximas vezes. Sem o clang baixado, o Visual Studio
Build Tools (carga de trabalho C++) é usado se estiver instalado; com o clang presente ele é o
preferido, por compilar o jogo ~4× mais rápido com o mesmo resultado. Para escolher, use
`--compiler clang` ou `--compiler msvc` na linha de comando (ou a variável `IRC_COMPILER`).

A recompilação se ajusta ao computador: as 12 músicas são comprimidas em paralelo, em segundo
plano e com prioridade baixa, enquanto o jogo é traduzido e compilado, uma por núcleo físico
disponível (menos um, deixado para a tradução) e sem passar da metade da memória livre; a
compilação usa todos os núcleos lógicos que o processo pode usar. Tempos medidos num Core
i7-11700 (8 núcleos / 16 threads): ~26 s com clang, ~42 s com MSVC; limitado a 4 threads, ~30 s.
Para deixar núcleos livres enquanto recompila, defina `IRC_JOBS` (por exemplo `IRC_JOBS=4`).

### Com janela (recomendado)

Dê dois cliques em **`IRC Recompilador.bat`**. Na janela:

1. **Escolher…** a imagem do CD (`.cue`; se escolher o `.bin`, o `.cue` ao lado é usado) e a
   pasta onde o jogo será instalado.
2. Confira os requisitos (verde = ok; o compilador C aparece como "será baixado automaticamente"
   até a primeira recompilação).
3. **Recompilar o jogo** — barra de progresso pelas 5 etapas e detalhes ao vivo (~30 s num PC de 8 núcleos).
4. **▶ Jogar**, **Configurações…** (tela cheia, proporção, resolução do 3D, limite de quadros,
   filtros), **Criar atalho na área de trabalho** e **Abrir pasta**.

Ao lado de Recompilar, ativos só quando já existe uma compilação na pasta escolhida:

- **Validar** — confere se os arquivos essenciais e as 12 músicas estão lá, compara cada arquivo
  com o registro de integridade gravado pela recompilação (`irc_build.json`: tamanho e SHA-1;
  saves e configurações em `game\VAR` e `game\SAVEDATA` ficam de fora) e abre o jogo em janela por
  alguns segundos para ver se ele inicia, carrega e fecha normalmente.
- **Apagar** — depois de confirmar, remove tudo o que a recompilação criou (executável, dados,
  músicas, saves, `_work` e o atalho da área de trabalho que aponta para ele). Só apaga pastas que
  têm cara de instalação do recompilador; a imagem do CD não é tocada.

A janela lembra a última imagem e pasta usadas. Na primeira abertura ela prepara o ambiente
Python sozinha. A pasta de instalação pode ter acentos (o executável usa UTF-8 como página de
código do Windows).

O visual segue os menus do jogo (fundo magenta, painel azul-marinho, letras itálicas amarelas).
O logotipo oficial e as fotos de rali que se alternam no topo (clique na foto para trocar) são lidos
da **imagem do CD do próprio jogador** (`BLUELOGO`, `CLOAD00–26` em `FILES\GFX`) e guardados como
PNG em `build\ui_assets` — nenhuma arte do jogo vem junto com a ferramenta. Sem o CD, a janela usa
só as cores e um logotipo em texto.

A janela abre em português; as bandeiras no canto superior direito do painel trocam para
inglês ou espanhol na hora (a escolha fica salva). Os textos estão em
[`ircrecomp/i18n.py`](ircrecomp/i18n.py).

### Pela linha de comando

```bat
ircrecomp.bat caminho\para\IRC.cue
```

Para validar uma instalação: `python -m ircrecomp check --out dist\IRC`.

Em ~30 segundos é criada a pasta `dist\IRC` com:

| Item | Conteúdo |
|---|---|
| `IRC.exe` | o jogo nativo (64 bits) |
| `SDL3.dll` | biblioteca de plataforma |
| `game\` | dados do CD + músicas em FLAC sem perdas (`music\track02..13.flac`) + saves |
| `irc_native.ini` | opções (escala da janela, tela cheia, filtro, limite de FPS) |

Linux/macOS: `./ircrecomp.sh caminho/para/IRC.cue` (precisa de `cmake` e um compilador C; o SDL3
é compilado a partir do código-fonte se não estiver instalado). Ainda não testado nesses sistemas.

O código do jogo **não é distribuído**: o C é gerado na máquina de quem tem o CD.

## Controles (padrão original do jogo)

| Ação | Jogador 1 | Jogador 2 |
|---|---|---|
| Direção | `Z` / `X` | `←` / `→` |
| Acelerar / frear | `'` / `/` | `↑` / `↓` |
| Marchas (manual) | `;` / `.` | `PgUp` / `PgDn` |
| Câmera / pausa | `C` / `P` | |

Os controles podem ser redefinidos no menu de opções do jogo. Gamepads e volantes funcionam
via DirectInput emulado (gatilho direito acelera, esquerdo freia; vibração para force feedback).
`Alt+Enter` alterna tela cheia. `Esc` no menu principal sai do jogo.

## O que já funciona

- Abertura e vídeos FMV, menus em 800×600×16
- **Modo acelerado (Direct3D 5) reimplementado** com rasterizador próprio multi-thread:
  corridas renderizadas em **alta resolução** (padrão 1280×960, até 8× via `render_scale`),
  cor de 24 bits, filtro bilinear, transparências, neblina e sombras — todos os efeitos que o
  jogo só liga em placas 3D "completas". O HUD 2D do jogo é combinado por detecção de diferenças.
  É o modo gráfico padrão; o modo software 8 bits original continua disponível em Settings.
- Música de CD (faixas do disco tocadas a partir dos WAV), efeitos sonoros (DirectSound primário)
- Teclado, mouse, joysticks/gamepads (SDL3), force feedback como vibração
- Saves e configurações (`game\VAR`, `game\SAVEDATA`)
- Ritmo de quadros igual ao monitor (o original dependia do vsync do `Flip`)
- Encerramento limpo, sem crashes nos testes automatizados

Fora de escopo por decisão do projeto: multiplayer em rede (o menu "Networked Rally" não conecta).

### Tela
Abre em tela cheia sem bordas na resolução do monitor; `Alt+Enter` alterna para janela
(ajustada a 85% da área útil, redimensionável). A imagem acompanha qualquer tamanho:
`aspect=4:3` (padrão, barras laterais em telas widescreen) ou `aspect=stretch` (preenche a tela).
A resolução interna do 3D segue o monitor (`render_scale=0`): 1080p → 1280×960, 1440p ou maior →
1920×1440 (~55–60 fps num Core i7 de 8 núcleos).

### Música
As faixas de áudio do CD são convertidas para **FLAC** (sem perdas: cada faixa é decodificada de
volta e comparada amostra por amostra com o CD antes de ser aceita). 407 MB → 266 MB. MP3 foi
descartado por perder qualidade.

### Patch 4.80 da comunidade
O patch encontrado na internet traz o `ral.exe` **4.80** (19/11/1998), mais antigo que o **4.81**
(09/12/1998) do próprio CD; vídeos FMV substituídos por arquivos vazios de 800 bytes (só pulam a
abertura); um `ral.zog` com `cdrom=.` (sem CD); e um `ral.cfg` que força ajustes para placas 3D de
1998 (`force16bittex`, `force3dtrans/alpha/shadow/fogging`, `triplebuffered`). Nada disso é necessário
aqui: o executável recompilado já é a versão mais nova, o CD é emulado, e o Direct3D reimplementado
oferece todos os recursos, que o jogo detecta sozinho com os mesmos valores (texturas de 8 bits com
paleta são até melhores que as forçadas em 16 bits).

## Opções de linha de comando

```
IRC.exe [--fullscreen] [--scale N] [--smooth] [--fps N] [--render-scale N] [--no-d3d] [--data pasta]
```

Depuração: `--trace` (log detalhado), `--shots pasta` (captura 1 quadro/s em BMP),
`--keys "ms:Tecla/duração,..."` (teclas roteirizadas), `--exit-after ms`, `--record pasta`
(grava o áudio em WAV), `--peek endereço:tamanho` / `--poke endereço=byte@ms` (lê/escreve a
memória do jogo).

## Como funciona

```
IRC.cue ──► ingest (bincue + ISO 9660) ──► game\ + music\
RAL.EXE ──► análise (pefile + capstone) ──► tradução x86→C ──► MSVC/clang ──► IRC.exe
                                                     runtime\ (SDL3) ──┘
```

- [`ircrecomp/analysis.py`](ircrecomp/analysis.py) — descoberta de código por *recursive descent*
  a partir do entry point, ponteiros relocados e tabelas de salto; detecção de funções sem retorno.
- [`ircrecomp/lifter.py`](ircrecomp/lifter.py) — uma função C por função x86; registradores e
  flags como variáveis locais; análise de vivacidade de flags; `xchg` atômico; barreiras em laços.
- [`runtime/`](runtime/) — memória do jogo numa janela de 4 GB endereçada por offsets de 32 bits
  (o C gerado não depende do tamanho de ponteiro do host), carregador da imagem, despacho de
  chamadas indiretas, threads, e as APIs reimplementadas:
  `w32_kernel32.c`, `w32_user32.c`, `w32_winmm.c` (CD-áudio MCI), `dx_ddraw.c`, `dx_dsound.c`,
  `dx_dinput.c`, `dx_d3d.c`, `dx_dplay.c`.

## Verificação da tradução

[`tests/semtest/semtest.py`](tests/semtest/semtest.py) compara o C gerado com o emulador de CPU
de referência **Unicorn**: cada forma de instrução usada pelo jogo e milhares de blocos básicos reais
rodam com estados aleatórios nos dois lados (memória materializada sob demanda com o mesmo
conteúdo pseudoaleatório), comparando registradores, flags definidas e páginas de memória.
Resultados:
- amostra padrão: 0 divergências em ~89 mil casos (1.201 instâncias de formas + 2.500 blocos);
- varredura completa (`--blocks 100000`): **379.250 casos cobrindo todos os 35.784 blocos básicos**;
  as únicas diferenças vêm de `popfd` com bits de sistema aleatórios (Trap Flag/IOPL, que o jogo nunca
  usa) e de um bug do próprio Unicorn (CF errado após `add` + `rep stos/movs` com falha de página,
  reproduzido isoladamente). Nenhum erro de tradução.

```bat
.venv\Scripts\python -I tests\semtest\semtest.py game\RAL.EXE --cases 24 --blocks 2500
```

Números do `RAL.EXE` 4.81: 1.673 funções, 74 mil instruções, ~300 mil linhas de C, 84 funções
de Windows importadas.

Veja [PESQUISA.md](PESQUISA.md) para a pesquisa e o roteiro completo.

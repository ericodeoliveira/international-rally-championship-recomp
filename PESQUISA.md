# International Rally Championship (1997) — Recompilação nativa para Windows 11

Pesquisa técnica e arquitetura de um recompilador automático (`ircrecomp`).

---

## 1. O que existe nesta pasta

| Item | Detalhe |
|---|---|
| `IRC.iso.bin` / `.cue` | CD de modo misto: **trilha 1 = dados** (MODE1/2352, 53.932 setores, ~106 MB) + **trilhas 2–13 = áudio CD** (trilha sonora, ~46 min) |
| Versão | `$VER: Rally 4.81 (9.12.98)` — já é a versão mais recente (inclui o patch 4.80 da comunidade e mais) |
| Desenvolvedora | Magnetic Fields (Software Design) Ltd. — Shaun e Andrew Southern (série *Lotus*, Amiga) |
| Executáveis | `RAL.EXE` (jogo, 2,1 MB), `SETUP.EXE` / `UNRAL.EXE` (instalador/desinstalador), `REDIST\DIRECTX` (DirectX 5) |

Extração feita com [tools/bincue.py](tools/bincue.py) → `extracted/track01.iso`, `extracted/track02..13.wav`, `extracted/cd/`.

---

## 2. Análise do `RAL.EXE`

### 2.1 Não existe código-fonte em C para "recompilar"

- Sem *Rich header*, linker 2.60 não-Microsoft, **nenhuma string de runtime C**, seção custom `GURUS`.
- ~89 mil instruções em 350 KB de código; só 11 prólogos `push ebp / mov ebp,esp`; 635 `pushal`.
- Convenção de chamada **por registradores**, matemática em **ponto fixo** (FPU só em 152 instruções — `fild/fstp` para converter vértices para o Direct3D), MMX em 42 instruções (cópia de memória).
- Strings estilo Amiga (`$VER:`, `Rally: Guru Meditation`, dump de registradores no crash).

**Conclusão: o jogo foi escrito em assembly x86 à mão.** Não há "fonte" a recuperar — a única forma automática de torná-lo nativo é **recompilação estática** (traduzir o binário x86 para C e compilar de novo) + uma **camada de plataforma nova** no lugar do DirectX 5 / Win9x.

Precedentes de jogos em assembly trazidos para código moderno: *Transport Tycoon* → OpenTTD, *RollerCoaster Tycoon* → OpenRCT2 (ambos começaram enganchando o executável original e substituindo funções aos poucos).

### 2.2 Viabilidade para recompilação estática — muito boa

| Métrica ([tools/discover.py](tools/discover.py)) | Valor | Por que importa |
|---|---|---|
| `.text` gravável? | **Não** | Sem código auto-modificável |
| Relocações base | **28.698** | Distingue ponteiro de constante com precisão → dá para relocar/recompilar sem adivinhar |
| Funções descobertas (1ª passada) | **1.654** | Escopo pequeno (jogos comerciais típicos: 10–30 mil) |
| Cobertura do código | **84,4 %** | O resto é alcançável por ponteiros/heurísticas |
| Tabelas de salto | 1 (8 entradas) | Assembly à mão quase não usa `switch` |
| Saltos indiretos não resolvidos | 67, quase todos `jmp [IAT]` (thunks de import) | Na prática ~1 caso real (`jmp [esi]`) |
| Ponteiros de código em dados | 322 | Callbacks / tabelas de estado → viram tabela de despacho |

Pegadinha: a seção `GURUS` (não executável pelos flags) **contém código** — stubs de erro `mov [errcode],X / jmp handler`. O recompilador deve tratá-la como código.

### 2.3 Superfície de sistema (o que a camada de plataforma precisa substituir)

Importações diretas: só ~84 funções.

| DLL | Uso |
|---|---|
| KERNEL32 (49) | arquivos, threads, `VirtualAlloc`, `GetDriveTypeA`/`GetVolumeInformationA` (**checagem do CD**) |
| USER32 (19) / GDI32 (1) | janela e loop de mensagens |
| WINMM (5) | `mciSendCommandA` → **música de CD-áudio**; `aux*Volume` |
| DDRAW (2) | `DirectDrawCreate` → `IDirectDraw2`, `IDirectDrawSurface2`, **640×480×8 com paleta**, 800×600×16 |
| Direct3D (via QueryInterface) | `IDirect3D2`, `IDirect3DDevice2` (DrawPrimitive DX5), `IDirect3DTexture2`; devices HAL/RGB/Ramp/MMX |
| ATI3DCIF (LoadLibrary) | API proprietária ATI Rage — basta reportar "ausente" |
| DSOUND (1) | buffer primário/secundário |
| DINPUT (1) | teclado, mouse, joystick, **force feedback** (constant/spring/damper) |
| DPLAYX (ord. #1, #9) | multiplayer em rede (DirectPlay) |
| ADVAPI32 (4) | registro `HKLM\SOFTWARE\Magnetic Fields\IRC` |

Todo o resto — renderizador por software, física, IA, decodificador de FMV próprio (`FILES\FMV`), formatos `.BUN/.GFX/CRSxxDAT` — está **dentro do assembly** e é recompilado "de graça".

Opções de `VAR\RAL.CFG` úteis: `forceno3dcard=1` (só o renderizador por software 8-bit — **reduz drasticamente o que precisa ser implementado na fase 1**), `nocdaudio`, `nofmv`, `logfile=1` (gera `var\logfile.txt`, útil para depurar o runtime).

### 2.4 Problemas conhecidos no Windows moderno (comunidade)

- `SETUP.EXE` exige DirectDraw 640×480×8 e costuma falhar → instalação manual.
- Proteção de CD (`var\ral.zog` com campos `cdrom`/`installation` + checagem de volume) → "Guru Meditation" sem o disco.
- Paleta 8-bit, tela preta até a janela receber foco, menu de pausa invisível no modo 16-bit acelerado.
- Som DirectSound com eco/estalos em CPUs rápidas (só melhora com *slowdown* no DxWnd).
- Música de CD não toca a partir de imagem sem drive virtual.
- Crash "Guru Meditation 80208517" ao sair de uma corrida em alguns wrappers.

O recompilador resolve todos eles na camada de plataforma, sem *crack*: o runtime **simula** o drive de CD apontando para os arquivos extraídos.

---

## 3. Arquitetura do `ircrecomp` (o software automático)

Uso pretendido:

```
ircrecomp build IRC.iso.cue --out build/
→ build/IRC.exe (nativo) + build/data/ (assets extraídos) + build/music/*.flac
```

O usuário fornece a própria imagem do CD; o código do jogo **nunca é distribuído** — é gerado na máquina dele (mesmo modelo de N64Recomp / projetos *recomp*).

```
 IRC.iso.bin/.cue
        │  1. Ingestão
        ▼
 ISO + WAVs ──► instalação virtual (FILES, VAR, ral.zog gerado, ral.cfg)
        │
 RAL.EXE│  2. Análise
        ▼
 PE + relocs ─► descoberta de funções/blocos ─► CFG ─► mapa de imports/COM
        │  3. Lifter
        ▼
 C gerado (1 função C por função x86, goto por bloco, flags preguiçosas)
        │  4. Compilação (CMake + clang-cl/MSVC)
        ▼
 runtime/ (plataforma: SDL3 + D3D11, áudio, input, CD virtual) ──► IRC.exe
        │  5. Validação
        ▼
 testes diferenciais por função (Unicorn executa o x86 original vs C gerado)
```

### Etapa 1 — Ingestão  *(feito: `tools/bincue.py`)*
- Parse do `.cue`, extração da trilha de dados → ISO; trilhas de áudio → WAV (converter para FLAC/OGG).
- Extrair ISO9660, montar a árvore de instalação, gerar `var\ral.zog`, `var\irc.cfg` e `ral.cfg` padrão. Não depender do `SETUP.EXE`.
- Verificar hash do `RAL.EXE` (suportar só versões conhecidas — 4.81 desta imagem; depois a versão PT-BR, a demo etc.).

### Etapa 2 — Análise  *(protótipo: `tools/discover.py`)*
- Sementes: entry point + alvos de `call` direto + ponteiros de código achados via relocações + entradas da tabela de salto.
- Recursive descent até 100 % de cobertura; o que sobrar vai para revisão manual num arquivo de configuração (`irc.toml`: limites de função, alvos de saltos indiretos, dados no meio do código).
- Resolver thunks `jmp [IAT]` → chamadas para o runtime.
- Identificar chamadas COM (`call [reg+off]` após `QueryInterface`) — tratadas pelo runtime via vtables falsas.

### Etapa 3 — Lifter x86-32 → C
Modelo recomendado (o mesmo de xboxrecomp / systemes3recomp, ajustado):

- **Contexto da CPU**: struct com `eax..edi`, `esp`, flags; registradores como variáveis locais dentro de cada função (o compilador os aloca em registradores reais).
- **Flags preguiçosas** + eliminação de flags mortas por bloco (o assembly à mão encadeia `adc`/`sbb`/`shrd` — precisa estar exato).
- **Pilha do convidado** dentro da memória do jogo (o código usa `pushal/popal` e manipula `esp` livremente); `call` empilha o endereço de retorno real; `ret` confere.
- **Memória**: imagem mapeada no endereço original `0x400000` (todos os ponteiros absolutos continuam válidos).
- **Despacho indireto**: tabela endereço-x86 → função C para os 322 ponteiros de código + `call reg`.
- **Instruções**: cobrir o subconjunto realmente usado (inteiro, `rep movs/stos`, `shld/shrd`, ~15 opcodes FPU, ~4 MMX). Gerar estatística de cobertura e falhar o build se aparecer opcode desconhecido.
- Saída: ~1.650 funções em algumas dezenas de arquivos `.c`.

**Alvo 32 bits vs 64 bits**
- **Fase 1 — Win32 (x86)**: o Windows 11 x64 roda 32 bits nativamente (WoW64). Mais simples: ponteiros do jogo = ponteiros do host.
- **Fase 2 — x64 / ARM64**: memória do jogo vira um bloco reservado de 4 GB (`mem_base + addr`); o runtime traduz ponteiros nas chamadas de API. Necessário para Windows on ARM sem emulação.

### Etapa 4 — Runtime (camada de plataforma) — **onde está a maior parte do trabalho**

| Original | Substituto moderno | Observação |
|---|---|---|
| DirectDraw2 8-bit + paleta | Objetos COM falsos que escrevem num buffer em RAM; apresentação via **SDL3 + D3D11**, conversão de paleta em shader | Janela/tela cheia, escala inteira ou suavizada, DPI, vsync |
| Direct3D2 DrawPrimitive (modo 16-bit acelerado) | Tradução para D3D11 (ou SDL_GPU) | Fase 2 — permite resolução maior que 800×600 e filtragem. Na fase 1 usar `forceno3dcard=1` |
| ATI3DCIF | Reportar ausente | — |
| DirectSound | SDL3 audio / XAudio2 | Corrigir temporização do buffer (fonte do eco em CPU rápida) |
| DirectInput 5 + FF | SDL3 gamepad/joystick + rumble/haptic | XInput, volantes modernos, rebind |
| MCI CD audio + `aux*Volume` | Player de FLAC/OGG das faixas extraídas | Implementa `MCI_PLAY` from/to em MSF/TMSF |
| `GetDriveTypeA`/`GetVolumeInformationA` | **Drive de CD virtual** (rótulo de volume original) | Sem crack, sem modificar o jogo |
| Arquivos (`CreateFileA` etc.) | Redirecionar para `data\`, case-insensitive, saves em `%APPDATA%` | — |
| Registro ADVAPI32 | Arquivo `.ini` | — |
| DirectPlay | Stub (fase 1) → UDP próprio (fase 3) | Multiplayer em rede é opcional |
| Timing (`GetTickCount`, `Sleep`, threads) | Relógio de alta resolução + limitador de quadros | Jogos dessa época quebram em CPUs rápidas |
| Tela "Guru Meditation" | Log + minidump com endereço x86 original | Facilita depuração |

### Etapa 5 — Validação automática
- **Teste diferencial por função**: executar a função original no **Unicorn Engine** e a versão em C com o mesmo estado de registradores/memória; comparar o estado final. Roda em massa no CI.
- **Replays determinísticos**: o jogo tem sistema de replay (`savedata\replay`, `files\replay\rintro00`) — reproduzir um replay nas duas versões e comparar quadros/checksums de memória.
- Comparação visual com o original rodando em dgVoodoo2/DxWnd (ou em uma VM Win98 / 86Box).

---

## 4. Roteiro sugerido

| Fase | Entrega | Critério de pronto |
|---|---|---|
| 0 | Rodar o original com wrappers (dgVoodoo2 ou DDrawCompat + wrapper de winmm para CD-áudio) | Referência jogável para comparação |
| 1 | Ingestão + análise completas; lifter gera C que compila | 100 % das funções levantadas, zero opcode desconhecido |
| 2 | Runtime 2D: janela, DirectDraw 8-bit, arquivos, CD virtual, teclado | Menu e corrida com `forceno3dcard=1` |
| 3 | Áudio (DSound + música), gamepad, force feedback, FMV | Jogo completo jogável |
| 4 | Testes diferenciais + replays no CI | Replays idênticos ao original |
| 5 | Direct3D2 → D3D11; alta resolução; widescreen (exige patches no código) | Modo acelerado sem os bugs conhecidos |
| 6 | Build x64/ARM64; DirectPlay → UDP | Opcional |

---

### Status (8 de outubro de 2026)

| Fase | Status |
|---|---|
| 0 | Dispensada: o jogo nativo já serve de referência |
| 1 | ✅ Lifter completo: 1.921 funções, 98,7 % do código, 4 sítios não suportados (dados, nunca executados) |
| 2 | ✅ Runtime 2D sobre SDL3; o jogo nunca toca o Windows real (84 APIs + DirectX 5 reimplementados) |
| 3 | ✅ Jogo completo jogável: FMV, menus, corridas, música de CD, efeitos, teclado/mouse/gamepad |
| 4 | ✅ Testes diferenciais contra o Unicorn: 0 divergências (todas as formas de instrução + blocos reais) |
| 5 | ✅ Direct3D 5 reimplementado com rasterizador próprio multi-thread em alta resolução (2× padrão, até 8×), cor de 24 bits; ⏳ widescreen |
| 6 | ✅ Saída 64 bits portável (memória do jogo como janela de 4 GB); ⏳ DirectPlay; ⏳ teste em Linux/macOS/ARM64 |

Descobertas que mudaram o plano:
- O jogo **espera `WM_ACTIVATEAPP`** antes de iniciar (causa da "tela preta até clicar" nos fóruns).
- O idioma de detecção de CPUID (bit 21 do EFLAGS) exige preservar bits não modelados em `pushfd/popfd`
  — achado pelo teste diferencial; sem isso o jogo desligava o caminho MMX.
- Em vez de traduzir Direct3D para D3D11, um rasterizador próprio na CPU (com faixas em paralelo)
  evitou toda a complexidade de threads de GPU e permitiu renderizar em qualquer resolução com o HUD
  2D do jogo combinado por diferença de pixels.

## 5. Ferramentas e dependências

- **Análise/lifter**: Python 3 + `pefile` + `capstone` (já no `.venv`); alternativa para o lifter: gerar LLVM IR com **remill** (Trail of Bits) — mais robusto, porém mais pesado e a saída não é C legível.
- **Build**: CMake + clang-cl ou MSVC (Visual Studio Build Tools) — **ainda não instalados nesta máquina**.
- **Runtime**: SDL3, D3D11, dr_flac/stb_vorbis.
- **Validação**: Unicorn Engine.
- **Referência**: Ghidra para navegar/nomear funções manualmente quando necessário.

## 6. Aspectos legais

"Abandonware" não é domínio público: os direitos do IRC continuam com quem detém o catálogo da Europress. Por isso o projeto deve distribuir **só o recompilador e o runtime**; o C gerado e os assets ficam na máquina de quem tem o CD.

---

## Fontes

- [International Rally Championship — Wikipedia](https://en.wikipedia.org/wiki/International_Rally_Championship)
- [VOGONS — como instalar se o setup.exe não funciona](https://www.vogons.org/viewtopic.php?t=42285)
- [DxWnd — discussão sobre IRC (ral.zog, CD-áudio, som, Guru Meditation)](https://sourceforge.net/p/dxwnd/discussion/general/thread/437d56d0/)
- [systemes3recomp — recompilação estática de executáveis Win32 x86](https://github.com/sp00nznet/systemes3recomp)
- [xboxrecomp — x86 → C → executável nativo](https://github.com/sp00nznet/xboxrecomp)
- [The Collection Chamber — IRC](https://collectionchamber.blogspot.com/p/international-rally-championship.html)

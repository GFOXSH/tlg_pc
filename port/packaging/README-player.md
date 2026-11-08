# The Last Guardian — port para PC / PC port

Por HayatoG · YouTube: [@hayatog-dev](https://www.youtube.com/@hayatog-dev) · Discord: [discord.gg/UMkdMYZh9g](https://discord.gg/UMkdMYZh9g) · Código: [github.com/HayatoG/tlg_pc](https://github.com/HayatoG/tlg_pc)

**Nenhum arquivo do jogo vem neste pacote.** Você precisa da sua própria cópia de The Last Guardian para PS4. / **No game files are included.** You need your own copy of The Last Guardian for PS4.

Versão experimental: o jogo roda do início ao gameplay, mas ainda pode travar em áreas não testadas. / Experimental build: the game runs from boot to gameplay, but may still crash in untested areas.

---

## Português

### Arquivos do jogo necessários

Tudo vem do **seu** PS4 (GoldHEN + Itemzflow). Nada disso vai no download. Use **The Last Guardian CUSA03627 (EUA) com o update 01.03**, montado numa pasta só, com o jogo base e o update por cima. São ~25 GB.

| Arquivo | Vem de | Tamanho | Para quê |
|---|---|---|---|
| `eboot.bin` | update 01.03 | 89 MB | o código do jogo (tem que ser o do 1.03) |
| `sce_module/libc.prx`, `sce_module/libSceFios2.prx` | update 01.03 | 1,5 MB | bibliotecas do próprio jogo |
| `sce_sys/` (`param.sfo`, `playgo-chunk.dat`, `icon0.png`, `pic0.png`, `pic1.png`) | jogo base + update | ~5 MB | metadados, instalação e a arte do launcher |
| `trico_boot.psarc`, `trico_001.psarc`, `trico_002.psarc`, `trico_003.psarc` | jogo base | 2,8 + 5,8 + 5,4 + 6,8 GB | conteúdo principal |
| `trico_bootp.psarc`, `trico_001p.psarc`, `trico_002p.psarc`, `trico_003p.psarc` | update 01.03 | ~1,6 GB | correções do update |
| `trico_enu.psarc` / `trico_ptb.psarc` / `trico_esl.psarc` / `trico_frc.psarc` (+ os `…p.psarc` do update) | base + update | ~280 MB cada | vozes e textos: inglês, português, espanhol, francês |
| `sce_discmap.plt`, `sce_discmap_patch.plt` | base, update | pequenos | mapa de arquivos (recomendado) |

O áudio usa a versão V5 de `libSceNgs2` incluída no pacote; nenhum módulo de áudio do firmware do console é necessário.

**Como montar a pasta:**
1. No Itemzflow, faça o dump do jogo (`CUSA03627`) e do update (`CUSA03627-patch`) para um pendrive exFAT.
2. No PC, copie a pasta do jogo base e depois copie o conteúdo do update **por cima**, substituindo os arquivos (`eboot.bin`, `sce_module/`, `sce_sys/`).
3. Confira os tamanhos. Arquivos acima de 4 GB vindos pelo FTP do GoldHEN 2.2 chegam cortados, então use o pendrive.

**PC:** Windows 10 (1903+)/11 64 bits ou Linux x86-64 (glibc 2.39+), placa de vídeo com Vulkan 1.3 (6 GB de VRAM recomendados).

### Como jogar

1. Extraia o pacote numa pasta qualquer (no Windows, fora do .zip).
2. Abra **The Last Guardian** (`The Last Guardian.exe` no Windows).
   - No Windows, se aparecer "O Windows protegeu o computador", clique em *Mais informações* e depois em *Executar assim mesmo*. O programa não é assinado digitalmente.
3. Em **Pasta do jogo**, escolha a pasta que tem o `eboot.bin`.
4. Escolha o idioma do jogo (inglês, português, espanhol ou francês) e clique em **Jogar**. A primeira partida demora um pouco mais.

No Linux, `./install_desktop.sh` adiciona o jogo ao menu de aplicativos.

### Durante o jogo

- **F10**: liga e desliga o contador de FPS
- **Insert** (ou R3 + L2 no controle): abre o menu do port (imagem, desempenho, overlay)
- Controles de PS4/PS5, Xbox e 8BitDo funcionam via SDL3, por USB ou Bluetooth.

### Teclado e mouse

Com a janela do jogo em foco, o mouse fica preso e gira a câmera. **Esc** solta o mouse; um clique na janela prende de novo. Abrir o menu do port (Insert) também solta o mouse.

| Ação no jogo | Botão do PS4 | Teclado e mouse |
|---|---|---|
| Agachar | X | Ctrl esquerdo |
| Pular | Triângulo | Espaço |
| Interagir | Círculo | E ou clique esquerdo |
| Esquivar / arremessar | Quadrado | Shift esquerdo |
| Encontrar Trico | L1 | Q |
| Chamar Trico / comando | R1 | F ou clique direito |
| Dica | L3 | Tab |
| Inventário | Direcional | Setas; roda para baixo/cima = esquerda/direita |
| Andar | Analógico esquerdo | W A S D |
| Câmera | Analógico direito | Mouse ou I J K L |
| Pausa | Options / Menu | Enter |

- No launcher, a caixa **Controles** liga o mouse, muda a sensibilidade e inverte o eixo Y. **Teclas e botões do mouse…** remapeia cada botão: clique em *Definir* e aperte a tecla ou o botão do mouse (os botões laterais e a roda também servem). Vale na próxima vez que o jogo abrir.
- Para deslizar o touchpad sem controle com touchpad, segure Backspace e use I J K L.
- L2, R2, R3 e cliques no touchpad não têm teclas padrão; continuam disponíveis para remapeamento.
- Para deslizar o touchpad sem controle com touchpad, segure Backspace e use I J K L.
- No jogo, a aba **Controles** do menu do port (Insert) ajusta sensibilidade, eixo Y, suavização, aceleração e anti-zona morta na hora.

### Onde ficam as coisas

- Saves: `user/savedata/`, ao lado do executável (guarde essa pasta ao atualizar)
- Logs: `out/logs/` (`out/last_run.log` é o da última sessão; mande ele junto se reportar um problema)
- Configurações do launcher: `~/.config/tlg-launcher/` (Linux) ou `%APPDATA%\tlg-launcher` (Windows)

### Problemas

- **Sem áudio**: confira `out/last_run.log` para mensagens de inicialização ou imports do NGS2. O port usa a libSceNgs2 V5 incluída; arquivos SPRX externos não são carregados.
- **Não abre ou fecha sozinho**: veja a caixa de log no launcher e mande o `out/last_run.log` no [Discord](https://discord.gg/UMkdMYZh9g) ou numa [issue](https://github.com/HayatoG/tlg_pc/issues).
- **NVIDIA**: use um driver recente.

---

## English

### Required game files

Everything comes from **your own** PS4 (GoldHEN + Itemzflow). None of it is in the download. Use **The Last Guardian CUSA03627 (US) with update 01.03**, merged into one folder, with the base game and the update on top. About 25 GB.

| File | From | Size | Purpose |
|---|---|---|---|
| `eboot.bin` | update 01.03 | 89 MB | the game code (must be the 1.03 one) |
| `sce_module/libc.prx`, `sce_module/libSceFios2.prx` | update 01.03 | 1.5 MB | the game's own libraries |
| `sce_sys/` (`param.sfo`, `playgo-chunk.dat`, `icon0.png`, `pic0.png`, `pic1.png`) | base + update | ~5 MB | metadata, install info and the launcher art |
| `trico_boot.psarc`, `trico_001.psarc`, `trico_002.psarc`, `trico_003.psarc` | base game | 2.8 + 5.8 + 5.4 + 6.8 GB | main content |
| `trico_bootp.psarc`, `trico_001p.psarc`, `trico_002p.psarc`, `trico_003p.psarc` | update 01.03 | ~1.6 GB | update fixes |
| `trico_enu.psarc` / `trico_ptb.psarc` / `trico_esl.psarc` / `trico_frc.psarc` (+ the update's `…p.psarc`) | base + update | ~280 MB each | voices and text: English, Portuguese, Spanish, French |
| `sce_discmap.plt`, `sce_discmap_patch.plt` | base, update | small | file map (recommended) |

Audio uses the bundled libSceNgs2 V5 image; no audio module from the console firmware is required.

**How to build the folder:**
1. In Itemzflow, dump the game (`CUSA03627`) and the update (`CUSA03627-patch`) to an exFAT USB drive.
2. On the PC, copy the base game folder, then copy the update's contents **over it**, replacing files (`eboot.bin`, `sce_module/`, `sce_sys/`).
3. Check the sizes. Files over 4 GB sent through GoldHEN 2.2's FTP arrive truncated, so use the USB drive.

**PC:** 64-bit Windows 10 (1903+)/11 or x86-64 Linux (glibc 2.39+), a graphics card with Vulkan 1.3 (6 GB of VRAM recommended).

### How to play

1. Extract the package anywhere (on Windows, outside the .zip).
2. Open **The Last Guardian** (`The Last Guardian.exe` on Windows).
   - On Windows, if "Windows protected your PC" appears, click *More info*, then *Run anyway*. The program is not digitally signed.
3. Under **Game folder**, choose the folder that has `eboot.bin`.
4. Pick the game language (English, Portuguese, Spanish or French) and click **Play**. The first start takes a little longer.

On Linux, `./install_desktop.sh` adds the game to the applications menu.

### While playing

- **F10**: toggles the FPS counter
- **Insert** (or R3 + L2 on a controller): opens the port's menu (image, performance, overlay)
- PS4/PS5, Xbox and 8BitDo controllers work through SDL3, over USB or Bluetooth.

### Keyboard and mouse

While the game window has focus, it takes the mouse and the mouse turns the camera. **Esc** releases the mouse; a click in the window takes it back. Opening the port's menu (Insert) also releases it.

| In game | PS4 button | Keyboard and mouse |
|---|---|---|
| Crouch | Cross | Left Ctrl |
| Jump | Triangle | Space |
| Interact | Circle | E or left click |
| Dash / throw | Square | Left Shift |
| Find Trico | L1 | Q |
| Call Trico / command | R1 | F or right click |
| Hint | L3 | Tab |
| Inventory | D-pad | Arrow keys; wheel down/up = left/right |
| Move | Left stick | W A S D |
| Camera | Right stick | Mouse or I J K L |
| Pause | Options / Menu | Enter |

- In the launcher, the **Controls** box turns the mouse on or off, sets its sensitivity and inverts the Y axis. **Keyboard and mouse buttons…** remaps each button: click *Set* and press the key or mouse button (the side buttons and the wheel work too). It applies at the next start. Backspace + IJKL swipes the touchpad.
- L2, R2, R3 and touchpad clicks have no keyboard defaults; they remain available for remapping.
- In game, the **Controls** tab of the port's menu (Insert) changes sensitivity, Y axis, smoothing, acceleration and anti-dead zone at once.

### Where things are

- Saves: `user/savedata/` next to the executable (keep this folder when updating)
- Logs: `out/logs/` (`out/last_run.log` is the latest session; attach it to bug reports)
- Launcher settings: `~/.config/tlg-launcher/` (Linux) or `%APPDATA%\tlg-launcher` (Windows)

### Problems

- **No audio**: check `out/last_run.log` for NGS2 initialization or import errors. The port uses the bundled libSceNgs2 V5 image; external SPRX files are not loaded.
- **Does not start or closes by itself**: check the log box in the launcher and send `out/last_run.log` on [Discord](https://discord.gg/UMkdMYZh9g) or in an [issue](https://github.com/HayatoG/tlg_pc/issues).
- **NVIDIA**: use a recent driver.

---

This project is not affiliated with Sony Interactive Entertainment, SIE Japan Studio or genDESIGN. The Last Guardian is a trademark of Sony Interactive Entertainment. / Este projeto não tem vínculo com a Sony Interactive Entertainment, SIE Japan Studio ou genDESIGN.

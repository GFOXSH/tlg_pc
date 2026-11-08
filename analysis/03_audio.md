# 03 — Áudio: The Last Guardian (CUSA03627, eboot v1.03)

Estudo de viabilidade do port para PC. Endereços em espaço Ghidra (base 0x1000000).
Legenda: **[C]** = confirmado no binário/dados, **[D]** = deduzido com boa confiança (decompilação + headers públicos/shadPS4), **[H]** = hipótese/palpite.

> Atualização: este estudo e o plano HLE abaixo são históricos. O runtime atual integra a imagem libSceNgs2 V5 do bundle estático e não carrega SPRX do firmware.

---

## 0. Resumo

- O jogo usa o **SCE "sndx"** (middleware de som da Sony, build `20160930_trico_lto`, "Built at Oct 27 2016 22:19:23"), **linkado estaticamente** no eboot, por cima de **libSceNgs2** (mixer/sampler por software da Sony) e **libSceAudioOut**. [C]
- Fluxo: `sndx_out_thread` → `sceNgs2SystemRender` (float32, 8 canais, grain de 256 amostras, 48 kHz) → `sceAudioOutOutputs` na porta MAIN. [C]
- **O Audio3d não é usado na prática**: o caminho do Audio3d só é ligado com `loadAudio3d=1` na string de config do `sceSndxInit`, e a única config que o jogo passa não tem essa chave. [C] Também não há controller speaker nem headset: `numControllerSpeakers`/`numHeadsetSpeakers` ficam no default 0, então o `sceAudioOutSetMixLevelPadSpk` nunca roda. [C]
- Codecs nos dados: **VAG/HEVAG da Sony** (codec SXD 0x21, quase todo o SFX), **ATRAC9** (codec SXD 0x42: BGM, diálogo, ambiência multicanal), PCM (raro). O NGS2 decodifica tudo internamente. **Não existe import de libSceAjm.** [C]
- Banks: `game/sounds/**.bod` (descritor) + `*_sbuf.data` (container `CDAT`/`SXDF` com chunks TONE/REQD/NAME/WAVE/SEQD) + `*.scs` (streams `SXDS`), dentro de `trico_boot.psarc` (e um pouco em trico_001/002/003). [C]
- O gow3_pc **já tem** AudioOut sobre SDL3 e ATRAC9 (LibAtrac9, via Ajm). **Não tem** NGS2 nem Audio3d, e a cópia do shadPS4 dele também não tem (só as classes de log `Lib.Ngs2`/`Lib.Audio3d`). **O trabalho grande é o NGS2.**

---

## 1. Inicialização, threads e render loop

### 1.1 Cadeia de init

```
FUN_01f9df50  (SoundManager do jogo; mutex "_sound")                         [C]
 ├─ FUN_011e24a0(FUN_01f9e270, FUN_01f9e2c0)   = sceSndxSetMallocFreeFunc     [D]
 ├─ FUN_011e2540("maxResources = 256 maxInstances = 256 maxSequences = 512
 │               maxWaves = 256 maxAtrac9Decoders = 128 enableRestricted = 0
 │               maxVariables = 640 cpuEnableFlags = 0000010")   = sceSndxInit   [C]
 │     (string em 0x25dea90; ref única em 0x1f9dfec; único caller de 0x11e2540)
 │   └─ FUN_011e25b0  sndx_system.c: "=== sndx system initialize ... ==="        [C]
 │        ├─ parseia config, aloca memória do sistema
 │        ├─ isSkipSetOutputMode? -> FUN_010042d0(8,8)/FUN_011f2450(8,8) "Set output mode (%d)"
 │        ├─ isSkipStartAudioOut? -> FUN_011f1090  (start do serviço AudioOut)    [C]
 │        └─ FUN_011b44e0(cfg) -> inits dos subsistemas; o init do engine NGS2
 │             FUN_011a69c0 é chamado por ponteiro (sem caller direto)           [D]
 ├─ FUN_011dff40(&{5 callbacks FUN_01f9e310..0x1f9e400}) = sceSndxStreamSetHandlers
 │     (o jogo injeta callbacks de I/O de arquivo para streaming;
 │      cria "sndx_stream_schedule_thread" em FUN_011e0fb0)                     [C]
 ├─ FUN_011d69c0(0..3, 3)                       (volumes/categorias) [H]
 └─ FUN_011e0420(0, buf, 0x200000, 0x20, 0) = sceSndxStreamBufferRegister (2 MB) [C]
```

A config do jogo **não tem** `loadAudio3d`, `noLoadNgs2`, `numControllerSpeakers`, `numHeadsetSpeakers`, `enableAUX` nem `enableBGM`, então todas ficam no default. Repare que falta um espaço em `enableRestricted = 0maxVariables`. O valor do parser deve virar 0 [H, mas irrelevante: o default 1 só acrescentaria uma porta "restricted"].

### 1.2 Engine NGS2: `FUN_011a69c0` (sndx_engine_ngs2_orbis.c) [C]

1. `loadAudio3d` (default 0): se ≠0, roda `sceSysmoduleLoadModule(0xa7)` (Audio3d) e liga a flag `DAT_067f1c18 = 1`.
2. `noLoadNgs2` (default 0): roda `sceSysmoduleLoadModule(0xb)` (NGS2) e liga `DAT_067f1c19 = 1`.
3. Lê `numGrains`(256), `enableACP`(0), `numDecoderMemoryBlocks`, `maxAtrac9Decoders`(o jogo passa 128), `maxVoiceChannels`(256), `maxSubmixers`(0), `numControllerSpeakers`/`numHeadsetSpeakers`(0), `enableRestricted`, `enableAUX`.
4. Monta o state global em `DAT_067f1c20` (0x4d00 bytes) com o mutex `"sce_sndx_engine_update"` em +0x10, e pools de voz: AT9 decoders (+0x20) e channels (+0x38).
5. Se tiver Audio3d: `sceAudio3dInitialize(0)`, depois `sceAudio3dGetDefaultOpenParameters`, depois `sceAudio3dPortOpen(0xff /*system user*/, {size 0x20, granularity=numGrains, ?=0, maxObjects=0x200, queueDepth=3, bufferMode=2}, &DAT_02704bd8)`. Em seguida `sceAudio3dPortSetAttribute(port, 0x10001, &0.0f, 4)` (provavelmente LATE_REVERB_LEVEL=0 [D]), cria o event flag `"sndx_audio3d_notify"` e a thread `"sndx_audio3d_out_thread"` (`FUN_011a8340`). **Esse caminho é morto no TLG.**
6. `sceNgs2SystemCreateWithAllocator({size 0x40, …, maxGrainSamples=0x100, numGrainSamples… sampleRate=0xBB80}, allocator=&PTR_FUN_02704be0, &sys)`.
7. Racks criados com `sceNgs2RackCreateWithAllocator` (IDs de rack são os do SDK NGS2) [C nos IDs, D nos nomes]:

| var | Rack ID | Tipo | Observação |
|---|---|---|---|
| local_5e0 | **0x3000** | MASTERING | maxVoices = ctrlSpk + (restricted+1) + headset + aux. No TLG vale **1** (só a MAIN) |
| local_5e8 | **0x2000** | SUBMIXER | "premix"/master bus, 1 voz, com *patch* para o mastering |
| local_5f0 | **0x1000** | SAMPLER | maxVoices 0x100 (ou maxAtrac9Decoders se `enableACP`) |
| local_5f8 | 0x2000 | SUBMIXER | só se `maxSubmixers`≠0. No TLG não é criado |

   Depois vem a configuração com `sceNgs2RackGetVoiceHandle` + `sceNgs2VoiceControl`. Parâmetros vistos: id `0x30000000` (setup do mastering, 8 canais), `0x30000005` (seleção de saída por índice de porta), `5` (PATCH submixer→mastering), `6` (KICK_EVENT 0=PLAY).

### 1.3 Buss FX: `FUN_011ac510` (= sceSndxBussFxSetup, sndx_buss_fx.c) [C]

Aqui é `sceNgs2RackQueryBufferSize` + `sceNgs2RackCreate`, com o buffer vindo do heap do sndx, um rack por efeito:

| Nome do FX (strings @0x24db961…) | Rack ID |
|---|---|
| THRU, EQ, COMP, DISTO, UMIX, AMP, PANNER | **0x2000** SUBMIXER (com módulos built-in: EQ/compressor/distortion/upmix) |
| REVERB | **0x2001** REVERB (parâmetros estilo I3DL2) |
| CHORUS / DELAY / PITCH | **0x4002** CUSTOM SUBMIXER, template de 0x4d8 bytes em `DAT_024dcac8`, custom module id 0x1c / 0x1d / 0x20 [D] |

**Efeitos que o TLG usa de fato** (strings em `game/sounds/trico_buss_sbuf.data`) [C]: THRU (8×), REVERB (6×, ex. `RoomReverb`, `SlowReverb`, params `room;reflections;reverbDelay;decayTime;decayHFRatio;HFReference;diffusion;density`), COMP (6×), UMIX (5×, `sur-input=REAR;auto-upmix=1;…`), DELAY (3×, `RoomDelay`, `SlowDelay`), CHORUS (2×), EQ de 8 bandas (LS/PK/HS), filtros `WaterFilterSFX/Amb`, `panner_distance/angle`.

### 1.4 Threads e render

| Thread | Entry | Função |
|---|---|---|
| `sndx_out_thread` | `FUN_011f28a0` → handler `FUN_011f2190` | **render loop principal** [C] |
| `sndx_audio3d_out_thread` | `FUN_011a8340` | `WaitEventFlag` → `sceAudio3dPortPush(port, 1)` (morto no TLG) [C] |
| `sndx_stream_schedule_thread` | `FUN_011e0fb0` | streaming de .scs [C] |
| `sndx_loader_thread`, `sndx_capture_thread`, `sceSndx-async-thread` | (strings) | carregamento, captura, async [D] |

Loop do `FUN_011f2190` [C]:
```
while(!quit){
  sceAudioOutGetPortState(main, &st);  DAT_06805fe1 = st.channel   // nº de canais da saída real
  sceAudioOutGetPortState(bgm,  &st2); DAT_06805fe5 = (st2.output!=0)
  aponta buffers duplos DAT_06806200 + (i*0x4000) + (flip*0x2000)
  FUN_011b04f0(&DAT_06806080, nPorts)      // render
  sceAudioOutOutputs(params, n)            // bloqueante = clock do áudio
  flip ^= 1
}
```
`FUN_011b04f0` (render, engine) [C]:
- Mapeia o formato da porta para o tipo de buffer NGS2: 4→0x12, 5→0x13 (PCM_I16L/B); 0x10→0x18, 0x11→0x19 (PCM_F32L/B). Todas as portas do TLG usam 0x10, ou seja **float32 LE**.
- Faz callbacks antes e depois do render (`+0x4ce8`, `+0x4cc8`, `+0x4cd8`, `+0x4cb8`, `+0x4cf0`, `DAT_067f1c88`). São hooks de update/capture.
- `scePthreadMutexLock(engine+0x10)`, depois `sceNgs2SystemRender(sys, bufInfo[], n)`, depois unlock.
- Se tiver Audio3d: `sceAudio3dPortGetQueueLevel`, depois `sceAudio3dPortAdvance` + `SetEventFlag` (acorda a thread de push).
- Mede o tempo com TSC (perf meter).

### 1.5 Sampler / vozes [C]

- `FUN_011a88b0`: aloca a voz. O tipo de waveform SXD **0x42 aloca decoder AT9** (`VOICE_ALLOC_AT9`) e channels (`VOICE_ALLOC_CHANNEL`). Para voz 3D (`param[0]==1`) roda `sceAudio3dObjectReserve`; se não tiver Audio3d, imprime `"Failed to initialize Audio3d. Please sceSndxInit("loadAudio3d=1")"`.
- `FUN_011a9690`: set de parâmetros da voz. Mapeia o tipo SXD para o tipo NGS2 (jump tables em `0x24daefc`/`0x24daf84`):
  - SXD 0x00–0x14 → NGS2 PCM 0x10–0x1a
  - **SXD 0x20 e 0x21 → NGS2 0x1c (VAG)**
  - SXD 0x28 → 0x87; 0x30–0x35 → 0x80–0x86 [tipos NGS2 >0x80 desconhecidos, H: variantes de HEVAG/ADPCM]
  - **SXD 0x42 → NGS2 0x40 (ATRAC9)**, com configData vindo do header da wave.
  - Parâmetros do sampler: `0x10000000` (waveform format: type/channels/rate/config, size 0x28), `0x10000001` (waveform blocks/segments, `VOICE_SEGMENT`), `0x10000003`, `0x10000005` (pitch), `0x10000008` (só com Audio3d); ADSR (`VOICE_ADSR`), filter (`VOICE_FILTER`), volume/matrix (`VOICE_VOLUME`).
- `FUN_011a8e90`: eventos via param 6 (KICK_EVENT): PLAY=0, RELEASE=1 (stop com release), STOP=2 (imediato), KILL=3 (`FUN_011a8160`), PAUSE=4, RESUME=5. Id 9 com flag 0x11 = peak meter [D].
- `FUN_011a85b0`: `sceNgs2VoiceGetState(h, &st, 0x30 / 0x10)` para detectar `VOICE_FINISHED`.
- `FUN_011ab580`: `sceAudio3dObjectSetAttributes(port, obj, 5, attrs)` com POSITION(3, 12 bytes), SPREAD(5), GAIN(4), PRIORITY(2) e PCM(1, 0x18 bytes) [D nos IDs]. Morto no TLG.

Callers de `sceNgs2VoiceControl`: 0x11a69c0, 0x11a8160, 0x11a82b0, 0x11a8e90, 0x11a9690, 0x11ab920, 0x11abb70, 0x11abd70, 0x11abef0, 0x11ac510, 0x11adc10, 0x11aefd0, 0x11b0300, 0x11b08e0, 0x11b11e0.

Superfície NGS2 usada: só 11 funções (`SystemCreateWithAllocator`, `SystemDestroy`, `SystemRender`, `RackCreate`, `RackCreateWithAllocator`, `RackDestroy`, `RackGetVoiceHandle`, `RackQueryBufferSize`, `VoiceControl`, `VoiceGetState`). **Toda a complexidade fica nos blocos de parâmetros do `VoiceControl`.**

---

## 2. AudioOut: `sndx_audioout_orbis.c`

- `FUN_011f1090`: start do serviço [C]. Lê `numMainChannels`(8), `numGrainSamples`(256, múltiplo de 64), prioridade/stack/afinidade da thread `"audioOut"`, e as chaves de pad/headset/restricted/AUX/BGM. Monta a tabela de portas `DAT_06806080` (stride 0x20: id, fmt=0x10, ch, grain, 48000, …, buf). Chama `sceAudioOutInit()` e abre as portas via `FUN_011f1fd0`.
- `FUN_011f1fd0` = open por "output ID" do sndx [C]:

| ID sndx | Tipo AudioOut | Canais | Uso |
|---|---|---|---|
| 0 | 0 MAIN | 8 | **porta principal (TLG)** |
| 0x78 | 0 MAIN + attr 0x10000 (RESTRICTED [D]) | 8 | só com enableRestricted |
| 0x79 | 0x7f AUX | 8 | só com enableAUX |
| 0x7f | 1 BGM | 8 | **aberta no TLG** (enableBGM default 1); só se consulta `GetPortState` nela, nunca se dá output [C] |
| 100–103 | 4 PADSPK | 1 | controller speaker, seguido de `sceAudioOutSetMixLevelPadSpk(h, 0.3548*32768 ≈ 11626)` |
| 0x6e–0x71 | 2 VOICE (headset) | 2 | |

  Formato: `3/4/5` = FLOAT mono/stereo/8ch. Na prática: **MAIN e BGM, FLOAT_8CH, 256 frames, 48 kHz**.
- `FUN_011f1cb0` / `FUN_011f2690`: shutdown e close de porta (`sceAudioOutOutput(h, NULL)` para drenar, depois `sceAudioOutClose`).

## 3. Audio3d vs AudioOut

- O Audio3d é **opcional** no sndx, ligado por `loadAudio3d=1`. Ele serviria para vozes "objeto" (posição/spread/gain por voz, com saída via `Audio3dPortPush` em vez do mastering). [C]
- O TLG **não liga** o Audio3d (config em 0x25dea90, único caller do sceSndxInit). Os imports existem só porque o sndx é linkado estático. Todo o 3D do jogo é **panning/matrix 7.1 feito pelo próprio sndx dentro do NGS2** (pan/filter/send por voz e o FX `PANNER`). [C, com leve ressalva: o default 0 do parser foi lido na decompilação, mas não testei em runtime]
- No port: **stub** do Audio3d (`sceAudio3dInitialize` retornando erro ou nunca chamado). Não precisa de downmix especial.

## 4. Controller speaker

- O suporte existe (portas 100–103, `SetMixLevelPadSpk` em 0x11f1fd0, o único caller), mas **não é usado**: `numControllerSpeakers` não aparece na config, o default é 0 e nenhuma porta PADSPK é aberta. Nenhuma outra função do eboot chama AudioOut. [C]
- Port: stub de `sceAudioOutSetMixLevelPadSpk` retornando 0.

## 5. Formatos e dados de som

Arquivos em `trico_boot.psarc` (lidos do TOC; mais 2/13/5 .bod em trico_001/002/003) [C]:

| Path | Qtde | Tam. |
|---|---|---|
| `game/sounds/**/*.bod` | 306 (boot) + 20 | ~100 KB. Descritor (strings `SoundContext`, `SoundBuffer`, path do sbuf, `premixing`, `mastering`, `isActive`) |
| `game/sounds/**/*_sbuf.data` | 306 + 20 | ~618 MB (boot) + ~288 MB |
| `game/sounds/**/*.scs` | 293 | ~510 MB. Streams `SXDS` (dialog 270, bgm/stream 11, se/stream 12) |
| `game/sounds/files.csv`, `condor_setup.lua`, scripts `game/scripts/common/sound*/…lua` | | lógica em Lua |

Path no eboot: `"game/sounds/%s"`. Dialog fica em `game/sounds/dialog/sNN_xx_<char>_v_NNNN` e não é localizado (o menino fala uma língua inventada).

- `*_sbuf.data` = `CDAT` → `SXDF` (versão 4, nome do bank, GUID) com chunks **TONE, REQD, NAME, WAVE, SEQD** [C]. A tabela WAVE tem entradas de 0x40+ bytes, com codec no byte +4, canais no +5 e sample rate no +8.
- Contagem de codecs (amostra de 5 banks) [C]:
  - `boy_se`: 1423× 0x21 mono, 2× 0x42
  - `cdr_se` (Trico): 559× 0x21 mono, 135× 0x21 stereo, 73× 0x21 @96 kHz, 7× 0x42
  - `amb_se_cmn`: 1333× 0x21, 120× 0x42 (2/4 canais), 1× 0x20
  - `bgm_st01_op`: 4× 0x42 com **6 canais**
  - dialog: 0x42 mono
- **0x21 = HEVAG/VAG da Sony** (4-bit ADPCM; nomenclatura do vgmstream para SXD) [D]. **0x42 = ATRAC9** [C pelo mapeamento → NGS2 0x40 e pela string `"Invalid ATRAC9 configure data (%08X)"`]. 0x20 é PCM ou PS-ADPCM [H].
- O vgmstream tem parser de SXD (SXDF/SXDS), útil para validar o decoder offline [D].

---

## 6. Comparação com o gow3_pc

| Componente | gow3_pc `src/runtime_*` | cópia do shadPS4 (`gpu/shadps4/core/libraries`) | TLG precisa |
|---|---|---|---|
| libSceAudioOut | **Sim**: `runtime_audio.c` (SDL3, 25 portas, tipos 0/1/2/3/4/126/127, S16/F32 1/2/8 ch, downmix 7.1→2.0, pacing por clock, `Outputs`, `GetPortState`, `SetVolume`) | não | Init/Open/Output/Outputs/GetPortState/Close + **SetMixLevelPadSpk (falta, mas é stub trivial)** |
| libSceAjm (ATRAC9/MP3) | **Sim**: `runtime_ajm.c` + LibAtrac9 (submódulo `third_party/LibAtrac9`, não baixado no clone) | não | não importado diretamente, mas o LibAtrac9 é reaproveitável |
| libSceNgs2 | **não** | **não** (só a classe de log `Lib.Ngs2`) | **sim, é o núcleo** |
| libSceAudio3d | não | não (só log) | stub |

Detalhes do AudioOut do gow3 que importam no TLG: o `GetPortState` devolve `channel=min(ch,2)` (o sndx lê esse byte em `DAT_06805fe1`, talvez para escolher o downmix). A porta BGM aberta e nunca alimentada vira um stream SDL silencioso, o que não atrapalha. Também tem que aceitar `sceAudioOutOutput(h, NULL)` no close.

---

## 7. Plano para o port de áudio

### Opção A: HLE do libSceNgs2 em software (recomendada se o runtime não tiver loader de .sprx)

1. **AudioOut**: reaproveitar `runtime_audio.c` do gow3_pc com ajustes mínimos (stub do `SetMixLevelPadSpk`, `Output(NULL)` e a semântica do `GetPortState`). Para 7.1 nativo, usar SDL3 (já está lá) ou WASAPI. **0,5–1 dia.**
2. **Audio3d**: stubs que retornam `SCE_AUDIO3D_ERROR_NOT_READY` (ou 0); a config nunca chama. **0,5 dia.**
3. **NGS2 (núcleo)**:
   - Objetos System/Rack/Voice, handles e alocador (o jogo passa allocator, então honrar callbacks `alloc/free` em `PTR_FUN_02704be0`). `QueryBufferSize`/`RackCreate` com buffer fornecido.
   - Grafo: sampler → (patch) → submixer(s)/FX → submixer premix → mastering → buffer de saída float 8ch/256 frames. Renderização por grain.
   - Sampler: waveform format (0x10000000), waveform blocks com loop/segmentos (0x10000001), pitch/resample (0x10000005), ADSR, biquad filter, matrix/port volume, send, eventos PLAY/STOP/STOP_IMM/KILL/PAUSE/RESUME, `VoiceGetState` (flags playing/finished, posição).
   - Decoders: PCM (trivial), **VAG/HEVAG** (~200–300 LOC, com referência pública no vgmstream), **ATRAC9** com LibAtrac9 (superframes, configData de 4 bytes, até 6 canais).
   - Mastering: setup 8ch, roteamento por índice de porta (0x30000005), limiter opcional.
   - FX: reverb I3DL2 (rack 0x2001, o maior trabalho de fidelidade), EQ 8 bandas e compressor (submixer), upmix (UMIX), chorus/delay (custom 0x4002). Pitch e distortion não aparecem nos dados do TLG.
   - Para descobrir os layouts exatos dos param blocks: instrumentar `VoiceControl` (dump dos blocos) e usar os headers públicos do NGS2/shadPS4 `ngs2.h`.
   - Estimativa: **MVP audível** (sampler + mastering + submixer passthrough, PCM/VAG/AT9, sem FX) em **2–3 semanas**. **Paridade boa** (reverb, EQ, comp, upmix, chorus/delay, pitch exato, loops) em **+3–5 semanas**. Total **5–8 semanas/pessoa**.

### Opção B: LLE do `libSceNgs2.sprx` original [H]

O NGS2 é código x86-64 da própria Sony. Se o runtime do port já tiver (ou ganhar) um loader de PRX/SPRX com resolução de NIDs, dá para carregar o `libSceNgs2.sprx` descriptografado do firmware do console do próprio usuário e implementar só as dependências dele (libkernel/libc e, provavelmente, **libSceAjm para ATRAC9**, que o gow3_pc já tem). O resultado seria fidelidade bit-exata, inclusive do reverb. Estimativa **1–2 semanas** se o loader existir, **3–4** se tiver que escrever. Riscos: dependências internas desconhecidas, como uso de ACP/Ajm quando `enableACP=0` (sem verificar), e a distribuição exige que o usuário forneça o firmware.

### Opção C (não recomendada): substituir o sndx inteiro

O sndx é estático e tem players de sequência, morph, xfader, variáveis e streaming. Reescrever isso é muito mais caro que emular o NGS2.

### Ordem sugerida
1. AudioOut (gow3) + stubs de Audio3d e PadSpk, com o jogo rodando mudo mas sincronizado (o `Outputs` bloqueante é o clock do áudio).
2. NGS2 com sampler PCM/VAG e mastering, para ter SFX audível.
3. ATRAC9 (BGM e diálogo) via LibAtrac9.
4. FX: reverb, depois EQ/comp, upmix e chorus/delay.
5. Validação: decodificar os banks offline (vgmstream ou ferramenta própria lendo SXDF/SXDS) e comparar.

---

## 8. Tabela de endereços

| Endereço | O quê | Status |
|---|---|---|
| 0x1f9df50 | init do sound manager do jogo (chama sceSndxInit) | C |
| 0x25dea90 | string de config do sceSndxInit | C |
| 0x11e2540 / 0x11e25b0 | sceSndxInit / corpo | C |
| 0x11a69c0 | init do engine NGS2 (Audio3d opcional, System/Racks) | C |
| 0x11ac510 | sceSndxBussFxSetup (racks de FX) | C |
| 0x11b04f0 | render (sceNgs2SystemRender + Audio3d advance) | C |
| 0x11b08e0 | update do engine chamado no render (usa VoiceControl) | D |
| 0x11f1090 | start do AudioOut (portas, cria sndx_out_thread) | C |
| 0x11f1fd0 | open de porta por ID sndx (+SetMixLevelPadSpk) | C |
| 0x11f2190 | corpo da sndx_out_thread (render → AudioOutOutputs) | C |
| 0x11f28a0 | entry da sndx_out_thread | C |
| 0x11f1cb0 / 0x11f2690 | shutdown / close de porta | C |
| 0x11a8340 | sndx_audio3d_out_thread (PortPush) | C |
| 0x11a88b0 | alocação de voz (AT9/channel, Audio3d ObjectReserve) | C |
| 0x11a9690 | parâmetros de voz (mapeamento waveform SXD→NGS2) | C |
| 0x11a8e90 | eventos de voz (play/pause/resume/release/stop/peak) | C |
| 0x11a8160 | free de voz (KILL, ObjectUnreserve) | C |
| 0x11a85b0 | polling de estado de voz (VoiceGetState) | C |
| 0x11ab580 | Audio3d ObjectSetAttributes | C |
| 0x11dff40 / 0x11e0420 | sceSndxStreamSetHandlers / StreamBufferRegister | C |
| 0x24be898…0x24be928 | PLT NGS2 / AudioOut (0x24be828…0x24be888) | C |
| 0x24bf568…0x24bf618 | PLT Audio3d | C |
| DAT_067f1c20 | state global do engine | C |
| DAT_067f1c18 | flag "Audio3d carregado" | C |
| DAT_02704bd8 | handle da porta Audio3d | C |
| DAT_06806080 | tabela de portas AudioOut do sndx | C |

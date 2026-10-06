# Cyber Music HUD

HUD minimalista que aparece no canto inferior direito quando uma música nova
começa no **Spotify** ou no **YouTube** (navegador), mostra título, artista e
plataforma com um visualizador de espectro que reage ao **áudio real**, e some
com fade depois de ~5,6 s.

```
┌──────────────────────────────────────┐
│ ▶  Tank!                     SPOTIFY │
│    Seatbelts                         │
│ ▆▇▇▅▃▄▆▂▁▃▅▁▁▁▁▁▁▂▂▂▂▂▃▃▃▃           │
└──────────────────────────────────────┘
```

## Como funciona

| Parte | Tecnologia | Por quê |
|---|---|---|
| Detectar música | **GSMTC** do Windows (`GlobalSystemMediaTransportControlsSessionManager`, o mesmo painel de mídia do volume) via C++/WinRT | Spotify e Chrome/Edge/Firefox publicam título/artista ali. Local, sem API, sem login, sem token |
| Plataforma | AUMID da sessão (`Spotify.exe`, `Chrome`…) + título das janelas do navegador | Navegador não expõe a URL; `"<vídeo> - YouTube - Google Chrome"` é o sinal local confiável. Se não der para confirmar YouTube, mostra o navegador (`CHROME`) |
| Áudio | **WASAPI loopback por processo** (Win10 2004+), na árvore de processos do player | Barras reagem só à música — notificação e outros apps não mexem nelas. Cai para loopback do sistema se não der |
| Espectro | FFT 2048 + 36 bandas log (45 Hz–15 kHz), dB com referência adaptativa | Barras usam a altura toda com volume alto ou baixo; silêncio = chão |
| Capa | Miniatura do GSMTC decodificada com WIC, quadrado central, 96 px, tingida de ciano com scanlines | Mantém o visual monocromático; miniatura 16:9 do YouTube vira quadrado |
| Visual | Direct2D/DirectWrite em janela em camadas, `WS_EX_NOACTIVATE \| WS_EX_TRANSPARENT` | Não rouba foco nem clique; translúcido de verdade |

Regras de exibição:
- anuncia uma faixa só depois de vê-la em 2 consultas seguidas (400 ms) — o
  Spotify troca título e artista em dois passos;
- pausar e retomar a mesma música **não** mostra de novo;
- sem artista, só anuncia se for YouTube confirmado (evita anúncio do Spotify e
  vídeo solto de outros sites);
- não aparece por cima de jogo/vídeo em tela cheia ou apresentação.

Consumo medido (i5-13420H, com outros apps rodando): **0 % de CPU** ocioso (só uma consulta GSMTC a
cada 400 ms, sem timer de desenho nem captura), ~3–10 % de um núcleo (varia com a carga da máquina) enquanto o
HUD está na tela (~32 fps), ~42 MB de memória privada.

## Uso

```
build.bat              compila build\CyberMusicHUD.exe
build\CyberMusicHUD.exe
```

Ícone na bandeja: **Mostrar agora**, **Pausar**, **Iniciar com o Windows**, **Sair**.

Linha de comando: `--show` (= Mostrar agora), `--exit`, `--enable-autostart`,
`--disable-autostart`, `--probe` (diagnóstico, abaixo).

## Testes e diagnóstico

```
build.bat test     testes de unidade de src\logic.h (classificação, detector, fade, FFT/espectro)
build.bat e2e      sobe o app e confere: canto certo, não rouba foco, click-through, some sozinho
build.bat probe    lista as sessões de mídia e captura 2 s de áudio, imprimindo o espectro
build.bat fake     compila build\fake\Spotify.exe: player de teste (MediaPlayer do WinRT)
                   com 2 faixas sintetizadas — exercita GSMTC + loopback por processo
tools\snapshot.ps1 salva PNGs do HUD em build\ para revisar o visual
```

O `fake` se chama `Spotify.exe` de propósito (é assim que o HUD reconhece
o Spotify) e fica isolado em `build\fake\`.

## Limitações conhecidas

- **YouTube em aba de fundo**: o título da janela é o da aba ativa, então a
  plataforma aparece como o navegador (`CHROME`) em vez de `YOUTUBE`. A faixa
  é anunciada mesmo assim, porque o YouTube informa o canal como artista.
- O Chrome só cria a sessão de mídia quando o áudio de fato toca.
- Loopback por processo exige Windows 10 2004 ou mais novo; antes disso, o
  visualizador ouve a saída inteira do sistema.

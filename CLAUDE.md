# Cyber Music HUD — notas para o Claude

C++20/Win32 puro (sem .NET: a máquina não tem SDK), compilado por `build.bat`
com MSVC do Build Tools (achado via vswhere). `/W4 /WX`: aviso é erro — é o
"lint" do projeto. qa-gate instalado (`.claude/`), `test_cmd = build.bat test`.

## Mapa

- `src/logic.h` — tudo que é puro e testável (classificação de fonte,
  `ChangeDetector`, `Timeline`, `DecodeText`, FFT/`SpectrumAnalyzer`). Lógica
  nova vai aqui, com teste em `tests/unit_tests.cpp`.
- `src/media.*` — thread MTA que consulta o GSMTC a cada 400 ms. Polling, não
  eventos, de propósito: o Spotify dispara evento antes de atualizar metadados.
- `src/audio.*` — loopback WASAPI por processo (fallback: sistema). Só roda
  enquanto o HUD está visível.
- `src/hud.*` — janela em camadas, Direct2D em DC render target.
- `src/procutil.*` — raiz da árvore de processos e títulos de janela.

## Regras

- Dados reais sempre: nada de barras aleatórias ou metadado inventado. Sem
  confirmação de YouTube, a etiqueta é o nome do navegador.
- Orçamento: 0 % CPU ocioso; ~3 % de 1 núcleo visível. `kFrameMs = 33`
  porque 16 ms custava ~12,7 % (medido); o render target `SOFTWARE` foi pior
  (~26 %). Re-medir ao mexer no desenho.
- Teste real sem Spotify/YouTube: `build.bat fake` e rode
  `build\fake\Spotify.exe` com o HUD aberto. A automação do Chrome não
  consegue tocar YouTube (aba oculta, `play()` trava).
- `tests/*.ps1` e `tools/*.ps1` com acento precisam de UTF-8 **com BOM**.
- Para editar arquivos com `\` (C++, .bat), use Write/Edit — o Bash tool
  corrompe barras invertidas em heredoc/python inline.

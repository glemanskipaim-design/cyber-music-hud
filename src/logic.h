// Lógica pura do HUD, sem Win32/WinRT: classificação da fonte de mídia,
// detecção de "música nova", linha do tempo do fade e análise de espectro.
// Tudo aqui é coberto por tests/unit_tests.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cwctype>
#include <initializer_list>
#include <numbers>
#include <string>
#include <vector>

namespace cmh {

inline std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

inline bool Contains(const std::wstring& haystack, const std::wstring& needle) {
  return haystack.find(needle) != std::wstring::npos;
}

// ------------------------------------------------------------ fonte de mídia

enum class Platform { Spotify, YouTube, YouTubeMusic, Browser };

struct Source {
  bool relevant = false;  // false: app de mídia que o HUD ignora (players locais etc.)
  bool browser = false;
  Platform platform = Platform::Browser;
  std::wstring exe;       // executável cujo áudio é capturado (minúsculo)
  std::wstring label;     // nome curto do navegador, usado quando a plataforma não é confirmada
};

// Fixo no Firefox: a AUMID dele é um hash do caminho de instalação padrão.
inline constexpr wchar_t kFirefoxAumid[] = L"308046b0af4a39cb";
// ID da extensão/PWA do YouTube Music no Chrome/Edge.
inline constexpr wchar_t kYouTubeMusicPwa[] = L"cinhimbnkkaeohfgghhklpknlkffjgod";

// aumid: SourceAppUserModelId da sessão do GSMTC ("Spotify.exe", "Chrome",
// "MSEdge", "SpotifyAB.SpotifyMusic_...!Spotify", "Chrome._crx_<id>", ...).
inline Source ClassifySource(const std::wstring& aumid) {
  const std::wstring id = Lower(aumid);
  Source s;
  if (Contains(id, L"spotify")) {
    s.relevant = true;
    s.platform = Platform::Spotify;
    s.exe = L"spotify.exe";
    s.label = L"SPOTIFY";
    return s;
  }
  struct Browser {
    const wchar_t* key;
    const wchar_t* exe;
    const wchar_t* label;
  };
  static const Browser kBrowsers[] = {
      {L"msedge", L"msedge.exe", L"EDGE"},     {L"microsoftedge", L"msedge.exe", L"EDGE"},
      {L"chrome", L"chrome.exe", L"CHROME"},   {L"firefox", L"firefox.exe", L"FIREFOX"},
      {kFirefoxAumid, L"firefox.exe", L"FIREFOX"}, {L"brave", L"brave.exe", L"BRAVE"},
      {L"opera", L"opera.exe", L"OPERA"},      {L"vivaldi", L"vivaldi.exe", L"VIVALDI"},
  };
  for (const Browser& b : kBrowsers) {
    if (Contains(id, b.key)) {
      s.relevant = true;
      s.browser = true;
      s.exe = b.exe;
      s.label = b.label;
      s.platform = Contains(id, kYouTubeMusicPwa) ? Platform::YouTubeMusic : Platform::Browser;
      return s;
    }
  }
  return s;
}

// Navegadores não informam a URL da mídia. O melhor sinal local é o título
// das janelas ("<vídeo> - YouTube - Google Chrome"), que só reflete a aba
// ativa de cada janela. Preferimos a janela que contém o título da faixa;
// sem ela, qualquer janela do YouTube serve; sem nada, fica Browser.
inline Platform PlatformFromWindowTitles(const std::vector<std::wstring>& titles,
                                         const std::wstring& trackTitle) {
  auto classify = [](const std::wstring& lower) {
    if (Contains(lower, L"youtube music")) return Platform::YouTubeMusic;
    if (Contains(lower, L"youtube")) return Platform::YouTube;
    return Platform::Browser;
  };
  const std::wstring track = Lower(trackTitle);
  if (!track.empty()) {
    for (const std::wstring& t : titles) {
      const std::wstring lower = Lower(t);
      if (Contains(lower, track)) return classify(lower);
    }
  }
  Platform best = Platform::Browser;
  for (const std::wstring& t : titles) {
    const Platform p = classify(Lower(t));
    if (p == Platform::YouTubeMusic) return p;
    if (p == Platform::YouTube) best = p;
  }
  return best;
}

inline std::wstring PlatformLabel(Platform p, const std::wstring& browserLabel) {
  switch (p) {
    case Platform::Spotify: return L"SPOTIFY";
    case Platform::YouTube: return L"YOUTUBE";
    case Platform::YouTubeMusic: return L"YT MUSIC";
    case Platform::Browser: break;
  }
  return browserLabel.empty() ? L"WEB" : browserLabel;
}

// Sem artista, só anunciamos o que sabemos ser YouTube: no navegador, mídia
// sem metadados costuma ser vídeo solto de outro site; no Spotify, anúncio.
inline bool WorthAnnouncing(Platform p, const std::wstring& artist) {
  return !artist.empty() || p == Platform::YouTube || p == Platform::YouTubeMusic;
}

// Corta texto de metadados num tamanho exibível, sem partir par surrogate.
inline constexpr size_t kMaxTextChars = 256;
inline std::wstring ClampText(std::wstring s, size_t maxChars = kMaxTextChars) {
  if (s.size() <= maxChars) return s;
  size_t cut = maxChars;
  if (cut > 0 && s[cut - 1] >= 0xD800 && s[cut - 1] <= 0xDBFF) --cut;  // high surrogate órfão
  s.resize(cut);
  return s;
}

// ------------------------------------------------------------ capa

// Paleta da família "cyber" (RGB 0..1): fonte única para o painel (hud.cpp)
// e para o tingimento da capa.
struct Rgb {
  float r, g, b;
};
inline constexpr Rgb kPaletteBase = {0.016f, 0.022f, 0.034f};  // fundo do painel
inline constexpr Rgb kPaletteCyan = {0.0f, 0.898f, 1.0f};      // destaque
inline constexpr Rgb kPaletteHot = {0.78f, 0.98f, 1.0f};       // ciano quase branco

// Lado (pixels) da capa decodificada: ~2x o tamanho na tela, nítida em 200 %.
inline constexpr int kArtPixels = 96;
inline constexpr int kArtStride = kArtPixels * 4;  // BGRA
inline constexpr size_t kArtBytes = static_cast<size_t>(kArtStride) * kArtPixels;
// Capa vem de qualquer app — ou de um site, via Media Session do navegador.
inline constexpr uint64_t kMaxArtFileBytes = 8ull << 20;
inline constexpr uint32_t kMaxArtSide = 4096;  // contra "bomba" de descompressão

// Formatos aceitos para a capa: só os comuns, decodificados pelos codecs
// nativos do Windows (nunca um codec de terceiro com dado não confiável).
enum class ImageFormat { Unknown, Png, Jpeg, Gif, Bmp };

inline ImageFormat SniffImage(const uint8_t* data, size_t size) {
  auto starts = [&](std::initializer_list<uint8_t> magic) {
    if (size < magic.size()) return false;
    return std::equal(magic.begin(), magic.end(), data);
  };
  if (starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return ImageFormat::Png;
  if (starts({0xFF, 0xD8, 0xFF})) return ImageFormat::Jpeg;
  if (starts({'G', 'I', 'F', '8'})) return ImageFormat::Gif;
  if (starts({'B', 'M'})) return ImageFormat::Bmp;
  return ImageFormat::Unknown;
}

// Capa em monocromático ciano, no idioma do painel: a luminância vira
// intensidade entre o fundo do painel, o ciano e o ciano quase branco; uma
// linha em cada três é escurecida (scanline). bgra: BGRA pré-multiplicado
// (o formato do WIC/Direct2D), `width` pixels por linha.
inline void TintArt(std::vector<uint8_t>& bgra, int width) {
  constexpr float kCyanAt = 0.75f;   // luminância que vira ciano puro
  constexpr float kScanline = 0.72f;
  auto mix = [](Rgb a, Rgb b, float t) {
    return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
  };
  if (width <= 0) return;
  const size_t pixels = bgra.size() / 4;
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t* p = &bgra[i * 4];
    const float a = p[3] / 255.0f;
    if (a <= 0.0f) continue;
    // Desfaz o pré-multiplicado para medir a luminância da cor real.
    float lum = (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) / (255.0f * a);
    lum = std::clamp((lum - 0.08f) / 0.84f, 0.0f, 1.0f);  // um pouco de contraste
    const Rgb c = lum < kCyanAt ? mix(kPaletteBase, kPaletteCyan, lum / kCyanAt)
                                : mix(kPaletteCyan, kPaletteHot, (lum - kCyanAt) / (1.0f - kCyanAt));
    const float k = 255.0f * a * ((i / static_cast<size_t>(width)) % 3 == 2 ? kScanline : 1.0f);
    p[0] = static_cast<uint8_t>(std::lround(c.b * k));
    p[1] = static_cast<uint8_t>(std::lround(c.g * k));
    p[2] = static_cast<uint8_t>(std::lround(c.r * k));
  }
}

// ------------------------------------------------------------ música nova

// Chave da faixa. A fonte entra na chave: a mesma música trocando do
// Spotify para o YouTube é um evento novo.
inline std::wstring TrackKey(const std::wstring& source, const std::wstring& title,
                             const std::wstring& artist) {
  return Lower(source) + L"\x1f" + title + L"\x1f" + artist;
}

// Decide quando anunciar. Alimentado a cada consulta com a faixa tocando
// agora (chave vazia = nada tocando). Só anuncia uma chave depois de vê-la
// em `stablePolls` consultas seguidas: o Spotify atualiza título e artista
// em dois passos, e sem isso o HUD mostraria "música nova / artista velho".
// Pausar e retomar a mesma faixa não anuncia de novo.
class ChangeDetector {
 public:
  explicit ChangeDetector(int stablePolls = 2) : stablePolls_(std::max(1, stablePolls)) {}

  bool Update(const std::wstring& key) {
    if (key.empty() || key == announced_) {
      pending_.clear();
      count_ = 0;
      return false;
    }
    if (key == pending_) {
      ++count_;
    } else {
      pending_ = key;
      count_ = 1;
    }
    if (count_ < stablePolls_) return false;
    announced_ = key;
    pending_.clear();
    count_ = 0;
    return true;
  }

  const std::wstring& Announced() const { return announced_; }

 private:
  int stablePolls_;
  std::wstring announced_;
  std::wstring pending_;
  int count_ = 0;
};

// ------------------------------------------------------------ fade

struct Timeline {
  double fadeInMs = 280;
  double holdMs = 4600;
  double fadeOutMs = 700;
  double Total() const { return fadeInMs + holdMs + fadeOutMs; }
};

inline double EaseOutCubic(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

// Opacidade 0..1 em `ms` desde a exibição.
inline double HudAlpha(const Timeline& tl, double ms) {
  if (ms <= 0) return 0.0;
  if (ms < tl.fadeInMs) return EaseOutCubic(ms / tl.fadeInMs);
  const double outStart = tl.fadeInMs + tl.holdMs;
  if (ms < outStart) return 1.0;
  if (ms >= tl.Total()) return 0.0;
  const double t = (ms - outStart) / tl.fadeOutMs;
  return 1.0 - t * t;  // ease-in: começa a sumir devagar
}

// Deslocamento vertical (fração 0..1 da distância de entrada) — o painel sobe
// enquanto aparece.
inline double HudSlide(const Timeline& tl, double ms) {
  return 1.0 - EaseOutCubic(ms / tl.fadeInMs);
}

// Fração 0..1 do tempo de exibição restante (linha fina que encolhe).
inline double HudRemaining(const Timeline& tl, double ms) {
  return std::clamp(1.0 - ms / tl.Total(), 0.0, 1.0);
}

// ------------------------------------------------------------ texto "decodificando"

// Revela `target` da esquerda para a direita conforme progress vai de 0 a 1;
// o trecho ainda não revelado mostra glifos que trocam com `frame`.
// Espaços ficam espaços, para a forma da palavra aparecer desde o início.
inline std::wstring DecodeText(const std::wstring& target, double progress, uint32_t frame) {
  static constexpr wchar_t kGlyphs[] = L"/\\|<>_-+=*#01x";
  constexpr uint32_t kGlyphCount = static_cast<uint32_t>(sizeof(kGlyphs) / sizeof(kGlyphs[0]) - 1);
  if (progress >= 1.0) return target;
  const size_t revealed = static_cast<size_t>(std::clamp(progress, 0.0, 1.0) * target.size());
  std::wstring out = target;
  for (size_t i = revealed; i < out.size(); ++i) {
    if (std::iswspace(out[i])) continue;
    uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ frame * 40503u;
    h ^= h >> 13;
    out[i] = kGlyphs[h % kGlyphCount];
  }
  return out;
}

// ------------------------------------------------------------ espectro

// FFT radix-2 iterativa, in-place. size deve ser potência de 2.
inline void Fft(std::vector<std::complex<float>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
    const std::complex<float> step(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<float> u = a[i + k];
        const std::complex<float> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
}

// Converte blocos de áudio mono em níveis 0..1 por banda (escala log de
// frequência e de amplitude), com ataque rápido, queda suave e marcas de pico.
// O nível de referência acompanha o volume da música, para as barras usarem a
// altura toda tanto com o volume do app alto quanto baixo; abaixo de
// kSilenceDb tudo é silêncio de verdade e as barras ficam no chão.
class SpectrumAnalyzer {
 public:
  static constexpr size_t kFftSize = 2048;
  static constexpr float kRangeDb = 42.0f;
  static constexpr float kSilenceDb = -75.0f;
  static constexpr float kResponseGamma = 1.7f;

  SpectrumAnalyzer(int bands, float sampleRate, float minHz = 45.0f, float maxHz = 15000.0f)
      : bands_(std::max(1, bands)),
        levels_(bands_, 0.0f),
        peaks_(bands_, 0.0f),
        raw_(bands_, 0.0f),
        window_(kFftSize),
        buffer_(kFftSize) {
    for (size_t i = 0; i < kFftSize; ++i) {
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                          static_cast<float>(kFftSize - 1));
      windowSum_ += window_[i];
    }
    SetSampleRate(sampleRate, minHz, maxHz);
  }

  void SetSampleRate(float sampleRate, float minHz = 45.0f, float maxHz = 15000.0f) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0f;
    const float binHz = sampleRate_ / static_cast<float>(kFftSize);
    const float nyquist = sampleRate_ * 0.5f;
    maxHz = std::min(maxHz, nyquist * 0.95f);
    edges_.assign(static_cast<size_t>(bands_) + 1, 0);
    centers_.assign(static_cast<size_t>(bands_), 0.0f);
    size_t prev = 1;
    for (int b = 0; b <= bands_; ++b) {
      const float hz = minHz * std::pow(maxHz / minHz, static_cast<float>(b) / static_cast<float>(bands_));
      size_t bin = static_cast<size_t>(std::lround(hz / binHz));
      bin = std::clamp<size_t>(bin, 1, kFftSize / 2);
      if (b > 0) bin = std::max(bin, prev + 1);  // toda banda tem pelo menos 1 bin
      edges_[static_cast<size_t>(b)] = std::min(bin, kFftSize / 2);
      prev = edges_[static_cast<size_t>(b)];
    }
    for (int b = 0; b < bands_; ++b) {
      const float lo = static_cast<float>(edges_[static_cast<size_t>(b)]) * binHz;
      const float hi = static_cast<float>(edges_[static_cast<size_t>(b) + 1]) * binHz;
      centers_[static_cast<size_t>(b)] = std::sqrt(std::max(lo, 1.0f) * std::max(hi, 1.0f));
    }
  }

  // samples: os kFftSize mais recentes (mono, -1..1). dt: segundos desde a última chamada.
  void Process(const float* samples, float dt) {
    for (size_t i = 0; i < kFftSize; ++i) buffer_[i] = {samples[i] * window_[i], 0.0f};
    Fft(buffer_);

    const float norm = 2.0f / windowSum_;  // senoide de amplitude 1 -> magnitude 1
    float loudest = -200.0f;
    for (int b = 0; b < bands_; ++b) {
      const size_t lo = edges_[static_cast<size_t>(b)];
      const size_t hi = edges_[static_cast<size_t>(b) + 1];
      float power = 0.0f;
      for (size_t k = lo; k < hi && k < kFftSize / 2; ++k) power = std::max(power, std::norm(buffer_[k]));
      float db = 10.0f * std::log10(power * norm * norm + 1e-12f);
      // Música tem menos energia nos agudos (~ -3 dB/oitava); compensar deixa
      // as barras da direita vivas sem inventar sinal.
      db += 3.0f * std::log2(centers_[static_cast<size_t>(b)] / 1000.0f);
      raw_[static_cast<size_t>(b)] = db;
      loudest = std::max(loudest, db);
    }

    // Referência: sobe na hora com o pico, desce 6 dB/s.
    reference_ = std::max(loudest, reference_ - 6.0f * dt);
    reference_ = std::clamp(reference_, kSilenceDb + kRangeDb, 6.0f);

    const float attack = std::min(1.0f, dt * 28.0f);
    const float release = std::min(1.0f, dt * 7.0f);
    for (int b = 0; b < bands_; ++b) {
      const size_t i = static_cast<size_t>(b);
      float target = raw_[i] < kSilenceDb ? 0.0f : (raw_[i] - (reference_ - kRangeDb)) / kRangeDb;
      // Curva > 1: bandas fracas encostam no chão e o que pulsa se destaca,
      // em vez de um "piso" de barras paradas na metade de baixo.
      target = std::pow(std::clamp(target, 0.0f, 1.0f), kResponseGamma);
      levels_[i] += (target - levels_[i]) * (target > levels_[i] ? attack : release);
      peaks_[i] = std::max(levels_[i], peaks_[i] - 0.55f * dt);
    }
  }

  // Sem áudio chegando: tudo desce suavemente até o chão.
  void Decay(float dt) {
    const float release = std::min(1.0f, dt * 7.0f);
    for (int b = 0; b < bands_; ++b) {
      const size_t i = static_cast<size_t>(b);
      levels_[i] -= levels_[i] * release;
      peaks_[i] = std::max(levels_[i], peaks_[i] - 0.55f * dt);
    }
  }

  void Reset() {
    std::fill(levels_.begin(), levels_.end(), 0.0f);
    std::fill(peaks_.begin(), peaks_.end(), 0.0f);
    reference_ = kSilenceDb + kRangeDb;
  }

  // Média das bandas abaixo de ~150 Hz: o "pulso" do bumbo/baixo.
  float Bass() const {
    float sum = 0.0f;
    int count = 0;
    for (int b = 0; b < bands_ && centers_[static_cast<size_t>(b)] < 150.0f; ++b, ++count) {
      sum += levels_[static_cast<size_t>(b)];
    }
    return count ? sum / static_cast<float>(count) : 0.0f;
  }

  int Bands() const { return bands_; }
  const std::vector<float>& Levels() const { return levels_; }
  const std::vector<float>& Peaks() const { return peaks_; }
  float BandCenterHz(int band) const { return centers_[static_cast<size_t>(band)]; }

 private:
  int bands_;
  float sampleRate_ = 48000.0f;
  float windowSum_ = 0.0f;
  float reference_ = kSilenceDb + kRangeDb;
  std::vector<float> levels_;
  std::vector<float> peaks_;
  std::vector<float> raw_;
  std::vector<float> window_;
  std::vector<float> centers_;
  std::vector<size_t> edges_;
  std::vector<std::complex<float>> buffer_;
};

}  // namespace cmh

// Testes de unidade da lógica pura (src/logic.h). Sem framework: cada CHECK
// que falha imprime a linha e o processo sai com código 1.
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "../src/logic.h"

using namespace cmh;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

static void TestClassifySource() {
  CHECK(ClassifySource(L"Spotify.exe").platform == Platform::Spotify);
  CHECK(ClassifySource(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify").exe == L"spotify.exe");
  CHECK(!ClassifySource(L"Spotify.exe").browser);

  const Source chrome = ClassifySource(L"Chrome");
  CHECK(chrome.relevant && chrome.browser && chrome.exe == L"chrome.exe" && chrome.label == L"CHROME");
  CHECK(chrome.platform == Platform::Browser);
  CHECK(ClassifySource(L"MSEdge").exe == L"msedge.exe");
  CHECK(ClassifySource(L"308046B0AF4A39CB").exe == L"firefox.exe");
  CHECK(ClassifySource(L"Chrome._crx_cinhimbnkkaeohfgghhklpknlkffjgod").platform == Platform::YouTubeMusic);

  CHECK(!ClassifySource(L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic").relevant);
  CHECK(!ClassifySource(L"vlc.exe").relevant);
  CHECK(!ClassifySource(L"").relevant);
}

static void TestPlatformFromWindowTitles() {
  const std::vector<std::wstring> yt = {L"Tank! - Seatbelts - YouTube - Google Chrome"};
  CHECK(PlatformFromWindowTitles(yt, L"Tank!") == Platform::YouTube);
  CHECK(PlatformFromWindowTitles({L"Tank! - YouTube Music"}, L"Tank!") == Platform::YouTubeMusic);

  // A janela com o título da faixa manda, mesmo havendo YouTube em outra.
  const std::vector<std::wstring> mixed = {L"Inbox - Gmail", L"Algo - YouTube - Chrome",
                                           L"Tank! | Bandcamp - Chrome"};
  CHECK(PlatformFromWindowTitles(mixed, L"Tank!") == Platform::Browser);

  // Aba em segundo plano: título não aparece; vale qualquer janela do YouTube.
  CHECK(PlatformFromWindowTitles({L"Docs - Chrome", L"Outro vídeo - YouTube - Chrome"}, L"Tank!") ==
        Platform::YouTube);
  CHECK(PlatformFromWindowTitles({L"Docs - Chrome"}, L"Tank!") == Platform::Browser);
  CHECK(PlatformFromWindowTitles({}, L"Tank!") == Platform::Browser);
  // Maiúsculas/minúsculas não importam.
  CHECK(PlatformFromWindowTitles({L"TANK! - youtube"}, L"tank!") == Platform::YouTube);
}

static void TestLabelsAndFilter() {
  CHECK(PlatformLabel(Platform::Spotify, L"") == L"SPOTIFY");
  CHECK(PlatformLabel(Platform::YouTube, L"CHROME") == L"YOUTUBE");
  CHECK(PlatformLabel(Platform::Browser, L"CHROME") == L"CHROME");
  CHECK(PlatformLabel(Platform::Browser, L"") == L"WEB");

  CHECK(WorthAnnouncing(Platform::Spotify, L"Seatbelts"));
  CHECK(!WorthAnnouncing(Platform::Spotify, L""));  // anúncio do Spotify
  CHECK(WorthAnnouncing(Platform::YouTube, L""));
  CHECK(!WorthAnnouncing(Platform::Browser, L""));  // vídeo solto de outro site
  CHECK(WorthAnnouncing(Platform::Browser, L"Canal"));

  CHECK(ClampText(L"curto") == L"curto");
  CHECK(ClampText(std::wstring(1000, L'x')).size() == kMaxTextChars);
  std::wstring emoji = std::wstring(3, L'a') + L"\xD83C\xDFB5";  // "aaa🎵"
  CHECK(ClampText(emoji, 4) == L"aaa");  // não deixa metade do emoji
}

static void TestChangeDetector() {
  ChangeDetector d(2);
  const std::wstring a = TrackKey(L"Spotify.exe", L"Tank!", L"Seatbelts");
  const std::wstring b = TrackKey(L"Spotify.exe", L"Rain", L"Seatbelts");

  CHECK(!d.Update(a));  // primeira vez: espera estabilizar
  CHECK(d.Update(a));   // segunda: anuncia
  CHECK(!d.Update(a));  // continua tocando: nada
  CHECK(!d.Update(L"")); // pausou
  CHECK(!d.Update(a));  // retomou a mesma: não repete
  CHECK(!d.Update(a));

  // Spotify em dois passos: título novo com artista velho por uma consulta.
  const std::wstring half = TrackKey(L"Spotify.exe", L"Rain", L"Old Artist");
  CHECK(!d.Update(half));
  CHECK(!d.Update(b));
  CHECK(d.Update(b));
  CHECK(d.Announced() == b);

  // Mesma música em outra fonte é evento novo.
  const std::wstring onYt = TrackKey(L"Chrome", L"Rain", L"Seatbelts");
  CHECK(!d.Update(onYt));
  CHECK(d.Update(onYt));

  // Voltar para uma música anterior (não a última) anuncia de novo.
  CHECK(!d.Update(a));
  CHECK(d.Update(a));

  // A fonte entra na chave sem diferenciar caixa.
  CHECK(TrackKey(L"Spotify.exe", L"x", L"y") == TrackKey(L"SPOTIFY.EXE", L"x", L"y"));

  ChangeDetector instant(1);
  CHECK(instant.Update(a));
}

static void TestTimeline() {
  Timeline tl;
  CHECK(HudAlpha(tl, 0) == 0.0);
  CHECK(HudAlpha(tl, tl.fadeInMs / 2) > 0.5);  // ease-out: já passou da metade
  CHECK(HudAlpha(tl, tl.fadeInMs) == 1.0);
  CHECK(HudAlpha(tl, tl.fadeInMs + tl.holdMs - 1) == 1.0);
  const double outMid = tl.fadeInMs + tl.holdMs + tl.fadeOutMs / 2;
  CHECK(HudAlpha(tl, outMid) > 0.5 && HudAlpha(tl, outMid) < 1.0);
  CHECK(HudAlpha(tl, tl.Total()) == 0.0);
  CHECK(tl.Total() >= 4000 && tl.Total() <= 6000);  // requisito: ~4–6 s na tela

  CHECK(HudSlide(tl, 0) == 1.0);
  CHECK(HudSlide(tl, tl.fadeInMs) == 0.0);
  CHECK(HudRemaining(tl, 0) == 1.0);
  CHECK(HudRemaining(tl, tl.Total() * 2) == 0.0);
}

static void TestDecodeText() {
  const std::wstring target = L"Tank! Seatbelts";
  CHECK(DecodeText(target, 1.0, 7) == target);
  const std::wstring start = DecodeText(target, 0.0, 7);
  CHECK(start.size() == target.size());
  CHECK(start[5] == L' ');  // espaços preservados
  CHECK(start != target);
  const std::wstring half = DecodeText(target, 0.5, 7);
  CHECK(half.substr(0, 7) == target.substr(0, 7));
  CHECK(DecodeText(L"", 0.3, 1).empty());
  // Determinístico para o mesmo quadro, muda entre quadros.
  CHECK(DecodeText(target, 0.2, 3) == DecodeText(target, 0.2, 3));
  CHECK(DecodeText(target, 0.2, 3) != DecodeText(target, 0.2, 4));
}

static std::vector<float> Sine(float hz, float amplitude, float rate) {
  std::vector<float> s(SpectrumAnalyzer::kFftSize);
  for (size_t i = 0; i < s.size(); ++i) {
    s[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * hz * static_cast<float>(i) / rate);
  }
  return s;
}

static int LoudestBand(const SpectrumAnalyzer& a) {
  int best = 0;
  for (int b = 1; b < a.Bands(); ++b) {
    if (a.Levels()[static_cast<size_t>(b)] > a.Levels()[static_cast<size_t>(best)]) best = b;
  }
  return best;
}

static void TestFft() {
  // Senoide exatamente no bin 8 concentra a energia lá.
  std::vector<std::complex<float>> x(64);
  for (size_t i = 0; i < x.size(); ++i) x[i] = {std::cos(2.0f * std::numbers::pi_v<float> * 8.0f * i / 64.0f), 0};
  Fft(x);
  CHECK(std::abs(std::abs(x[8]) - 32.0f) < 1e-3f);
  CHECK(std::abs(x[3]) < 1e-3f);
}

static void TestSpectrum() {
  const float rate = 48000.0f;
  for (float hz : {100.0f, 1000.0f, 8000.0f}) {
    SpectrumAnalyzer a(36, rate);
    const std::vector<float> s = Sine(hz, 0.5f, rate);
    for (int i = 0; i < 30; ++i) a.Process(s.data(), 1.0f / 60.0f);
    const int band = LoudestBand(a);
    const float center = a.BandCenterHz(band);
    // A banda mais alta é a que contém a frequência (tolerância de ~meia oitava).
    CHECK(center > hz / 1.5f && center < hz * 1.5f);
    CHECK(a.Levels()[static_cast<size_t>(band)] > 0.8f);
    // Longe da frequência, as barras ficam baixas: reage ao sinal, não oscila à toa.
    CHECK(a.Levels()[static_cast<size_t>(band > 18 ? 0 : 35)] < 0.2f);
  }

  // Silêncio: tudo no chão.
  SpectrumAnalyzer quiet(36, rate);
  const std::vector<float> zeros(SpectrumAnalyzer::kFftSize, 0.0f);
  for (int i = 0; i < 30; ++i) quiet.Process(zeros.data(), 1.0f / 60.0f);
  for (float level : quiet.Levels()) CHECK(level == 0.0f);

  // Volume baixo ainda ocupa a altura (referência adaptativa)...
  SpectrumAnalyzer soft(36, rate);
  const std::vector<float> s = Sine(1000.0f, 0.02f, rate);
  for (int i = 0; i < 120; ++i) soft.Process(s.data(), 1.0f / 60.0f);
  CHECK(soft.Levels()[static_cast<size_t>(LoudestBand(soft))] > 0.8f);

  // ...e as barras descem quando o áudio para.
  const float before = soft.Levels()[static_cast<size_t>(LoudestBand(soft))];
  for (int i = 0; i < 30; ++i) soft.Decay(1.0f / 60.0f);
  CHECK(soft.Levels()[static_cast<size_t>(LoudestBand(soft))] < before * 0.1f);

  // Grave puxa Bass(); agudo não.
  SpectrumAnalyzer low(36, rate), high(36, rate);
  const std::vector<float> s60 = Sine(70.0f, 0.5f, rate), s5k = Sine(5000.0f, 0.5f, rate);
  for (int i = 0; i < 30; ++i) {
    low.Process(s60.data(), 1.0f / 60.0f);
    high.Process(s5k.data(), 1.0f / 60.0f);
  }
  CHECK(low.Bass() > 0.3f);
  CHECK(high.Bass() < 0.05f);

  // Taxas diferentes (44,1 kHz) mantêm as bandas válidas e crescentes.
  SpectrumAnalyzer other(36, 44100.0f);
  for (int b = 1; b < other.Bands(); ++b) CHECK(other.BandCenterHz(b) > other.BandCenterHz(b - 1));
}

int main() {
  TestClassifySource();
  TestPlatformFromWindowTitles();
  TestLabelsAndFilter();
  TestChangeDetector();
  TestTimeline();
  TestDecodeText();
  TestFft();
  TestSpectrum();
  std::printf("%d verificações, %d falhas\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

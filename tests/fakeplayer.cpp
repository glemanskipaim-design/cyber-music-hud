// Player de teste: toca duas "músicas" sintetizadas pelo MediaPlayer do
// WinRT, que se registra no painel de mídia do Windows (GSMTC) como qualquer
// player. Compilado como build\fake\Spotify.exe, é classificado como Spotify
// e o loopback por processo captura exatamente o áudio dele — exercita o
// caminho inteiro do HUD sem depender do Spotify/YouTube reais.
//
// Uso: build\fake\Spotify.exe [segundos_por_faixa]   (padrão 12)
#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;

namespace {

constexpr int kRate = 44100;

// Bumbo, baixo e acorde em loop: espectro com graves e agudos que variam no
// tempo, para as barras terem o que mostrar.
std::vector<int16_t> Synth(double seconds, double rootHz, double bpm) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  std::vector<int16_t> pcm(n);
  const double beat = 60.0 / bpm;
  const double steps[] = {1.0, 1.0, 1.335, 1.5, 1.0, 1.782, 1.5, 1.335};
  uint32_t noise = 12345;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double inBeat = std::fmod(t, beat);
    const int step = static_cast<int>(t / beat) % 8;
    const double f = rootHz * steps[step];
    double s = 0.0;
    s += 0.45 * std::sin(2 * std::numbers::pi * (50 + 80 * std::exp(-inBeat * 30)) * inBeat) * std::exp(-inBeat * 9);
    s += 0.22 * std::sin(2 * std::numbers::pi * f * t) * std::exp(-inBeat * 3);
    s += 0.10 * std::sin(2 * std::numbers::pi * f * 2.0 * t) * std::exp(-inBeat * 5);
    s += 0.07 * std::sin(2 * std::numbers::pi * f * 3.0 * t) * std::exp(-inBeat * 7);
    const double half = std::fmod(t + beat / 2, beat);
    noise = noise * 1664525u + 1013904223u;
    s += 0.10 * ((noise >> 9) / 4194304.0 - 1.0) * std::exp(-half * 40);  // chimbal
    pcm[i] = static_cast<int16_t>(std::clamp(s, -1.0, 1.0) * 24000);
  }
  return pcm;
}

std::wstring WriteWav(const std::wstring& path, const std::vector<int16_t>& pcm) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return {};
  const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(kRate); u32(kRate * 2); u16(2); u16(16);
  fwrite("data", 1, 4, f); u32(data);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
  return path;
}

MediaPlaybackItem MakeItem(const std::wstring& wav, const wchar_t* title, const wchar_t* artist) {
  auto file = Windows::Storage::StorageFile::GetFileFromPathAsync(wav).get();
  MediaPlaybackItem item(MediaSource::CreateFromStorageFile(file));
  auto props = item.GetDisplayProperties();
  props.Type(MediaPlaybackType::Music);
  props.MusicProperties().Title(title);
  props.MusicProperties().Artist(artist);
  item.ApplyDisplayProperties(props);
  return item;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  init_apartment(apartment_type::multi_threaded);
  const double seconds = argc > 1 ? _wtof(argv[1]) : 12.0;

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const std::wstring a = WriteWav(std::wstring(temp) + L"cmh-fake-1.wav", Synth(seconds, 82.4, 118));
  const std::wstring b = WriteWav(std::wstring(temp) + L"cmh-fake-2.wav", Synth(seconds, 110.0, 96));
  if (a.empty() || b.empty()) return 1;

  MediaPlaybackList list;
  list.Items().Append(MakeItem(a, L"Tank!", L"Seatbelts"));
  list.Items().Append(MakeItem(b, L"Rush", L"Seatbelts"));
  MediaPlayer player;
  player.Source(list);
  player.Play();
  std::printf("tocando 2 faixas de %.0f s\n", seconds);
  Sleep(static_cast<DWORD>(seconds * 2 * 1000 + 1500));
  player.Pause();
  return 0;
}

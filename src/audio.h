// Captura do áudio que está tocando, para o visualizador.
//
// Preferência: loopback por processo (Windows 10 2004+), que ouve só a árvore
// de processos do player — notificações e outros apps não mexem nas barras.
// Se não der (processo sumiu, Windows antigo), cai para o loopback do
// dispositivo de saída padrão, que ouve tudo que sai no alto-falante.
//
// A captura roda numa thread própria e só existe enquanto o HUD está visível.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace cmh {

enum class CaptureMode { None, Process, System };

// Últimas amostras mono, escritas pela thread de captura e lidas pela UI.
class SampleRing {
 public:
  explicit SampleRing(size_t capacity);
  void Push(const float* samples, size_t count);
  // Copia as `count` amostras mais recentes (zeros se ainda não houver tantas).
  void Latest(float* out, size_t count) const;
  // Quantas amostras chegaram desde a última chamada.
  size_t TakeArrived();
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::vector<float> data_;
  size_t write_ = 0;
  size_t filled_ = 0;
  size_t arrived_ = 0;
};

class AudioCapture {
 public:
  AudioCapture();
  ~AudioCapture();
  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  // pid 0 = vai direto para o loopback do sistema. Retorna na hora; a
  // ativação acontece na thread de captura.
  void Start(DWORD pid);
  void Stop();

  CaptureMode Mode() const { return mode_.load(); }
  float SampleRate() const { return sampleRate_.load(); }
  SampleRing& Samples() { return ring_; }

 private:
  void Run(DWORD pid);

  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  std::atomic<CaptureMode> mode_{CaptureMode::None};
  std::atomic<float> sampleRate_{48000.0f};
  SampleRing ring_;
};

}  // namespace cmh

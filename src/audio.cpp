#include "audio.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace cmh {
namespace {

constexpr REFERENCE_TIME kBufferHns = 200'000;  // 20 ms
constexpr DWORD kActivateTimeoutMs = 2000;
constexpr DWORD kCaptureWaitMs = 10;
constexpr size_t kRingCapacity = 8192;

// Recebe o resultado de ActivateAudioInterfaceAsync. FtmBase: o callback
// chega numa thread do pool de MTA.
class ActivateHandler
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
 public:
  ActivateHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~ActivateHandler() override {
    if (done_) CloseHandle(done_);
  }

  STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
    HRESULT activateResult = E_FAIL;
    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(operation->GetActivateResult(&activateResult, &unknown)) && SUCCEEDED(activateResult)) {
      unknown.As(&client_);
    }
    SetEvent(done_);
    return S_OK;
  }

  // nullptr se falhou, passou do prazo ou `stop` foi sinalizado (o HUD
  // escondeu): Stop() na thread de UI nunca espera a ativação inteira.
  ComPtr<IAudioClient> Wait(DWORD timeoutMs, HANDLE stop) {
    if (!done_) return nullptr;
    const HANDLE waits[] = {done_, stop};
    if (WaitForMultipleObjects(2, waits, FALSE, timeoutMs) != WAIT_OBJECT_0) return nullptr;
    return client_;
  }

 private:
  HANDLE done_;
  ComPtr<IAudioClient> client_;
};

// Loopback só da árvore de processos de `pid`. Formato pedido por nós (o
// motor converte): float estéreo 48 kHz.
ComPtr<IAudioClient> OpenProcessLoopback(DWORD pid, HANDLE stop, WAVEFORMATEX& format) {
  AUDIOCLIENT_ACTIVATION_PARAMS params = {};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT activation = {};
  activation.vt = VT_BLOB;
  activation.blob.cbSize = sizeof(params);
  activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  auto handler = Microsoft::WRL::Make<ActivateHandler>();
  if (!handler) return nullptr;
  ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
  if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                         &activation, handler.Get(), &operation))) {
    return nullptr;
  }
  ComPtr<IAudioClient> client = handler->Wait(kActivateTimeoutMs, stop);
  if (!client) return nullptr;

  format = {};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = 2;
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 32;
  format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                kBufferHns, 0, &format, nullptr))) {
    return nullptr;
  }
  return client;
}

struct MixFormatDeleter {
  void operator()(WAVEFORMATEX* f) const { CoTaskMemFree(f); }
};

// Loopback do dispositivo de saída padrão, no formato de mixagem dele.
ComPtr<IAudioClient> OpenSystemLoopback(WAVEFORMATEX& format) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
    return nullptr;
  }
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return nullptr;
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return nullptr;

  WAVEFORMATEX* raw = nullptr;
  if (FAILED(client->GetMixFormat(&raw)) || !raw) return nullptr;
  std::unique_ptr<WAVEFORMATEX, MixFormatDeleter> mix(raw);
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferHns, 0,
                                mix.get(), nullptr))) {
    return nullptr;
  }
  format = *mix;
  // Para o laço de leitura basta saber se é float de 32 bits ou PCM de 16.
  if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix.get());
    format.wFormatTag = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? WAVE_FORMAT_IEEE_FLOAT
                        : ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM     ? WAVE_FORMAT_PCM
                                                                         : WAVE_FORMAT_UNKNOWN;
  }
  return client;
}

bool SupportedFormat(const WAVEFORMATEX& f) {
  return f.nChannels > 0 && ((f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && f.wBitsPerSample == 32) ||
                             (f.wFormatTag == WAVE_FORMAT_PCM && f.wBitsPerSample == 16));
}

// Média dos canais de cada quadro.
void DownmixTo(std::vector<float>& mono, const BYTE* data, UINT32 frames, const WAVEFORMATEX& f) {
  mono.resize(frames);
  const UINT32 channels = f.nChannels;
  const float scale = 1.0f / static_cast<float>(channels);
  if (f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    const auto* s = reinterpret_cast<const float*>(data);
    for (UINT32 i = 0; i < frames; ++i) {
      float sum = 0;
      for (UINT32 c = 0; c < channels; ++c) sum += s[i * channels + c];
      mono[i] = sum * scale;
    }
  } else {
    const auto* s = reinterpret_cast<const int16_t*>(data);
    for (UINT32 i = 0; i < frames; ++i) {
      float sum = 0;
      for (UINT32 c = 0; c < channels; ++c) sum += static_cast<float>(s[i * channels + c]) / 32768.0f;
      mono[i] = sum * scale;
    }
  }
}

}  // namespace

// ------------------------------------------------------------ SampleRing

SampleRing::SampleRing(size_t capacity) : data_(capacity, 0.0f) {}

void SampleRing::Push(const float* samples, size_t count) {
  std::lock_guard lock(mutex_);
  const size_t cap = data_.size();
  if (count > cap) {
    samples += count - cap;
    arrived_ += count - cap;
    count = cap;
  }
  for (size_t i = 0; i < count; ++i) {
    data_[write_] = samples[i];
    write_ = (write_ + 1) % cap;
  }
  filled_ = std::min(cap, filled_ + count);
  arrived_ += count;
}

void SampleRing::Latest(float* out, size_t count) const {
  std::lock_guard lock(mutex_);
  const size_t cap = data_.size();
  const size_t available = std::min({count, filled_, cap});
  const size_t zeros = count - available;
  std::fill(out, out + zeros, 0.0f);
  size_t read = (write_ + cap - available) % cap;
  for (size_t i = zeros; i < count; ++i) {
    out[i] = data_[read];
    read = (read + 1) % cap;
  }
}

size_t SampleRing::TakeArrived() {
  std::lock_guard lock(mutex_);
  const size_t n = arrived_;
  arrived_ = 0;
  return n;
}

void SampleRing::Clear() {
  std::lock_guard lock(mutex_);
  std::fill(data_.begin(), data_.end(), 0.0f);
  write_ = filled_ = arrived_ = 0;
}

// ------------------------------------------------------------ AudioCapture

AudioCapture::AudioCapture() : ring_(kRingCapacity) {}

AudioCapture::~AudioCapture() { Stop(); }

void AudioCapture::Start(DWORD pid) {
  Stop();
  ring_.Clear();
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) return;
  thread_ = std::thread(&AudioCapture::Run, this, pid);
}

void AudioCapture::Stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  if (stopEvent_) CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
  mode_ = CaptureMode::None;
}

void AudioCapture::Run(DWORD pid) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
  {
    WAVEFORMATEX format = {};
    ComPtr<IAudioClient> client;
    CaptureMode mode = CaptureMode::None;
    if (pid) {
      client = OpenProcessLoopback(pid, stopEvent_, format);
      if (client) mode = CaptureMode::Process;
    }
    if (!client) {
      client = OpenSystemLoopback(format);
      if (client) mode = CaptureMode::System;
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (client && ready && SupportedFormat(format) && SUCCEEDED(client->SetEventHandle(ready)) &&
        SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start())) {
      sampleRate_ = static_cast<float>(format.nSamplesPerSec);
      mode_ = mode;
      std::vector<float> mono;
      const HANDLE waits[] = {stopEvent_, ready};
      // O timeout cobre drivers que não sinalizam o evento em loopback.
      bool healthy = true;
      while (healthy) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kCaptureWaitMs);
        if (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0 + 1) break;  // parar ou erro
        UINT32 packet = 0;
        // Falha aqui = dispositivo invalidado ou player fechado: encerra a
        // captura (Mode() volta a None) em vez de girar à toa.
        while ((healthy = SUCCEEDED(capture->GetNextPacketSize(&packet))) && packet > 0) {
          BYTE* data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
            healthy = false;
            break;
          }
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            mono.assign(frames, 0.0f);
          } else {
            DownmixTo(mono, data, frames, format);
          }
          capture->ReleaseBuffer(frames);
          ring_.Push(mono.data(), mono.size());
        }
      }
      client->Stop();
    }
    if (ready) CloseHandle(ready);
    mode_ = CaptureMode::None;
  }
  CoUninitialize();
}

}  // namespace cmh

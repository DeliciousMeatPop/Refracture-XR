// Audio: libaaudio.so and libOpenSLES.so (buffer-queue players) for the guest and
// refract.media.HostAudio (behind the shim's android.media.AudioTrack / AudioRecord), all on
// WASAPI shared-mode streams of the default Windows
// playback and recording devices. WASAPI converts rate, channel count and sample format, so every
// stream gets exactly the format the app asked for, and an AAudio data callback writes straight into
// the WASAPI buffer (guest addresses are host addresses).
#include "audio_hle.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host_runtime.h"
#include "linux_abi.h"
#include "memory.h"
#include "thunks.h"

namespace rn {

namespace {

// --- AAudio constants (aaudio/AAudio.h) ---------------------------------------------------------

enum : s32 {
    kOk = 0,
    kErrDisconnected = -899,
    kErrIllegalArgument = -898,
    kErrInvalidState = -895,
    kErrInvalidHandle = -892,
    kErrUnavailable = -889,
    kErrTimeout = -885,
    kErrInvalidFormat = -883,
    kErrInvalidRate = -880,
};
enum : s32 { kDirOutput = 0, kDirInput = 1 };
enum : s32 { kFmtUnspecified = 0, kFmtI16 = 1, kFmtFloat = 2, kFmtI24 = 3, kFmtI32 = 4 };
enum : s32 {
    kStateOpen = 2,
    kStateStarting = 3,
    kStateStarted = 4,
    kStatePaused = 6,
    kStateFlushed = 8,
    kStateStopped = 10,
    kStateClosing = 11,
    kStateClosed = 12,
    kStateDisconnected = 13,
};
constexpr s32 kCallbackStop = 1;
constexpr s32 kSharingShared = 1;
// The ids android.media.AudioManager reports for the speaker and the microphone.
constexpr s32 kOutputDeviceId = 2, kInputDeviceId = 1;
constexpr s64 kForever = INT64_MAX / 4;

const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void ComInit() {
    thread_local bool done = false;
    if (!done) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // RPC_E_CHANGED_MODE is fine: WASAPI is free-threaded
        done = true;
    }
}

u32 SampleBytes(s32 format) {
    switch (format) {
    case kFmtI16:
        return 2;
    case kFmtI24:
        return 3;
    default:
        return 4;
    }
}

DWORD ChannelMask(int channels) {
    switch (channels) {
    case 1:
        return 0x4;  // front center
    case 2:
        return 0x3;  // front left/right
    case 4:
        return 0x33;  // quad
    case 6:
        return 0x3f;  // 5.1
    case 8:
        return 0x63f;  // 7.1
    default:
        return 0;
    }
}

IMMDevice* DefaultDevice(bool capture) {
    ComInit();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return nullptr;
    IMMDevice* dev = nullptr;
    en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev);
    en->Release();
    return dev;
}

// The default playback device's mixing rate (what Android reports as the native output rate).
int MixRate() {
    static std::once_flag once;
    static int rate = 48000;
    std::call_once(once, [] {
        IMMDevice* dev = DefaultDevice(false);
        IAudioClient* client = nullptr;
        if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client)))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix))) {
                rate = static_cast<int>(mix->nSamplesPerSec);
                CoTaskMemFree(mix);
            }
            client->Release();
        }
        if (dev)
            dev->Release();
    });
    return rate;
}

// --- streams ---------------------------------------------------------------------------------------

struct Config {
    s32 direction = kDirOutput, sample_rate = 0, channels = 0, format = kFmtUnspecified, sharing = kSharingShared,
        perf = 10 /* NONE */, capacity = 0, frames_per_cb = 0, device_id = 0, session = -1 /* NONE */,
        usage = 1 /* MEDIA */, content = 2 /* MUSIC */, input_preset = 6 /* VOICE_RECOGNITION */,
        allowed_capture = 1, spatialization = 1, spatialized = 0, privacy = 0;
    u32 channel_mask = 0;
    u64 data_cb = 0, data_user = 0, error_cb = 0, error_user = 0;
};

constexpr u32 kStreamMagic = 0x53414152;   // "RAAS"
constexpr u32 kBuilderMagic = 0x42414152;  // "RAAB"

struct Builder {
    u32 magic = kBuilderMagic;
    Config c;
};

struct Stream {
    u32 magic = kStreamMagic;
    Config c;
    bool capture = false;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture_client = nullptr;
    IAudioClock* clock = nullptr;
    HANDLE event = nullptr;
    u32 device_frames = 0;  // WASAPI buffer size
    u32 frame_bytes = 0;
    u32 burst = 0;
    std::atomic<u32> buffer_size{0};
    std::atomic<s32> state{kStateOpen};
    std::atomic<s64> frames_written{0}, frames_read{0};
    std::atomic<s32> xruns{0};
    std::mutex ctl;  // start / stop / close
    std::mutex io;   // blocking read / write and the capture queue
    std::vector<u8> pending;  // captured audio not yet delivered
    size_t pending_off = 0;
    std::vector<u8> cb_buf;
    // Callback mode: a guest worker thread runs the data callback while `running`.
    std::atomic<bool> running{false};
    std::mutex worker_mu;
    std::condition_variable worker_cv;
    bool worker_busy = false;
    bool free_on_exit = false;  // closed from its own callback: the worker frees it

    ~Stream() {
        magic = 0;
        if (clock)
            clock->Release();
        if (render)
            render->Release();
        if (capture_client)
            capture_client->Release();
        if (client)
            client->Release();
        if (event)
            CloseHandle(event);
    }
    size_t PendingBytes() const { return pending.size() - pending_off; }
};

thread_local Stream* tls_worker_stream = nullptr;

Stream* AsStream(Stream* s) { return s && s->magic == kStreamMagic ? s : nullptr; }

s32 OpenStream(const Config& in, Stream** out) {
    *out = nullptr;
    ComInit();
    auto s = std::make_unique<Stream>();
    s->c = in;
    Config& c = s->c;
    s->capture = c.direction == kDirInput;
    if (c.channels <= 0)
        c.channels = s->capture ? 1 : 2;
    if (c.format == kFmtUnspecified)
        c.format = kFmtFloat;
    if (c.format < kFmtI16 || c.format > kFmtI32)
        return kErrInvalidFormat;
    if (c.channels > 8)
        return kErrIllegalArgument;
    if (c.sample_rate <= 0)
        c.sample_rate = MixRate();
    if (c.sample_rate < 4000 || c.sample_rate > 768000)
        return kErrInvalidRate;

    IMMDevice* dev = DefaultDevice(s->capture);
    if (!dev) {
        Log("audio: no default %s device", s->capture ? "recording" : "playback");
        return kErrUnavailable;
    }
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&s->client));
    dev->Release();
    if (FAILED(hr))
        return kErrUnavailable;

    const u32 bytes = SampleBytes(c.format);
    s->frame_bytes = bytes * static_cast<u32>(c.channels);
    s->burst = c.frames_per_cb > 0 ? static_cast<u32>(c.frames_per_cb) : static_cast<u32>(c.sample_rate) / 100;
    const u32 want = std::max({static_cast<u32>(std::max(c.capacity, 0)), s->burst * 3,
                               static_cast<u32>(c.sample_rate) * 30 / 1000});
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(c.channels);
    f.Format.nSamplesPerSec = static_cast<DWORD>(c.sample_rate);
    f.Format.wBitsPerSample = static_cast<WORD>(bytes * 8);
    f.Format.nBlockAlign = static_cast<WORD>(s->frame_bytes);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
    f.dwChannelMask = ChannelMask(c.channels);
    f.SubFormat = c.format == kFmtFloat ? kSubtypeFloat : kSubtypePcm;
    hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               static_cast<REFERENCE_TIME>(want) * 10000000 / c.sample_rate, 0, &f.Format, nullptr);
    if (FAILED(hr)) {
        Log("audio: cannot open a %s stream (%d Hz, %d ch, format %d): 0x%08lx", s->capture ? "recording" : "playback",
            c.sample_rate, c.channels, c.format, hr);
        return kErrUnavailable;
    }
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->client->SetEventHandle(s->event);
    UINT32 frames = 0;
    s->client->GetBufferSize(&frames);
    s->device_frames = frames;
    if (c.frames_per_cb <= 0 && s->burst * 2 > frames)
        s->burst = std::max(1u, frames / 2);
    // Like AAudio, the capacity is only the limit: start at about two bursts (at least 40 ms, which
    // rides out the JIT's slower callbacks), and the app may change it with setBufferSizeInFrames.
    s->buffer_size = std::min(frames, std::max(s->burst * 2, static_cast<u32>(c.sample_rate) / 25));
    if (s->capture)
        hr = s->client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&s->capture_client));
    else
        hr = s->client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&s->render));
    if (FAILED(hr))
        return kErrUnavailable;
    s->client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&s->clock));
    static std::atomic<s32> next_session{1000};
    if (c.session == 0)
        c.session = next_session++;
    c.device_id = s->capture ? kInputDeviceId : kOutputDeviceId;
    c.sharing = kSharingShared;
    s->cb_buf.resize(static_cast<size_t>(s->burst) * s->frame_bytes);
    RN_INFO("audio: %s stream %d Hz, %d ch, format %d, %u frames per burst, buffer %u of %u frames%s",
            s->capture ? "recording" : "playback", c.sample_rate, c.channels, c.format, s->burst,
            s->buffer_size.load(), frames, c.data_cb ? ", callback" : "");
    *out = s.release();
    return kOk;
}

// Moves everything WASAPI has captured into s->pending (s->io held). False if the device is gone.
bool DrainCapture(Stream* s) {
    for (;;) {
        UINT32 packet = 0;
        if (FAILED(s->capture_client->GetNextPacketSize(&packet)))
            return false;
        if (packet == 0)
            return true;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        HRESULT hr = s->capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr))
            return false;
        if (hr == AUDCLNT_S_BUFFER_EMPTY)
            return true;
        const size_t n = static_cast<size_t>(frames) * s->frame_bytes;
        const size_t old = s->pending.size();
        s->pending.resize(old + n);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            memset(s->pending.data() + old, 0, n);
        else
            memcpy(s->pending.data() + old, data, n);
        s->capture_client->ReleaseBuffer(frames);
        // Keep at most a second when nobody reads.
        const size_t max = static_cast<size_t>(s->c.sample_rate) * s->frame_bytes;
        if (s->PendingBytes() > max) {
            s->pending_off = s->pending.size() - max;
            s->xruns++;
        }
    }
}

void CompactPending(Stream* s) {
    if (s->pending_off == s->pending.size()) {
        s->pending.clear();
        s->pending_off = 0;
    } else if (s->pending_off > 65536) {
        s->pending.erase(s->pending.begin(), s->pending.begin() + static_cast<ptrdiff_t>(s->pending_off));
        s->pending_off = 0;
    }
}

u32 WriteLimit(Stream* s) {
    return std::min<u32>(s->buffer_size.load(), s->device_frames);
}

// --- callback mode ---------------------------------------------------------------------------------

// Host threads adopted as guest threads, reused across streams (an adopted thread lives forever).
class GuestWorkers {
public:
    void Post(std::function<void(GuestThread*)> job) {
        std::lock_guard lock(mu_);
        jobs_.push_back(std::move(job));
        if (jobs_.size() > idle_) {
            ++idle_;
            std::thread([this] { Loop(); }).detach();
        }
        cv_.notify_one();
    }

private:
    void Loop() {
        ComInit();
        GuestThread* t = EnsureGuestThread();
        t->name = "AAudio";
        std::unique_lock lock(mu_);
        for (;;) {
            cv_.wait(lock, [&] { return !jobs_.empty(); });
            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            --idle_;
            lock.unlock();
            job(t);
            lock.lock();
            ++idle_;
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void(GuestThread*)>> jobs_;
    size_t idle_ = 0;
};

GuestWorkers& Workers() {
    static GuestWorkers* w = new GuestWorkers;
    return *w;
}

s32 CallData(GuestThread* t, Stream* s, void* data, u32 frames) {
    return static_cast<s32>(CallGuest(t, s->c.data_cb, {reinterpret_cast<u64>(s), s->c.data_user,
                                                       reinterpret_cast<u64>(data), frames}));
}

bool OutputCallbacks(Stream* s, GuestThread* t, bool* stopped) {
    bool started = false;
    while (s->running) {
        UINT32 padding = 0;
        if (FAILED(s->client->GetCurrentPadding(&padding)))
            return false;
        if (started && padding == 0)
            s->xruns++;
        while (s->running && padding + s->burst <= WriteLimit(s)) {
            BYTE* data = nullptr;
            if (FAILED(s->render->GetBuffer(s->burst, &data)))
                return false;
            const s32 r = CallData(t, s, data, s->burst);
            s->render->ReleaseBuffer(s->burst, 0);
            s->frames_written += s->burst;
            padding += s->burst;
            if (r == kCallbackStop) {
                s->running = false;
                *stopped = true;
            }
        }
        if (!started) {
            if (FAILED(s->client->Start()))
                return false;
            started = true;
        }
        WaitForSingleObject(s->event, 100);
    }
    return true;
}

bool InputCallbacks(Stream* s, GuestThread* t, bool* stopped) {
    if (FAILED(s->client->Start()))
        return false;
    const size_t chunk = s->cb_buf.size();
    while (s->running) {
        WaitForSingleObject(s->event, 100);
        std::unique_lock lock(s->io);
        if (!DrainCapture(s))
            return false;
        while (s->running && s->PendingBytes() >= chunk) {
            memcpy(s->cb_buf.data(), s->pending.data() + s->pending_off, chunk);
            s->pending_off += chunk;
            lock.unlock();
            const s32 r = CallData(t, s, s->cb_buf.data(), s->burst);
            s->frames_read += s->burst;
            lock.lock();
            if (r == kCallbackStop) {
                s->running = false;
                *stopped = true;
            }
        }
        CompactPending(s);
    }
    return true;
}

void RunCallbacks(Stream* s, GuestThread* t) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    tls_worker_stream = s;
    bool stopped = false;
    const bool ok = s->capture ? InputCallbacks(s, t, &stopped) : OutputCallbacks(s, t, &stopped);
    s->client->Stop();
    if (!ok) {
        Log("audio: %s device lost", s->capture ? "recording" : "playback");
        s->running = false;
        s->state = kStateDisconnected;
        if (s->c.error_cb)
            CallGuest(t, s->c.error_cb, {reinterpret_cast<u64>(s), s->c.error_user,
                                         static_cast<u64>(static_cast<s64>(kErrDisconnected))});
    } else if (stopped) {
        s->state = kStateStopped;
    }
    tls_worker_stream = nullptr;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    bool free_it;
    {
        std::lock_guard lock(s->worker_mu);
        s->worker_busy = false;
        free_it = s->free_on_exit;
        s->worker_cv.notify_all();
    }
    if (free_it)
        delete s;
}

void WaitWorker(Stream* s) {
    std::unique_lock lock(s->worker_mu);
    s->worker_cv.wait(lock, [&] { return !s->worker_busy; });
}

// --- control ---------------------------------------------------------------------------------------

s32 Start(Stream* s) {
    std::lock_guard lock(s->ctl);
    const s32 st = s->state;
    if (st == kStateDisconnected)
        return kErrDisconnected;
    if (st == kStateClosing || st == kStateClosed)
        return kErrInvalidState;
    if (st == kStateStarted || st == kStateStarting)
        return kOk;
    if (s->c.data_cb) {
        WaitWorker(s);  // a run its callback stopped
        s->running = true;
        {
            std::lock_guard wl(s->worker_mu);
            s->worker_busy = true;
        }
        s->state = kStateStarted;
        Workers().Post([s](GuestThread* t) { RunCallbacks(s, t); });
        return kOk;
    }
    const HRESULT hr = s->client->Start();
    if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
        s->state = kStateDisconnected;
        return kErrDisconnected;
    }
    s->state = kStateStarted;
    return kOk;
}

// Stops the stream (pause and stop differ only in the state they leave).
s32 Halt(Stream* s, s32 final_state) {
    if (tls_worker_stream == s) {  // from its own callback: the worker stops the client on its way out
        s->running = false;
        s->state = final_state;
        return kOk;
    }
    std::lock_guard lock(s->ctl);
    s->running = false;
    SetEvent(s->event);
    WaitWorker(s);
    s->client->Stop();
    if (s->state != kStateDisconnected)
        s->state = final_state;
    if (s->capture && final_state == kStateStopped) {
        std::lock_guard io(s->io);
        DrainCapture(s);
        s->pending.clear();
        s->pending_off = 0;
    }
    return kOk;
}

s32 Flush(Stream* s) {
    std::lock_guard lock(s->ctl);
    if (s->capture)
        return kErrIllegalArgument;
    const s32 st = s->state;
    if (st != kStatePaused && st != kStateStopped && st != kStateOpen && st != kStateFlushed)
        return kErrInvalidState;
    s->client->Reset();
    s->state = kStateFlushed;
    return kOk;
}

s32 Close(Stream* s) {
    if (tls_worker_stream == s) {
        s->running = false;
        s->state = kStateClosed;
        std::lock_guard lock(s->worker_mu);
        s->free_on_exit = true;
        return kOk;
    }
    Halt(s, kStateStopped);
    {
        std::lock_guard io(s->io);  // a blocking read/write in progress times out
    }
    s->state = kStateClosed;
    delete s;
    return kOk;
}

// --- blocking read / write -------------------------------------------------------------------------

void WaitIo(Stream* s, u64 deadline) {
    const u64 now = MonotonicNs();
    WaitForSingleObject(s->event, static_cast<DWORD>(std::min<u64>((deadline - now) / 1000000 + 1, 20)));
}

s64 Write(Stream* s, const u8* src, s64 frames, s64 timeout_ns) {
    if (s->capture || s->c.data_cb)
        return kErrInvalidState;
    if (s->state == kStateDisconnected)
        return kErrDisconnected;
    std::lock_guard lock(s->io);
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s64 done = 0;
    while (done < frames) {
        UINT32 padding = 0;
        BYTE* data = nullptr;
        if (FAILED(s->client->GetCurrentPadding(&padding))) {
            s->state = kStateDisconnected;
            return done ? done : kErrDisconnected;
        }
        const u32 limit = WriteLimit(s);
        if (padding < limit) {
            const u32 n = static_cast<u32>(std::min<s64>(limit - padding, frames - done));
            if (FAILED(s->render->GetBuffer(n, &data))) {
                s->state = kStateDisconnected;
                return done ? done : kErrDisconnected;
            }
            memcpy(data, src + done * s->frame_bytes, static_cast<size_t>(n) * s->frame_bytes);
            s->render->ReleaseBuffer(n, 0);
            done += n;
            s->frames_written += n;
            continue;
        }
        if (timeout_ns <= 0 || MonotonicNs() >= deadline || s->state != kStateStarted)
            break;
        WaitIo(s, deadline);
    }
    return done;
}

s64 Read(Stream* s, u8* dst, s64 frames, s64 timeout_ns) {
    if (!s->capture || s->c.data_cb)
        return kErrInvalidState;
    if (s->state == kStateDisconnected)
        return kErrDisconnected;
    std::lock_guard lock(s->io);
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s64 done = 0;
    while (done < frames) {
        if (!DrainCapture(s)) {
            s->state = kStateDisconnected;
            return done ? done : kErrDisconnected;
        }
        const s64 have = static_cast<s64>(s->PendingBytes() / s->frame_bytes);
        if (have) {
            const s64 n = std::min(have, frames - done);
            const size_t bytes = static_cast<size_t>(n) * s->frame_bytes;
            memcpy(dst + done * s->frame_bytes, s->pending.data() + s->pending_off, bytes);
            s->pending_off += bytes;
            done += n;
            s->frames_read += n;
            continue;
        }
        if (timeout_ns <= 0 || MonotonicNs() >= deadline || s->state != kStateStarted)
            break;
        WaitIo(s, deadline);
    }
    CompactPending(s);
    return done;
}

// Frames the device has played (output) or captured (input) so far, and when (CLOCK_MONOTONIC ns).
void DevicePosition(Stream* s, s64* frames, s64* ns) {
    UINT64 freq = 0, pos = 0, qpc = 0;
    if (s->clock && SUCCEEDED(s->clock->GetFrequency(&freq)) && freq &&
        SUCCEEDED(s->clock->GetPosition(&pos, &qpc))) {
        *frames = static_cast<s64>(static_cast<double>(pos) * s->c.sample_rate / static_cast<double>(freq));
        *ns = static_cast<s64>(qpc * 100);  // QPC time in 100 ns units, MonotonicNs's clock
        return;
    }
    UINT32 padding = 0;
    if (!s->capture)
        s->client->GetCurrentPadding(&padding);
    *frames = s->capture ? s->frames_read.load() : s->frames_written.load() - padding;
    *ns = static_cast<s64>(MonotonicNs());
}

// --- libaaudio.so ----------------------------------------------------------------------------------

Builder* AsBuilder(Builder* b) { return b && b->magic == kBuilderMagic ? b : nullptr; }

s32 A_createStreamBuilder(u64 out) {
    u64 p = reinterpret_cast<u64>(new Builder);
    SafeCopyToGuest(out, &p, 8);
    return kOk;
}
s32 A_builderDelete(Builder* b) {
    if (!AsBuilder(b))
        return kErrInvalidHandle;
    b->magic = 0;
    delete b;
    return kOk;
}
s32 A_openStream(Builder* b, u64 out) {
    if (!AsBuilder(b))
        return kErrInvalidHandle;
    Stream* s = nullptr;
    const s32 r = OpenStream(b->c, &s);
    u64 p = reinterpret_cast<u64>(s);
    SafeCopyToGuest(out, &p, 8);
    return r;
}
template <s32 Config::*F>
void A_set(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.*F = v;
}
void A_setPrivacy(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.privacy = v & 0xff ? 1 : 0;
}
void A_setSpatialized(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.spatialized = v & 0xff ? 1 : 0;
}
void A_setChannelMask(Builder* b, u32 mask) {
    if (AsBuilder(b)) {
        b->c.channel_mask = mask;
        b->c.channels = std::popcount(mask);
    }
}
void A_setDataCallback(Builder* b, u64 fn, u64 user) {
    if (AsBuilder(b)) {
        b->c.data_cb = fn;
        b->c.data_user = user;
    }
}
void A_setErrorCallback(Builder* b, u64 fn, u64 user) {
    if (AsBuilder(b)) {
        b->c.error_cb = fn;
        b->c.error_user = user;
    }
}
void A_ignore(Builder*, u64) {}

template <s32 Config::*F>
s32 A_get(Stream* s) {
    return AsStream(s) ? s->c.*F : kErrInvalidHandle;
}
s32 A_getChannelMask(Stream* s) {
    if (!AsStream(s))
        return 0;
    if (s->c.channel_mask)
        return static_cast<s32>(s->c.channel_mask);
    return s->c.channels == 1 ? 1 : static_cast<s32>((1u << s->c.channels) - 1);
}
s32 A_requestStart(Stream* s) { return AsStream(s) ? Start(s) : kErrInvalidHandle; }
s32 A_requestStop(Stream* s) { return AsStream(s) ? Halt(s, kStateStopped) : kErrInvalidHandle; }
s32 A_requestPause(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return s->capture ? kErrIllegalArgument : Halt(s, kStatePaused);
}
s32 A_requestFlush(Stream* s) { return AsStream(s) ? Flush(s) : kErrInvalidHandle; }
s32 A_release(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    Halt(s, kStateStopped);
    s->state = kStateClosing;
    return kOk;
}
s32 A_close(Stream* s) { return AsStream(s) ? Close(s) : kErrInvalidHandle; }
s32 A_getState(Stream* s) { return AsStream(s) ? s->state.load() : kStateClosed; }
s32 A_waitForStateChange(Stream* s, s32 input, u64 next, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s32 st;
    while ((st = s->state) == input && MonotonicNs() < deadline)
        Sleep(1);
    if (next)
        SafeCopyToGuest(next, &st, 4);
    return st == input ? kErrTimeout : kOk;
}
s32 A_read(Stream* s, u64 buffer, s32 frames, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return static_cast<s32>(Read(s, reinterpret_cast<u8*>(buffer), frames, timeout_ns));
}
s32 A_write(Stream* s, u64 buffer, s32 frames, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return static_cast<s32>(Write(s, reinterpret_cast<const u8*>(buffer), frames, timeout_ns));
}
s32 A_getBufferSize(Stream* s) { return AsStream(s) ? static_cast<s32>(s->buffer_size.load()) : kErrInvalidHandle; }
s32 A_setBufferSize(Stream* s, s32 frames) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    const u32 lo = std::min(s->burst * 2, s->device_frames);
    s->buffer_size = std::clamp<u32>(static_cast<u32>(std::max(frames, 0)), lo, s->device_frames);
    return static_cast<s32>(s->buffer_size.load());
}
s32 A_getBufferCapacity(Stream* s) { return AsStream(s) ? static_cast<s32>(s->device_frames) : kErrInvalidHandle; }
s32 A_getFramesPerBurst(Stream* s) { return AsStream(s) ? static_cast<s32>(s->burst) : kErrInvalidHandle; }
s32 A_getXRunCount(Stream* s) { return AsStream(s) ? s->xruns.load() : kErrInvalidHandle; }
s32 A_getHardwareSampleRate(Stream*) { return MixRate(); }
s32 A_getHardwareFormat(Stream*) { return kFmtFloat; }
s32 A_isMMapUsed(Stream*) { return 0; }
s64 A_getFramesWritten(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (!s->capture)
        return s->frames_written;
    std::lock_guard lock(s->io);
    return s->frames_read + static_cast<s64>(s->PendingBytes() / s->frame_bytes);
}
s64 A_getFramesRead(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (s->capture)
        return s->frames_read;
    s64 frames, ns;
    DevicePosition(s, &frames, &ns);
    return std::min(frames, s->frames_written.load());
}
s32 A_getTimestamp(Stream* s, s32 /*clockid*/, u64 frame_pos, u64 time_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (s->state != kStateStarted)
        return kErrInvalidState;
    s64 frames, ns;
    DevicePosition(s, &frames, &ns);
    if (frame_pos)
        SafeCopyToGuest(frame_pos, &frames, 8);
    if (time_ns)
        SafeCopyToGuest(time_ns, &ns, 8);
    return kOk;
}

const char* A_resultText(s32 r) {
    switch (r) {
    case kOk:
        return "AAUDIO_OK";
    case kErrDisconnected:
        return "AAUDIO_ERROR_DISCONNECTED";
    case kErrIllegalArgument:
        return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case kErrInvalidState:
        return "AAUDIO_ERROR_INVALID_STATE";
    case kErrInvalidHandle:
        return "AAUDIO_ERROR_INVALID_HANDLE";
    case kErrUnavailable:
        return "AAUDIO_ERROR_UNAVAILABLE";
    case kErrTimeout:
        return "AAUDIO_ERROR_TIMEOUT";
    case kErrInvalidFormat:
        return "AAUDIO_ERROR_INVALID_FORMAT";
    case kErrInvalidRate:
        return "AAUDIO_ERROR_INVALID_RATE";
    default:
        return "AAUDIO_ERROR_UNKNOWN";
    }
}
const char* A_stateText(s32 st) {
    static const char* const kNames[] = {
        "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN",  "AAUDIO_STREAM_STATE_OPEN",
        "AAUDIO_STREAM_STATE_STARTING",      "AAUDIO_STREAM_STATE_STARTED",  "AAUDIO_STREAM_STATE_PAUSING",
        "AAUDIO_STREAM_STATE_PAUSED",        "AAUDIO_STREAM_STATE_FLUSHING", "AAUDIO_STREAM_STATE_FLUSHED",
        "AAUDIO_STREAM_STATE_STOPPING",      "AAUDIO_STREAM_STATE_STOPPED",  "AAUDIO_STREAM_STATE_CLOSING",
        "AAUDIO_STREAM_STATE_CLOSED",        "AAUDIO_STREAM_STATE_DISCONNECTED",
    };
    return st >= 0 && st < 14 ? kNames[st] : "AAUDIO_STREAM_STATE_UNKNOWN";
}
s32 A_getMMapPolicy() { return 1; }  // AAUDIO_POLICY_NEVER
s32 A_setMMapPolicy(s32) { return kOk; }

template <auto F>
ThunkFn W() {
    return &Wrap<F>;
}

// --- libOpenSLES.so --------------------------------------------------------------------------------
//
// An OpenSL ES interface (SLObjectItf, SLPlayItf, ...) is the address of a slot that holds the
// address of a function table. Every object gets a guest page of slots, one per interface kind,
// pointing at shared tables of stubs (guest/lists/extra/libOpenSLES.txt), so a slot address names
// both the object and the interface. A player plays its buffer queue through a blocking-write
// stream on an adopted guest thread, which also runs the app's buffer-queue callback after each
// buffer. Meta's audio SDK opens its Oboe streams with OpenSL ES, and its Oboe (1.7) deadlocks
// while holding Wwise's main lock when slCreateEngine fails.

enum : u32 {
    kSlOk = 0,
    kSlPreconditionsViolated = 1,
    kSlParameterInvalid = 2,
    kSlResourceError = 4,
    kSlBufferInsufficient = 7,
    kSlContentUnsupported = 9,
    kSlFeatureUnsupported = 12,
};
enum : u32 { kSlUnrealized = 1, kSlRealized = 2 };
enum : u32 { kSlStopped = 1, kSlPaused = 2, kSlPlaying = 3 };
constexpr u32 kSlLocatorOutputMix = 4, kSlLocatorBufferQueue = 6, kSlLocatorAndroidSimpleBufferQueue = 0x800007BD;
constexpr u32 kSlFormatPcm = 2, kSlFormatPcmEx = 4, kSlReprFloat = 3;
constexpr u32 kSlTimeUnknown = 0xFFFFFFFF;

// Interface kinds, in slot order.
enum SlItf : u32 { kItfObject, kItfEngine, kItfPlay, kItfBufferQueue, kItfVolume, kItfConfig, kItfCount };

// An interface id's 16 bytes are its name (gen_thunks.py iid_object).
std::string SlIdName(u64 iid) {
    char tag[17] = {};
    if (!iid || !SafeCopyFromGuest(tag, iid, 16))
        return "?";
    return tag;
}

int SlItfFromId(u64 iid) {
    static const std::pair<const char*, SlItf> kIds[] = {
        {"OBJECT", kItfObject},
        {"ENGINE", kItfEngine},
        {"PLAY", kItfPlay},
        {"ANDROIDSIMPLEBUF", kItfBufferQueue},  // SLAndroidSimpleBufferQueueItf and SLBufferQueueItf
        {"BUFFERQUEUE", kItfBufferQueue},       // share one layout
        {"VOLUME", kItfVolume},
        {"ANDROIDCONFIGURA", kItfConfig},
    };
    const std::string name = SlIdName(iid);
    for (const auto& [n, i] : kIds)
        if (name == n)
            return i;
    return -1;
}

struct SlObject {
    enum Kind { kEngine, kOutputMix, kPlayer } kind = kEngine;
    u64 page = 0;
    bool has[kItfCount] = {};
    u32 state = kSlUnrealized;
    // Player
    Config cfg;
    u32 queue_cap = 0;
    Stream* stream = nullptr;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<u64, u32>> queue;  // buffers (address, bytes)
    u32 front_off = 0;                      // bytes of the front buffer already played
    u32 played = 0;                         // buffers completed since the last Clear
    u64 clear_gen = 0;
    u32 play_state = kSlStopped;
    s64 pos_base = 0;  // device frames at the last stop (positions count from there)
    u64 bq_cb = 0, bq_ctx = 0;
    u64 play_cb = 0, play_ctx = 0;
    u32 play_events = 0, marker = kSlTimeUnknown, update_period = 1000;
    bool worker_busy = false, free_on_exit = false;
    s16 volume_mb = 0, stereo_pos = 0;
    u32 mute = 0, stereo_on = 0;
    u32 perf_mode = 0 /* SL_ANDROID_PERFORMANCE_NONE */, stream_type = 3 /* SL_ANDROID_STREAM_MEDIA */;

    u64 Itf(SlItf i) const { return page + i * 8; }
};

std::mutex g_sl_mu;
std::map<u64, SlObject*> g_sl_objects;  // by slot page
u64 g_sl_tables[kItfCount] = {};
thread_local SlObject* tls_sl_player = nullptr;

SlObject* SlFrom(u64 itf, SlItf kind) {
    std::lock_guard lock(g_sl_mu);
    auto it = g_sl_objects.find(itf & ~(kPageSize - 1));
    if (it == g_sl_objects.end() || (itf & (kPageSize - 1)) != kind * 8u)
        return nullptr;
    return it->second;
}

template <typename T>
void SlPut(u64 addr, T v) {
    if (addr)
        SafeCopyToGuest(addr, &v, sizeof(T));
}

// Fails when a required interface is one this object doesn't have.
u32 SlCheckInterfaces(const bool (&has)[kItfCount], u32 n, u64 ids, u64 required) {
    for (u32 i = 0; i < n; ++i) {
        u64 iid = 0;
        u32 req = 0;
        SafeCopyFromGuest(&iid, ids + i * 8, 8);
        if (required)
            SafeCopyFromGuest(&req, required + i * 4, 4);
        const int k = SlItfFromId(iid);
        if ((k < 0 || !has[k]) && req) {
            Log("OpenSL ES: required interface %s is not supported", SlIdName(iid).c_str());
            return kSlFeatureUnsupported;
        }
    }
    return kSlOk;
}

void SlApplyVolume(SlObject* o) {
    IAudioStreamVolume* vol = nullptr;
    if (!o->stream || FAILED(o->stream->client->GetService(__uuidof(IAudioStreamVolume),
                                                           reinterpret_cast<void**>(&vol))))
        return;
    const float gain = o->mute ? 0.0f : static_cast<float>(std::pow(10.0, o->volume_mb / 2000.0));
    UINT32 n = 0;
    vol->GetChannelCount(&n);
    std::vector<float> levels(n, std::min(gain, 1.0f));
    if (n)
        vol->SetAllVolumes(n, levels.data());
    vol->Release();
}

void RunPlayer(SlObject* o, GuestThread* t) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    tls_sl_player = o;
    Stream* s = o->stream;
    std::unique_lock lock(o->mu);
    while (o->play_state == kSlPlaying) {
        if (o->queue.empty()) {
            o->cv.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }
        const auto [buf, size] = o->queue.front();
        const u32 off = o->front_off;
        const u64 gen = o->clear_gen;
        lock.unlock();
        const s64 frames = (size - off) / s->frame_bytes;
        const s64 n = frames > 0 ? Write(s, GuestPtr<const u8>(buf + off), frames, kForever) : 0;
        lock.lock();
        if (n < 0) {
            Log("OpenSL ES: playback device lost");
            break;
        }
        if (gen != o->clear_gen)
            continue;  // Clear() dropped the buffer meanwhile
        o->front_off += static_cast<u32>(n) * s->frame_bytes;
        if (n < frames)
            continue;  // paused or stopped part way: the loop condition decides
        o->queue.pop_front();
        o->front_off = 0;
        ++o->played;
        if (const u64 cb = o->bq_cb) {
            const u64 ctx = o->bq_ctx;
            lock.unlock();
            CallGuest(t, cb, {o->Itf(kItfBufferQueue), ctx});
            lock.lock();
        }
    }
    o->worker_busy = false;
    const bool free_it = o->free_on_exit;
    o->cv.notify_all();
    lock.unlock();
    tls_sl_player = nullptr;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (free_it) {
        Close(o->stream);
        delete o;
    }
}

u32 SlSetPlayState(SlObject* o, u32 st) {
    std::unique_lock lock(o->mu);
    if (o->play_state == st)
        return kSlOk;
    o->play_state = st;
    if (st == kSlPlaying) {
        lock.unlock();
        Start(o->stream);
        lock.lock();
        if (!o->worker_busy) {
            o->worker_busy = true;
            Workers().Post([o](GuestThread* t) { RunPlayer(o, t); });
        }
        o->cv.notify_all();
        return kSlOk;
    }
    // Like Android, this doesn't wait for a callback in progress (it may be waiting for a lock the
    // caller holds); Destroy does.
    o->cv.notify_all();
    lock.unlock();
    Halt(o->stream, st == kSlPaused ? kStatePaused : kStateStopped);  // ends the worker's blocking write
    lock.lock();
    if (st == kSlStopped) {
        s64 frames = 0, ns = 0;
        DevicePosition(o->stream, &frames, &ns);
        o->pos_base = frames;
    }
    return kSlOk;
}

SlObject* SlNewObject(SlObject::Kind kind, std::initializer_list<SlItf> itfs) {
    const s64 page = Mem().Map(0, kPageSize, lx::PROT_READ_ | lx::PROT_WRITE_, lx::MAP_PRIVATE_ | lx::MAP_ANONYMOUS_,
                               nullptr, 0, "[anon:opensles]");
    if (page < 0)
        return nullptr;
    auto* o = new SlObject;
    o->kind = kind;
    o->page = static_cast<u64>(page);
    for (SlItf i : itfs) {
        o->has[i] = true;
        SlPut(o->Itf(i), g_sl_tables[i]);
    }
    std::lock_guard lock(g_sl_mu);
    g_sl_objects[o->page] = o;
    return o;
}

// SLObjectItf
u32 O_Realize(u64 self, u32 /*async: realized synchronously*/) {
    SlObject* o = SlFrom(self, kItfObject);
    if (!o)
        return kSlParameterInvalid;
    if (o->state == kSlRealized)
        return kSlPreconditionsViolated;
    if (o->kind == SlObject::kPlayer && OpenStream(o->cfg, &o->stream) != kOk)
        return kSlResourceError;
    if (o->stream)
        SlApplyVolume(o);
    o->state = kSlRealized;
    return kSlOk;
}
u32 O_Resume(u64 self, u32) { return SlFrom(self, kItfObject) ? kSlOk : kSlParameterInvalid; }
u32 O_GetState(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfObject);
    if (!o || !out)
        return kSlParameterInvalid;
    SlPut(out, o->state);
    return kSlOk;
}
u32 O_GetInterface(u64 self, u64 iid, u64 out) {
    SlObject* o = SlFrom(self, kItfObject);
    if (!o || !out)
        return kSlParameterInvalid;
    const int k = SlItfFromId(iid);
    if (k < 0 || !o->has[k]) {
        SlPut<u64>(out, 0);
        return kSlFeatureUnsupported;
    }
    if (o->state != kSlRealized && k != kItfConfig)  // Android configures players before Realize
        return kSlPreconditionsViolated;
    SlPut(out, o->Itf(static_cast<SlItf>(k)));
    return kSlOk;
}
u32 O_RegisterCallback(u64 self, u64, u64) { return SlFrom(self, kItfObject) ? kSlOk : kSlParameterInvalid; }
void O_AbortAsyncOperation(u64) {}
void O_Destroy(u64 self) {
    SlObject* o = SlFrom(self, kItfObject);
    if (!o)
        return;
    {
        std::lock_guard lock(g_sl_mu);
        g_sl_objects.erase(o->page);
    }
    bool worker_frees = false;
    if (o->stream) {
        if (tls_sl_player == o) {  // from its own callback: the worker closes it on the way out
            std::lock_guard lock(o->mu);
            o->play_state = kSlStopped;
            o->free_on_exit = worker_frees = true;
        } else {
            SlSetPlayState(o, kSlStopped);
            std::unique_lock lock(o->mu);
            o->cv.wait(lock, [&] { return !o->worker_busy; });
        }
    }
    Mem().Unmap(o->page, kPageSize);
    if (worker_frees)
        return;
    if (o->stream)
        Close(o->stream);
    delete o;
}
u32 O_SetPriority(u64, s32, u32) { return kSlOk; }
u32 O_GetPriority(u64, u64 prio, u64 preemptable) {
    SlPut<s32>(prio, 0x7FFFFFFF);  // SL_PRIORITY_NORMAL
    SlPut<u32>(preemptable, 1);
    return kSlOk;
}
u32 O_SetLossOfControlInterfaces(u64, s16, u64, u32) { return kSlOk; }

// SLEngineItf
u32 E_CreateOutputMix(u64 self, u64 out, u32 n, u64 ids, u64 required) {
    if (!SlFrom(self, kItfEngine) || !out)
        return kSlParameterInvalid;
    const bool has[kItfCount] = {true};
    if (const u32 r = SlCheckInterfaces(has, n, ids, required))
        return r;
    SlObject* o = SlNewObject(SlObject::kOutputMix, {kItfObject});
    if (!o)
        return kSlResourceError;
    SlPut(out, o->Itf(kItfObject));
    return kSlOk;
}

u32 E_CreateAudioPlayer(u64 self, u64 out, u64 src, u64 sink, u32 n, u64 ids, u64 required) {
    if (!SlFrom(self, kItfEngine) || !out || !src || !sink)
        return kSlParameterInvalid;
    u64 src_loc = 0, src_fmt = 0, sink_loc = 0;
    SafeCopyFromGuest(&src_loc, src, 8);
    SafeCopyFromGuest(&src_fmt, src + 8, 8);
    SafeCopyFromGuest(&sink_loc, sink, 8);
    u32 loc[2] = {}, sink_type = 0, fmt[8] = {};
    if (!src_loc || !src_fmt || !sink_loc || !SafeCopyFromGuest(loc, src_loc, 8) ||
        !SafeCopyFromGuest(&sink_type, sink_loc, 4) || !SafeCopyFromGuest(fmt, src_fmt, 28))
        return kSlParameterInvalid;
    if ((loc[0] != kSlLocatorAndroidSimpleBufferQueue && loc[0] != kSlLocatorBufferQueue) ||
        sink_type != kSlLocatorOutputMix) {
        Log("OpenSL ES: unsupported player (source locator 0x%x, sink locator 0x%x)", loc[0], sink_type);
        return kSlContentUnsupported;
    }
    // SLDataFormat_PCM / SLAndroidDataFormat_PCM_EX: type, channels, rate (mHz), bits, container,
    // channel mask, endianness[, representation].
    if (fmt[0] == kSlFormatPcmEx)
        SafeCopyFromGuest(&fmt[7], src_fmt + 28, 4);
    Config c;
    c.direction = kDirOutput;
    c.channels = static_cast<s32>(fmt[1]);
    c.sample_rate = static_cast<s32>(fmt[2] / 1000);
    if (fmt[0] != kSlFormatPcm && fmt[0] != kSlFormatPcmEx)
        c.format = kFmtUnspecified;
    else if (fmt[3] == 16)
        c.format = kFmtI16;
    else if (fmt[3] == 24 && fmt[4] == 24)
        c.format = kFmtI24;
    else if (fmt[3] == 32)
        c.format = fmt[0] == kSlFormatPcmEx && fmt[7] == kSlReprFloat ? kFmtFloat : kFmtI32;
    else
        c.format = kFmtUnspecified;
    if (c.format == kFmtUnspecified || c.channels < 1 || c.channels > 8 || c.sample_rate <= 0) {
        Log("OpenSL ES: unsupported PCM format (type %u, %u ch, %u mHz, %u/%u bits)", fmt[0], fmt[1], fmt[2],
            fmt[3], fmt[4]);
        return kSlContentUnsupported;
    }
    const bool has[kItfCount] = {true, false, true, true, true, true};
    if (const u32 r = SlCheckInterfaces(has, n, ids, required))
        return r;
    SlObject* o = SlNewObject(SlObject::kPlayer, {kItfObject, kItfPlay, kItfBufferQueue, kItfVolume, kItfConfig});
    if (!o)
        return kSlResourceError;
    o->cfg = c;
    o->queue_cap = std::max(loc[1], 1u);
    SlPut(out, o->Itf(kItfObject));
    return kSlOk;
}

u32 E_CreateAudioRecorder(u64, u64, u64, u64, u32, u64, u64) {
    Log("OpenSL ES: audio recorders are not supported");
    return kSlFeatureUnsupported;
}
u32 E_Unsupported() { return kSlFeatureUnsupported; }
u32 E_QueryNumSupportedInterfaces(u64, u32, u64 out) {
    SlPut<u32>(out, 0);
    return kSlOk;
}
u32 E_QueryNumSupportedExtensions(u64, u64 out) {
    SlPut<u32>(out, 0);
    return kSlOk;
}
u32 E_QuerySupportedInterfaces() { return kSlParameterInvalid; }
u32 E_IsExtensionSupported(u64, u64, u64 out) {
    SlPut<u32>(out, 0);
    return kSlOk;
}

// SLPlayItf
u32 P_SetPlayState(u64 self, u32 st) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o || st < kSlStopped || st > kSlPlaying)
        return kSlParameterInvalid;
    if (!o->stream)
        return kSlPreconditionsViolated;
    return SlSetPlayState(o, st);
}
u32 P_GetPlayState(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o || !out)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    SlPut(out, o->play_state);
    return kSlOk;
}
u32 P_GetDuration(u64, u64 out) {
    SlPut(out, kSlTimeUnknown);
    return kSlOk;
}
u32 P_GetPosition(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o || !out)
        return kSlParameterInvalid;
    if (!o->stream) {
        SlPut<u32>(out, 0);
        return kSlOk;
    }
    s64 frames = 0, ns = 0;
    DevicePosition(o->stream, &frames, &ns);
    const s64 played = std::max<s64>(frames - o->pos_base, 0);
    SlPut(out, static_cast<u32>(played * 1000 / o->cfg.sample_rate));
    return kSlOk;
}
u32 P_RegisterCallback(u64 self, u64 cb, u64 ctx) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    o->play_cb = cb;  // play events (markers, head at end) are never raised
    o->play_ctx = ctx;
    return kSlOk;
}
template <u32 SlObject::*F>
u32 P_SetU32(u64 self, u32 v) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o)
        return kSlParameterInvalid;
    o->*F = v;
    return kSlOk;
}
template <u32 SlObject::*F>
u32 P_GetU32(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o || !out)
        return kSlParameterInvalid;
    SlPut(out, o->*F);
    return kSlOk;
}
u32 P_ClearMarkerPosition(u64 self) { return P_SetU32<&SlObject::marker>(self, kSlTimeUnknown); }
u32 P_GetMarkerPosition(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfPlay);
    if (!o || !out)
        return kSlParameterInvalid;
    if (o->marker == kSlTimeUnknown)
        return kSlPreconditionsViolated;
    SlPut(out, o->marker);
    return kSlOk;
}

// SLAndroidSimpleBufferQueueItf / SLBufferQueueItf
u32 B_Enqueue(u64 self, u64 buf, u32 size) {
    SlObject* o = SlFrom(self, kItfBufferQueue);
    if (!o || !buf || !size)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    if (o->queue.size() >= o->queue_cap)
        return kSlBufferInsufficient;
    o->queue.emplace_back(buf, size);
    o->cv.notify_all();
    return kSlOk;
}
u32 B_Clear(u64 self) {
    SlObject* o = SlFrom(self, kItfBufferQueue);
    if (!o)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    o->queue.clear();
    o->front_off = 0;
    o->played = 0;
    ++o->clear_gen;
    return kSlOk;
}
u32 B_GetState(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfBufferQueue);
    if (!o || !out)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    const u32 st[2] = {static_cast<u32>(o->queue.size()), o->played};  // count, index
    SafeCopyToGuest(out, st, sizeof(st));
    return kSlOk;
}
u32 B_RegisterCallback(u64 self, u64 cb, u64 ctx) {
    SlObject* o = SlFrom(self, kItfBufferQueue);
    if (!o)
        return kSlParameterInvalid;
    std::lock_guard lock(o->mu);
    o->bq_cb = cb;
    o->bq_ctx = ctx;
    return kSlOk;
}

// SLVolumeItf
u32 V_SetVolumeLevel(u64 self, s16 mb) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o || mb > 0)
        return kSlParameterInvalid;
    o->volume_mb = mb;
    SlApplyVolume(o);
    return kSlOk;
}
u32 V_GetVolumeLevel(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o || !out)
        return kSlParameterInvalid;
    SlPut(out, o->volume_mb);
    return kSlOk;
}
u32 V_GetMaxVolumeLevel(u64, u64 out) {
    SlPut<s16>(out, 0);
    return kSlOk;
}
u32 V_SetMute(u64 self, u32 mute) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o)
        return kSlParameterInvalid;
    o->mute = mute != 0;
    SlApplyVolume(o);
    return kSlOk;
}
template <u32 SlObject::*F>
u32 V_GetU32(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o || !out)
        return kSlParameterInvalid;
    SlPut(out, o->*F);
    return kSlOk;
}
u32 V_EnableStereoPosition(u64 self, u32 on) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o)
        return kSlParameterInvalid;
    o->stereo_on = on != 0;  // recorded, not applied
    return kSlOk;
}
u32 V_SetStereoPosition(u64 self, s16 permille) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o)
        return kSlParameterInvalid;
    o->stereo_pos = permille;
    return kSlOk;
}
u32 V_GetStereoPosition(u64 self, u64 out) {
    SlObject* o = SlFrom(self, kItfVolume);
    if (!o || !out)
        return kSlParameterInvalid;
    SlPut(out, o->stereo_pos);
    return kSlOk;
}

// SLAndroidConfigurationItf: the stream type and performance mode keys.
u32* SlConfigValue(SlObject* o, u64 key) {
    std::string k;
    SafeReadString(key, k, 64);
    if (k == "androidPerformanceMode")
        return &o->perf_mode;
    if (k == "androidPlaybackStreamType")
        return &o->stream_type;
    return nullptr;
}
u32 C_SetConfiguration(u64 self, u64 key, u64 value, u32 size) {
    SlObject* o = SlFrom(self, kItfConfig);
    u32* v = o ? SlConfigValue(o, key) : nullptr;
    if (!v || !value || size < 4)
        return kSlParameterInvalid;
    SafeCopyFromGuest(v, value, 4);
    return kSlOk;
}
u32 C_GetConfiguration(u64 self, u64 key, u64 size, u64 value) {
    SlObject* o = SlFrom(self, kItfConfig);
    u32* v = o ? SlConfigValue(o, key) : nullptr;
    if (!v || !size)
        return kSlParameterInvalid;
    u32 have = 0;
    SafeCopyFromGuest(&have, size, 4);
    SlPut<u32>(size, 4);
    if (!value)
        return kSlOk;
    if (have < 4)
        return kSlParameterInvalid;
    SlPut(value, *v);
    return kSlOk;
}

// The interface tables, in each interface's function order (OpenSLES.h, OpenSLES_Android.h).
struct SlMethod {
    const char* name;
    ThunkFn fn;
};
struct SlTable {
    SlItf itf;
    const char* prefix;
    std::vector<SlMethod> methods;
};
const std::vector<SlTable>& SlTables() {
    static const std::vector<SlTable> tables = {
        {kItfObject,
         "Object",
         {{"Realize", W<&O_Realize>()},
          {"Resume", W<&O_Resume>()},
          {"GetState", W<&O_GetState>()},
          {"GetInterface", W<&O_GetInterface>()},
          {"RegisterCallback", W<&O_RegisterCallback>()},
          {"AbortAsyncOperation", W<&O_AbortAsyncOperation>()},
          {"Destroy", W<&O_Destroy>()},
          {"SetPriority", W<&O_SetPriority>()},
          {"GetPriority", W<&O_GetPriority>()},
          {"SetLossOfControlInterfaces", W<&O_SetLossOfControlInterfaces>()}}},
        {kItfEngine,
         "Engine",
         {{"CreateLEDDevice", W<&E_Unsupported>()},
          {"CreateVibraDevice", W<&E_Unsupported>()},
          {"CreateAudioPlayer", W<&E_CreateAudioPlayer>()},
          {"CreateAudioRecorder", W<&E_CreateAudioRecorder>()},
          {"CreateMidiPlayer", W<&E_Unsupported>()},
          {"CreateListener", W<&E_Unsupported>()},
          {"Create3DGroup", W<&E_Unsupported>()},
          {"CreateOutputMix", W<&E_CreateOutputMix>()},
          {"CreateMetadataExtractor", W<&E_Unsupported>()},
          {"CreateExtensionObject", W<&E_Unsupported>()},
          {"QueryNumSupportedInterfaces", W<&E_QueryNumSupportedInterfaces>()},
          {"QuerySupportedInterfaces", W<&E_QuerySupportedInterfaces>()},
          {"QueryNumSupportedExtensions", W<&E_QueryNumSupportedExtensions>()},
          {"QuerySupportedExtension", W<&E_QuerySupportedInterfaces>()},
          {"IsExtensionSupported", W<&E_IsExtensionSupported>()}}},
        {kItfPlay,
         "Play",
         {{"SetPlayState", W<&P_SetPlayState>()},
          {"GetPlayState", W<&P_GetPlayState>()},
          {"GetDuration", W<&P_GetDuration>()},
          {"GetPosition", W<&P_GetPosition>()},
          {"RegisterCallback", W<&P_RegisterCallback>()},
          {"SetCallbackEventsMask", W<&P_SetU32<&SlObject::play_events>>()},
          {"GetCallbackEventsMask", W<&P_GetU32<&SlObject::play_events>>()},
          {"SetMarkerPosition", W<&P_SetU32<&SlObject::marker>>()},
          {"ClearMarkerPosition", W<&P_ClearMarkerPosition>()},
          {"GetMarkerPosition", W<&P_GetMarkerPosition>()},
          {"SetPositionUpdatePeriod", W<&P_SetU32<&SlObject::update_period>>()},
          {"GetPositionUpdatePeriod", W<&P_GetU32<&SlObject::update_period>>()}}},
        {kItfBufferQueue,
         "BufferQueue",
         {{"Enqueue", W<&B_Enqueue>()},
          {"Clear", W<&B_Clear>()},
          {"GetState", W<&B_GetState>()},
          {"RegisterCallback", W<&B_RegisterCallback>()}}},
        {kItfVolume,
         "Volume",
         {{"SetVolumeLevel", W<&V_SetVolumeLevel>()},
          {"GetVolumeLevel", W<&V_GetVolumeLevel>()},
          {"GetMaxVolumeLevel", W<&V_GetMaxVolumeLevel>()},
          {"SetMute", W<&V_SetMute>()},
          {"GetMute", W<&V_GetU32<&SlObject::mute>>()},
          {"EnableStereoPosition", W<&V_EnableStereoPosition>()},
          {"IsEnabledStereoPosition", W<&V_GetU32<&SlObject::stereo_on>>()},
          {"SetStereoPosition", W<&V_SetStereoPosition>()},
          {"GetStereoPosition", W<&V_GetStereoPosition>()}}},
        {kItfConfig,
         "Config",
         {{"SetConfiguration", W<&C_SetConfiguration>()},
          {"GetConfiguration", W<&C_GetConfiguration>()},
          {"AcquireJavaProxy", W<&E_Unsupported>()},
          {"ReleaseJavaProxy", W<&E_Unsupported>()}}},
    };
    return tables;
}

std::string SlStubName(const SlTable& t, const SlMethod& m) {
    return std::string("refract_sl_") + t.prefix + "_" + m.name;
}

// Builds the guest function tables (once the stub library has registered its stubs).
bool SlInitTables() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        const s64 page = Mem().Map(0, kPageSize, lx::PROT_READ_ | lx::PROT_WRITE_,
                                   lx::MAP_PRIVATE_ | lx::MAP_ANONYMOUS_, nullptr, 0, "[anon:opensles tables]");
        if (page < 0)
            return;
        u64 at = static_cast<u64>(page);
        for (const SlTable& t : SlTables()) {
            g_sl_tables[t.itf] = at;
            for (const SlMethod& m : t.methods) {
                const u64 stub = StubAddress(kLibOpenSLES, SlStubName(t, m));
                if (!stub) {
                    Log("OpenSL ES: libOpenSLES.so has no %s stub", SlStubName(t, m).c_str());
                    return;
                }
                SlPut(at, stub);
                at += 8;
            }
        }
        Mem().Protect(static_cast<u64>(page), kPageSize, lx::PROT_READ_);
        ok = true;
    });
    return ok;
}

u32 Sl_CreateEngine(u64 out, u32, u64, u32 n, u64 ids, u64 required) {
    if (!out)
        return kSlParameterInvalid;
    if (!SlInitTables())
        return kSlResourceError;
    const bool has[kItfCount] = {true, true};
    if (const u32 r = SlCheckInterfaces(has, n, ids, required))
        return r;
    SlObject* o = SlNewObject(SlObject::kEngine, {kItfObject, kItfEngine});
    if (!o)
        return kSlResourceError;
    SlPut(out, o->Itf(kItfObject));
    return kSlOk;
}
u32 Sl_QueryNumSupportedEngineInterfaces(u64 out) {
    SlPut<u32>(out, 0);
    return kSlOk;
}
u32 Sl_QuerySupportedEngineInterfaces() { return kSlParameterInvalid; }

// --- refract.media.HostAudio (android.media.AudioTrack / AudioRecord) ------------------------------

Stream* JStream(jlong h) { return AsStream(reinterpret_cast<Stream*>(h)); }

jlong JNICALL J_open(JNIEnv*, jclass, jboolean capture, jint rate, jint channels, jint format, jint buffer_frames) {
    Config c;
    c.direction = capture ? kDirInput : kDirOutput;
    c.sample_rate = rate;
    c.channels = channels;
    c.format = format;
    c.capacity = buffer_frames;
    c.input_preset = 7;  // VOICE_COMMUNICATION
    Stream* s = nullptr;
    return OpenStream(c, &s) == kOk ? reinterpret_cast<jlong>(s) : 0;
}
jint JNICALL J_start(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? Start(s) : kErrInvalidHandle;
}
void JNICALL J_stop(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Halt(s, kStateStopped);
}
void JNICALL J_pause(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Halt(s, kStatePaused);
}
void JNICALL J_flush(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Flush(s);
}
void JNICALL J_close(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Close(s);
}
jint JNICALL J_bufferFrames(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? static_cast<jint>(s->device_frames) : 0;
}
jlong JNICALL J_position(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    if (!s)
        return 0;
    if (s->capture)
        return s->frames_read;
    UINT32 padding = 0;
    s->client->GetCurrentPadding(&padding);
    return s->frames_written - padding;
}
jint JNICALL J_underruns(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? s->xruns.load() : 0;
}
jint JNICALL J_mixRate(JNIEnv*, jclass) { return MixRate(); }

// Byte counts in and out; negative results are AAudio errors.
jint JNICALL J_writeArray(JNIEnv* env, jclass, jlong h, jobject array, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    if (!s || !array || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 frames = bytes / s->frame_bytes;
    std::vector<u8> tmp(static_cast<size_t>(frames) * s->frame_bytes);
    void* p = env->GetPrimitiveArrayCritical(static_cast<jarray>(array), nullptr);
    if (!p)
        return kErrIllegalArgument;
    memcpy(tmp.data(), static_cast<u8*>(p) + off, tmp.size());
    env->ReleasePrimitiveArrayCritical(static_cast<jarray>(array), p, JNI_ABORT);
    const s64 n = Write(s, tmp.data(), frames, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_writeBuffer(JNIEnv* env, jclass, jlong h, jobject buf, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    u8* p = buf ? static_cast<u8*>(env->GetDirectBufferAddress(buf)) : nullptr;
    if (!s || !p || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 n = Write(s, p + off, bytes / s->frame_bytes, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_readArray(JNIEnv* env, jclass, jlong h, jobject array, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    if (!s || !array || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    std::vector<u8> tmp(static_cast<size_t>(bytes / s->frame_bytes) * s->frame_bytes);
    const s64 n = Read(s, tmp.data(), bytes / s->frame_bytes, blocking ? kForever : 0);
    if (n <= 0)
        return static_cast<jint>(n);
    void* p = env->GetPrimitiveArrayCritical(static_cast<jarray>(array), nullptr);
    if (!p)
        return kErrIllegalArgument;
    memcpy(static_cast<u8*>(p) + off, tmp.data(), static_cast<size_t>(n) * s->frame_bytes);
    env->ReleasePrimitiveArrayCritical(static_cast<jarray>(array), p, 0);
    return static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_readBuffer(JNIEnv* env, jclass, jlong h, jobject buf, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    u8* p = buf ? static_cast<u8*>(env->GetDirectBufferAddress(buf)) : nullptr;
    if (!s || !p || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 n = Read(s, p + off, bytes / s->frame_bytes, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}

}  // namespace

bool RegisterAudioNatives(JNIEnv* env) {
    jclass c = env->FindClass("refract/media/HostAudio");
    if (!c) {
        env->ExceptionClear();
        Log("refract.media.HostAudio missing: no AudioTrack/AudioRecord");
        return false;
    }
    auto m = [](const char* name, const char* sig, auto fn) {
        return JNINativeMethod{const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)};
    };
    const JNINativeMethod methods[] = {
        m("open", "(ZIIII)J", &J_open),
        m("start", "(J)I", &J_start),
        m("stop", "(J)V", &J_stop),
        m("pause", "(J)V", &J_pause),
        m("flush", "(J)V", &J_flush),
        m("close", "(J)V", &J_close),
        m("bufferFrames", "(J)I", &J_bufferFrames),
        m("position", "(J)J", &J_position),
        m("underruns", "(J)I", &J_underruns),
        m("mixRate", "()I", &J_mixRate),
        m("writeArray", "(JLjava/lang/Object;IIZ)I", &J_writeArray),
        m("writeBuffer", "(JLjava/nio/ByteBuffer;IIZ)I", &J_writeBuffer),
        m("readArray", "(JLjava/lang/Object;IIZ)I", &J_readArray),
        m("readBuffer", "(JLjava/nio/ByteBuffer;IIZ)I", &J_readBuffer),
    };
    return env->RegisterNatives(c, methods, sizeof(methods) / sizeof(methods[0])) == JNI_OK;
}

void RegisterAudioHle() {
    RegisterHostLibrary(
        kLibAAudio, "libaaudio.so",
        {
            {"AAudio_createStreamBuilder", W<&A_createStreamBuilder>()},
            {"AAudioStreamBuilder_delete", W<&A_builderDelete>()},
            {"AAudioStreamBuilder_openStream", W<&A_openStream>()},
            {"AAudioStreamBuilder_setDirection", W<&A_set<&Config::direction>>()},
            {"AAudioStreamBuilder_setSampleRate", W<&A_set<&Config::sample_rate>>()},
            {"AAudioStreamBuilder_setChannelCount", W<&A_set<&Config::channels>>()},
            {"AAudioStreamBuilder_setSamplesPerFrame", W<&A_set<&Config::channels>>()},
            {"AAudioStreamBuilder_setChannelMask", W<&A_setChannelMask>()},
            {"AAudioStreamBuilder_setFormat", W<&A_set<&Config::format>>()},
            {"AAudioStreamBuilder_setSharingMode", W<&A_set<&Config::sharing>>()},
            {"AAudioStreamBuilder_setPerformanceMode", W<&A_set<&Config::perf>>()},
            {"AAudioStreamBuilder_setBufferCapacityInFrames", W<&A_set<&Config::capacity>>()},
            {"AAudioStreamBuilder_setFramesPerDataCallback", W<&A_set<&Config::frames_per_cb>>()},
            {"AAudioStreamBuilder_setDeviceId", W<&A_set<&Config::device_id>>()},
            {"AAudioStreamBuilder_setSessionId", W<&A_set<&Config::session>>()},
            {"AAudioStreamBuilder_setUsage", W<&A_set<&Config::usage>>()},
            {"AAudioStreamBuilder_setContentType", W<&A_set<&Config::content>>()},
            {"AAudioStreamBuilder_setInputPreset", W<&A_set<&Config::input_preset>>()},
            {"AAudioStreamBuilder_setAllowedCapturePolicy", W<&A_set<&Config::allowed_capture>>()},
            {"AAudioStreamBuilder_setSpatializationBehavior", W<&A_set<&Config::spatialization>>()},
            {"AAudioStreamBuilder_setIsContentSpatialized", W<&A_setSpatialized>()},
            {"AAudioStreamBuilder_setPrivacySensitive", W<&A_setPrivacy>()},
            {"AAudioStreamBuilder_setPackageName", W<&A_ignore>()},
            {"AAudioStreamBuilder_setAttributionTag", W<&A_ignore>()},
            {"AAudioStreamBuilder_setDataCallback", W<&A_setDataCallback>()},
            {"AAudioStreamBuilder_setErrorCallback", W<&A_setErrorCallback>()},
            {"AAudioStream_requestStart", W<&A_requestStart>()},
            {"AAudioStream_requestPause", W<&A_requestPause>()},
            {"AAudioStream_requestFlush", W<&A_requestFlush>()},
            {"AAudioStream_requestStop", W<&A_requestStop>()},
            {"AAudioStream_release", W<&A_release>()},
            {"AAudioStream_close", W<&A_close>()},
            {"AAudioStream_getState", W<&A_getState>()},
            {"AAudioStream_waitForStateChange", W<&A_waitForStateChange>()},
            {"AAudioStream_read", W<&A_read>()},
            {"AAudioStream_write", W<&A_write>()},
            {"AAudioStream_getBufferSizeInFrames", W<&A_getBufferSize>()},
            {"AAudioStream_setBufferSizeInFrames", W<&A_setBufferSize>()},
            {"AAudioStream_getBufferCapacityInFrames", W<&A_getBufferCapacity>()},
            {"AAudioStream_getFramesPerBurst", W<&A_getFramesPerBurst>()},
            {"AAudioStream_getFramesPerDataCallback", W<&A_get<&Config::frames_per_cb>>()},
            {"AAudioStream_getXRunCount", W<&A_getXRunCount>()},
            {"AAudioStream_getSampleRate", W<&A_get<&Config::sample_rate>>()},
            {"AAudioStream_getHardwareSampleRate", W<&A_getHardwareSampleRate>()},
            {"AAudioStream_getChannelCount", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getSamplesPerFrame", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getHardwareChannelCount", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getChannelMask", W<&A_getChannelMask>()},
            {"AAudioStream_getFormat", W<&A_get<&Config::format>>()},
            {"AAudioStream_getHardwareFormat", W<&A_getHardwareFormat>()},
            {"AAudioStream_getSharingMode", W<&A_get<&Config::sharing>>()},
            {"AAudioStream_getPerformanceMode", W<&A_get<&Config::perf>>()},
            {"AAudioStream_getDirection", W<&A_get<&Config::direction>>()},
            {"AAudioStream_getDeviceId", W<&A_get<&Config::device_id>>()},
            {"AAudioStream_getSessionId", W<&A_get<&Config::session>>()},
            {"AAudioStream_getUsage", W<&A_get<&Config::usage>>()},
            {"AAudioStream_getContentType", W<&A_get<&Config::content>>()},
            {"AAudioStream_getInputPreset", W<&A_get<&Config::input_preset>>()},
            {"AAudioStream_getAllowedCapturePolicy", W<&A_get<&Config::allowed_capture>>()},
            {"AAudioStream_getSpatializationBehavior", W<&A_get<&Config::spatialization>>()},
            {"AAudioStream_isContentSpatialized", W<&A_get<&Config::spatialized>>()},
            {"AAudioStream_isPrivacySensitive", W<&A_get<&Config::privacy>>()},
            {"AAudioStream_isMMapUsed", W<&A_isMMapUsed>()},
            {"AAudioStream_getFramesWritten", W<&A_getFramesWritten>()},
            {"AAudioStream_getFramesRead", W<&A_getFramesRead>()},
            {"AAudioStream_getTimestamp", W<&A_getTimestamp>()},
            {"AAudio_convertResultToText", W<&A_resultText>()},
            {"AAudio_convertStreamStateToText", W<&A_stateText>()},
            {"AAudio_getMMapPolicy", W<&A_getMMapPolicy>()},
            {"AAudio_setMMapPolicy", W<&A_setMMapPolicy>()},
        });
    std::map<std::string, ThunkFn> sl = {
        {"slCreateEngine", W<&Sl_CreateEngine>()},
        {"slQueryNumSupportedEngineInterfaces", W<&Sl_QueryNumSupportedEngineInterfaces>()},
        {"slQuerySupportedEngineInterfaces", W<&Sl_QuerySupportedEngineInterfaces>()},
    };
    for (const SlTable& t : SlTables())
        for (const SlMethod& m : t.methods)
            sl[SlStubName(t, m)] = m.fn;
    RegisterHostLibrary(kLibOpenSLES, "libOpenSLES.so", std::move(sl));
}

}  // namespace rn

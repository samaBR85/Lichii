// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_common/drivers/renhdrum_pad.h"

#ifdef ENABLE_RENHDRUM

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <renhdrum.h>

#include "common/dynamic_library.h"
#include "common/logging.h"
#include "common/settings.h"

#ifdef RENHDRUM_SDL_HDRUMBLE
// Added by .patch/sdl3-hdrumble/0002-hdrumble-idle-write.patch (SDL3 is linked statically). Each
// returns 1 when the mode is active, 0 when SDL refused it, -1 for a pad that is not a HIDAPI
// Switch pad.
extern "C" {
int SDLCALL SDL_HDRumbleSetNoQueue(SDL_Joystick* joystick, int enable, int gap_ms, int pause_ms);
int SDLCALL SDL_HDRumbleSetNoQueueBluetooth(SDL_Joystick* joystick, int enable, int gap_ms);
int SDLCALL SDL_HDRumbleSetBluetoothAfterInput(SDL_Joystick* joystick, int enable);
}
#endif

namespace InputCommon {

namespace {

// The RenHDrum C ABI, resolved from the shared library at runtime: nothing to link, and a build
// without the library just keeps the standard rumble.
struct RenHDrumApi {
    decltype(&renhdrum_engine_create) engine_create{};
    decltype(&renhdrum_engine_destroy) engine_destroy{};
    decltype(&renhdrum_engine_apply_settings) engine_apply_settings{};
    decltype(&renhdrum_engine_push_half_sample) engine_push_half_sample{};
    decltype(&renhdrum_engine_tick) engine_tick{};
    decltype(&renhdrum_settings_resolve) settings_resolve{};
    decltype(&renhdrum_dualsense_dsp_create) dsp_create{};
    decltype(&renhdrum_dualsense_dsp_destroy) dsp_destroy{};
    decltype(&renhdrum_dualsense_dsp_set_half_target) dsp_set_half_target{};
    decltype(&renhdrum_dualsense_dsp_fill_f32) dsp_fill_f32{};

    Common::DynamicLibrary library;
    bool loaded{};
};

const RenHDrumApi& Api() {
    static const RenHDrumApi api = [] {
        RenHDrumApi a;
#if defined(_WIN32)
        constexpr const char* name = "renhdrum.dll";
#elif defined(__APPLE__)
        constexpr const char* name = "librenhdrum.dylib";
#else
        constexpr const char* name = "librenhdrum.so";
#endif
        a.library = Common::DynamicLibrary(name);
        if (!a.library.IsOpen()) {
            LOG_INFO(Input, "RenHDrum: {} not found, using the standard rumble", name);
            return a;
        }
        a.loaded = a.library.GetSymbol("renhdrum_engine_create", &a.engine_create) &&
                   a.library.GetSymbol("renhdrum_engine_destroy", &a.engine_destroy) &&
                   a.library.GetSymbol("renhdrum_engine_apply_settings", &a.engine_apply_settings) &&
                   a.library.GetSymbol("renhdrum_engine_push_half_sample",
                                       &a.engine_push_half_sample) &&
                   a.library.GetSymbol("renhdrum_engine_tick", &a.engine_tick) &&
                   a.library.GetSymbol("renhdrum_settings_resolve", &a.settings_resolve) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_create", &a.dsp_create) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_destroy", &a.dsp_destroy) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_set_half_target",
                                       &a.dsp_set_half_target) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_fill_f32", &a.dsp_fill_f32);
        if (a.loaded) {
            LOG_INFO(Input, "RenHDrum: loaded {}", name);
        } else {
            LOG_WARNING(Input, "RenHDrum: {} is missing functions, using the standard rumble", name);
        }
        return a;
    }();
    return api;
}

constexpr Uint16 NintendoVendorId = 0x057E;
constexpr Uint16 SonyVendorId = 0x054C;
constexpr std::array<Uint8, 10> NeutralPacket{0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
constexpr int DualSenseAudioMaxAttempts = 10;
// SDL no-queue gap while the 5 ms envelope runs (Ryuhdr AmFmNoQueueGapMs)
[[maybe_unused]] constexpr int NoQueueEnvelopeGapMs = 10;

bool IsSwitchHdController(Uint16 vid, Uint16 pid) {
    return vid == NintendoVendorId && (pid == 0x2009 || pid == 0x2006 || pid == 0x2007);
}

bool IsDualSense(Uint16 vid, Uint16 pid) {
    return vid == SonyVendorId && (pid == 0x0CE6 || pid == 0x0DF2);
}

struct UserSettings {
    bool hd_enabled{true};
    int usb_interval_ms{15};
    bool envelope_5ms{true};
    bool frequency_in_packet{true};
    bool crisp_notes{true};
    int bluetooth_level{3};
    bool bt_after_input{true};
    bool bt_fast{true};
    bool bt_crisp_notes{true};
    bool dualsense_enabled{true};
    int dualsense_volume{100};
    bool dualsense_crisp_notes{true};
    int gain{100};
    int low_boost{0};
    bool operator==(const UserSettings&) const = default;
};

UserSettings ReadUserSettings() {
    const auto& s = Settings::values;
    return {
        .hd_enabled = s.hd_rumble_enabled.GetValue(),
        .usb_interval_ms = std::clamp(s.hd_rumble_usb_interval_ms.GetValue(), 5, 100),
        .envelope_5ms = s.hd_rumble_envelope_5ms.GetValue(),
        .frequency_in_packet = s.hd_rumble_frequency_in_packet.GetValue(),
        .crisp_notes = s.hd_rumble_crisp_notes.GetValue(),
        .bluetooth_level = std::clamp(s.hd_rumble_bluetooth_level.GetValue(), 1, 3),
        .bt_after_input = s.hd_rumble_bt_after_input.GetValue(),
        .bt_fast = s.hd_rumble_bt_fast.GetValue(),
        .bt_crisp_notes = s.hd_rumble_bt_crisp_notes.GetValue(),
        .dualsense_enabled = s.dualsense_haptics_enabled.GetValue(),
        .dualsense_volume = std::clamp(s.dualsense_haptics_volume.GetValue(), 0, 100),
        .dualsense_crisp_notes = s.dualsense_crisp_notes.GetValue(),
        .gain = std::clamp(s.hd_rumble_gain.GetValue(), 25, 1000),
        .low_boost = std::clamp(s.hd_rumble_low_boost.GetValue(), 0, 100),
    };
}

// Gain, then a power curve that lifts weak vibrations more than strong ones (exponent 1.0 at
// boost 0 down to 0.4 at boost 100). Below a small knee the curve turns quadratic, so the
// near-silent values games send between hits do not become a constant buzz.
float ShapeAmplitude(float amplitude, const UserSettings& u) {
    const float x = (std::min)(amplitude * static_cast<float>(u.gain) / 100.0f, 1.0f);
    if (x <= 0.0f || u.low_boost <= 0) {
        return x;
    }
    const float exponent = 1.0f - 0.006f * static_cast<float>(u.low_boost);
    constexpr float knee = 0.02f;
    if (x < knee) {
        const float r = x / knee;
        return std::pow(knee, exponent) * r * r;
    }
    return std::pow(x, exponent);
}

} // namespace

struct RenHDrumPad::Impl {
    SDL_Joystick* joystick{};
    SDL_Gamepad* gamepad{};
    bool is_bluetooth{};

    // Guards everything below. Never held while SDL_SendJoystickEffect runs on the HLE thread.
    mutable std::mutex mutex;
    RenHdcEngine* engine{};
    RenHdcDualSenseDsp* dsp{};
    SDL_AudioStream* audio_stream{};
    bool audio_inited{};
    int audio_attempts{};
    std::int64_t audio_next_us{};
    UserSettings user{};

    // Ryuhdr's recommended mode (0.2.6) with the user's choices from the HD Rumble dialog. The SDL
    // transport modes are switched on where SDL has them; a mode SDL refused is cleared, so the
    // engine never paces for writes that SDL does not gate.
    void ApplyEngineSettings() {
        const auto& api = Api();
        RenHdcConfig config{};
        config.enable_hd_rumble = user.hd_enabled;
        config.hd_rumble_usb_interval_ms = user.usb_interval_ms;
        config.hd_rumble_bluetooth_level = user.bluetooth_level;
        config.hd_rumble_usb_auto_slow = true;
        config.hd_rumble_experimental_usb_no_queue = true;
        config.hd_rumble_experimental_envelope_5ms = user.envelope_5ms;
        config.hd_rumble_experimental_frequency_in_packet = user.frequency_in_packet;
        config.hd_rumble_experimental_frequency_both_bands = user.frequency_in_packet;
        config.hd_rumble_experimental_crisp_notes = user.crisp_notes;
        config.hd_rumble_experimental_bt_after_input = user.bt_after_input;
        config.hd_rumble_experimental_bt_fast = user.bt_fast;
        config.hd_rumble_experimental_bt_crisp_notes = user.bt_crisp_notes;

        RenHdcSettings settings{};
        if (api.settings_resolve(&config, &settings) != RENHDRUM_OK) {
            return;
        }

#ifdef RENHDRUM_SDL_HDRUMBLE
        SDL_HDRumbleSetNoQueue(joystick, 0, 0, 0);
        SDL_HDRumbleSetNoQueueBluetooth(joystick, 0, 0);
        SDL_HDRumbleSetBluetoothAfterInput(joystick, 0);
        if (is_bluetooth) {
            // B3 needs B1 on; SDL never runs both on one pad, B3 wins
            settings.bt_fast =
                settings.enabled && settings.bt_fast && settings.bt_after_input &&
                SDL_HDRumbleSetNoQueueBluetooth(joystick, 1, NoQueueEnvelopeGapMs) == 1;
            settings.bt_after_input = settings.enabled && settings.bt_after_input &&
                                      !settings.bt_fast &&
                                      SDL_HDRumbleSetBluetoothAfterInput(joystick, 1) == 1;
            settings.usb_no_queue = false;
        } else {
            const int gap_ms =
                settings.envelope_5ms ? NoQueueEnvelopeGapMs : settings.usb_interval_ms;
            settings.usb_no_queue =
                settings.enabled && settings.usb_no_queue &&
                SDL_HDRumbleSetNoQueue(joystick, 1, gap_ms, settings.usb_pause_ms) == 1;
            settings.bt_after_input = false;
            settings.bt_fast = false;
        }
#else
        // Stock SDL queues every write; the engine paces by its clock.
        settings.usb_no_queue = false;
        settings.bt_after_input = false;
        settings.bt_fast = false;
#endif
        if (is_bluetooth) {
            LOG_INFO(Input, "RenHDrum HD Rumble Bluetooth: level {}, {}{}", settings.bluetooth_level,
                     settings.bt_fast          ? "5 ms envelope while SDL is idle"
                     : settings.bt_after_input ? "written right after an input report"
                                               : "clock based writes",
                     settings.enabled && settings.bt_crisp_notes ? ", crisp notes" : "");
        } else {
            const bool envelope = settings.usb_no_queue && settings.envelope_5ms;
            LOG_INFO(Input, "RenHDrum HD Rumble wired: {} ms, {}{}{}", settings.usb_interval_ms,
                     settings.usb_no_queue ? "no-queue writes" : "queued writes",
                     envelope ? ", 5 ms envelope" : "",
                     envelope && settings.crisp_notes ? ", crisp notes" : "");
        }
        api.engine_apply_settings(engine, &settings);
    }

    // Caller holds the mutex. Picks up changes made in the HD Rumble dialog.
    void RefreshUserSettings() {
        const UserSettings now = ReadUserSettings();
        if (now == user) {
            return;
        }
        const UserSettings old = user;
        user = now;
        const auto& api = Api();
        if (engine) {
            if (old.hd_enabled && !now.hd_enabled) {
                SDL_SendJoystickEffect(joystick, NeutralPacket.data(),
                                      static_cast<int>(NeutralPacket.size()));
            }
            ApplyEngineSettings();
        }
        if (dsp && (old.dualsense_volume != now.dualsense_volume ||
                    old.dualsense_crisp_notes != now.dualsense_crisp_notes)) {
            // The audio callback runs with the stream locked, so it never sees a freed DSP.
            if (audio_stream) {
                SDL_LockAudioStream(audio_stream);
            }
            api.dsp_destroy(dsp);
            dsp = api.dsp_create(now.dualsense_volume, now.dualsense_crisp_notes);
            if (audio_stream) {
                SDL_UnlockAudioStream(audio_stream);
            }
        }
    }

    // Caller holds the mutex. The DualSense exposes its voice coils as a 4-channel USB audio
    // device that Windows may list a moment after the HID device, hence the retries.
    void TryOpenDualSenseAudio() {
        if (!dsp || audio_stream) {
            return;
        }
        ++audio_attempts;
        if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
            audio_attempts = DualSenseAudioMaxAttempts;
            LOG_INFO(Input, "RenHDrum: DualSense on Bluetooth, using the standard rumble");
            return;
        }
        if (!SDL_WasInit(SDL_INIT_AUDIO)) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                LOG_WARNING(Input, "RenHDrum: SDL audio init failed: {}", SDL_GetError());
                return;
            }
            audio_inited = true;
        }

        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        SDL_AudioDeviceID matched = 0;
        for (int i = 0; devices && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            SDL_AudioSpec spec{};
            const int channels =
                SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr) ? spec.channels : 0;
            if (name && channels >= 4 &&
                (std::strstr(name, "DualSense") != nullptr ||
                 std::strstr(name, "Wireless Controller") != nullptr)) {
                matched = devices[i];
                break;
            }
        }
        SDL_free(devices);
        if (matched == 0) {
            LOG_DEBUG(Input, "RenHDrum: no DualSense audio device yet (attempt {})", audio_attempts);
            return;
        }

        const SDL_AudioSpec spec{
            .format = SDL_AUDIO_F32,
            .channels = 4,
            .freq = 48000,
        };
        audio_stream = SDL_OpenAudioDeviceStream(matched, &spec, &AudioCallback, this);
        if (!audio_stream) {
            LOG_WARNING(Input, "RenHDrum: could not open the DualSense audio device: {}",
                        SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(audio_stream);
        // Stop SDL's HID rumble once so the pad plays the audio haptics
        if (gamepad) {
            SDL_RumbleGamepad(gamepad, 0, 0, 0);
        } else {
            SDL_RumbleJoystick(joystick, 0, 0, 0);
        }
        LOG_INFO(Input, "RenHDrum: DualSense haptics on \"{}\"", SDL_GetAudioDeviceName(matched));
    }

    static void SDLCALL AudioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additional_amount, int /*total_amount*/) {
        auto* const self = static_cast<Impl*>(userdata);
        if (additional_amount <= 0 || !self || !self->dsp) {
            return;
        }
        constexpr int frame_bytes = 4 * static_cast<int>(sizeof(float));
        constexpr int chunk_frames = 256;
        std::array<float, chunk_frames * 4> chunk{};
        int frames = additional_amount / frame_bytes;
        while (frames > 0) {
            const int count = (std::min)(frames, chunk_frames);
            if (Api().dsp_fill_f32(self->dsp, chunk.data(), static_cast<size_t>(count)) !=
                RENHDRUM_OK) {
                chunk.fill(0.0f); // never send garbage to the actuators
            }
            SDL_PutAudioStreamData(stream, chunk.data(), count * frame_bytes);
            frames -= count;
        }
    }

    // Caller holds the mutex.
    void Release() {
        const auto& api = Api();
        if (audio_stream) {
            SDL_PauseAudioStreamDevice(audio_stream);
            SDL_ClearAudioStream(audio_stream);
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = nullptr;
        }
        if (audio_inited) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            audio_inited = false;
        }
        if (engine) {
            SDL_SendJoystickEffect(joystick, NeutralPacket.data(),
                                      static_cast<int>(NeutralPacket.size()));
            api.engine_destroy(engine);
            engine = nullptr;
        }
        if (dsp) {
            api.dsp_destroy(dsp);
            dsp = nullptr;
        }
    }
};

bool IsRenHDrumBuilt() {
    return true;
}

bool IsRenHDrumLoaded() {
    return Api().loaded;
}

std::shared_ptr<RenHDrumPad> RenHDrumPad::Create(SDL_Joystick* joystick, SDL_Gamepad* gamepad) {
    if (!joystick || !IsRenHDrumLoaded()) {
        return nullptr;
    }
    const Uint16 vid = SDL_GetJoystickVendor(joystick);
    const Uint16 pid = SDL_GetJoystickProduct(joystick);
    const bool switch_pad = IsSwitchHdController(vid, pid);
    if (!switch_pad && !IsDualSense(vid, pid)) {
        return nullptr;
    }

    const auto& api = Api();
    auto state = std::make_unique<Impl>();
    state->joystick = joystick;
    state->gamepad = gamepad;
    state->is_bluetooth =
        SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    state->user = ReadUserSettings();

    std::scoped_lock lock{state->mutex};
    if (switch_pad) {
        state->engine = api.engine_create(state->is_bluetooth, pid == 0x2009);
        if (!state->engine) {
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: HD Rumble for {:04X}:{:04X} (Bluetooth: {})", vid, pid,
                 state->is_bluetooth);
        state->ApplyEngineSettings();
    } else {
        state->dsp = api.dsp_create(state->user.dualsense_volume, state->user.dualsense_crisp_notes);
        if (!state->dsp) {
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: DualSense haptics for {:04X}:{:04X}", vid, pid);
        state->TryOpenDualSenseAudio();
    }
    return std::shared_ptr<RenHDrumPad>(new RenHDrumPad(std::move(state)));
}

RenHDrumPad::RenHDrumPad(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}

RenHDrumPad::~RenHDrumPad() {
    Shutdown();
}

void RenHDrumPad::Shutdown() {
    std::scoped_lock lock{impl->mutex};
    if (impl->joystick) {
        impl->Release();
        impl->joystick = nullptr;
        impl->gamepad = nullptr;
    }
}

bool RenHDrumPad::IsActive() const {
    std::scoped_lock lock{impl->mutex};
    return impl->engine || (impl->dsp && impl->audio_stream);
}

bool RenHDrumPad::Vibrate(std::int64_t now_us, bool is_right,
                          const Common::Input::VibrationStatus& status) {
    {
        std::scoped_lock lock{impl->mutex};
        if (!impl->joystick) {
            return false;
        }
        impl->RefreshUserSettings();
        const auto& u = impl->user;
        if (!u.hd_enabled) {
            return false;
        }
        const float low = ShapeAmplitude(status.low_amplitude, u);
        const float high = ShapeAmplitude(status.high_amplitude, u);
        const auto& api = Api();
        if (!impl->engine) {
            if (!impl->dsp || !u.dualsense_enabled) {
                return false;
            }
            api.dsp_set_half_target(impl->dsp, is_right, low, status.low_frequency, high,
                                    status.high_frequency);
            return impl->audio_stream != nullptr;
        }
        api.engine_push_half_sample(impl->engine, now_us, is_right, low, status.low_frequency,
                                    high, status.high_frequency);
    }
    Tick(now_us);
    return true;
}

void RenHDrumPad::Tick(std::int64_t now_us) {
    std::array<Uint8, 10> packet{};
    SDL_Joystick* joystick = nullptr;
    {
        std::scoped_lock lock{impl->mutex};
        if (!impl->joystick) {
            return;
        }
        if (impl->dsp && !impl->audio_stream && impl->audio_attempts < DualSenseAudioMaxAttempts &&
            now_us >= impl->audio_next_us) {
            impl->audio_next_us = now_us + 2'000'000;
            impl->TryOpenDualSenseAudio();
        }
        if (!impl->engine || Api().engine_tick(impl->engine, now_us, packet.data()) != 1) {
            return;
        }
        joystick = impl->joystick;
    }
    SDL_SendJoystickEffect(joystick, packet.data(), static_cast<int>(packet.size()));
}

} // namespace InputCommon

#else // ENABLE_RENHDRUM

namespace InputCommon {

bool IsRenHDrumBuilt() {
    return false;
}

bool IsRenHDrumLoaded() {
    return false;
}

struct RenHDrumPad::Impl {};

std::shared_ptr<RenHDrumPad> RenHDrumPad::Create(SDL_Joystick*, SDL_Gamepad*) {
    return nullptr;
}

RenHDrumPad::RenHDrumPad(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}
RenHDrumPad::~RenHDrumPad() = default;
void RenHDrumPad::Shutdown() {}
bool RenHDrumPad::IsActive() const {
    return false;
}
bool RenHDrumPad::Vibrate(std::int64_t, bool, const Common::Input::VibrationStatus&) {
    return false;
}
void RenHDrumPad::Tick(std::int64_t) {}

} // namespace InputCommon

#endif // ENABLE_RENHDRUM

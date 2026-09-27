#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <iostream>
#include <complex>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

// Globale Steuerung
std::atomic<float>    g_mic_energy{0.0f};
std::atomic<float>    g_cutoff_base{400.0f};  // Filter-Frequenz in Hz
std::atomic<float>    g_resonance{0.6f};     // Filter-Resonanz (0.0 bis 0.95)
std::atomic<uint64_t> g_bpm{124};             // Tempo für Arpeggiator
std::atomic<bool>     g_running{true};

// 16-Step Analog Bassline-Noten (Frequenzen in Hz)
const float BASS_ARPEGGIOM_FREQ[16] = {
    65.41f,  // C2
    65.41f,  // C2
    130.81f, // C3
    77.78f,  // Eb2
    65.41f,  // C2
    98.00f,  // G2
    116.54f, // Bb2
    130.81f, // C3
    58.27f,  // Bb1
    116.54f, // Bb2
    65.41f,  // C2
    155.56f, // Eb3
    77.78f,  // Eb2
    65.41f,  // C2
    98.00f,  // G2
    87.31f   // F2
};

// State für den Arpeggiator & Moog Ladder Filter
struct AdelicSynthState {
    uint64_t n = 0;
    double sampleRate = 44100.0;

    // Q3 Neuronal-Zustand
    int64_t a = 729;
    int64_t b = 7;

    // Phasen & Arpeggiator
    double phase = 0.0;
    uint64_t step_index = 0;
    uint64_t samples_per_step = 5334; // ca. 124 BPM bei 16tel Noten
    uint64_t step_sample_count = 0;

    // Hüllkurve (Env Follower)
    float env = 0.0f;

    // Moog 24dB Ladder Filter Zustände
    float stage[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float stage_delay[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// 3-Adische Hensel-Bestimmung
inline int extract_3adic_trit(int64_t a, int64_t b) {
    int rem_a = static_cast<int>((a % 3 + 3) % 3);
    int rem_b = static_cast<int>((b % 3 + 3) % 3);

    if (rem_a == 0) return 0;
    if ((rem_a - rem_b + 3) % 3 == 0) return 1;
    if ((rem_a + rem_b) % 3 == 0)     return -1;
    return 0;
}

// 24dB Moog Lowpass Filter Implementation
float process_moog_filter(float input, float cutoff_hz, float resonance, float sampleRate, float* stage, float* stage_delay) {
    float cutoff = std::clamp(cutoff_hz, 40.0f, 12000.0f);
    float res = std::clamp(resonance, 0.0f, 0.95f);

    float f = (2.0f * cutoff) / sampleRate;
    float k = 3.6f * f - 1.6f * f * f - 1.0f;
    float p = (k + 1.0f) * 0.5f;
    float scale = std::pow(1.0f - p, 1.3f) * 1.3f;
    float r = res * 4.0f * scale;

    float x = input - r * stage_delay[3];

    // 4 Pol-Stufen (24dB / Octave)
    stage[0] = x * p + stage_delay[0] * (1.0f - p);
    stage[1] = stage[0] * p + stage_delay[1] * (1.0f - p);
    stage[2] = stage[1] * p + stage_delay[2] * (1.0f - p);
    stage[3] = stage[2] * p + stage_delay[3] * (1.0f - p);

    for (int i = 0; i < 4; ++i) {
        stage_delay[i] = stage[i];
    }

    return stage[3];
}

// Full-Duplex Audio-Callback
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AdelicSynthState* state = static_cast<AdelicSynthState*>(pDevice->pUserData);
    float* out = static_cast<float*>(pOutput);
    const float* mic_in = static_cast<const float*>(pInput);

    // 1. RMS Mikrofon-Energie messen
    float sum_sq = 0.0f;
    if (mic_in) {
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            float sample = mic_in[i];
            sum_sq += sample * sample;
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(frameCount));
        g_mic_energy.store(rms);
    }

    float mic_e = g_mic_energy.load();
    float base_cutoff = g_cutoff_base.load();
    float res = g_resonance.load();
    uint64_t bpm = g_bpm.load();

    // Dynamische Berechnung der Sample-Länge pro Step basierend auf BPM
    state->samples_per_step = static_cast<uint64_t>((60.0 / static_cast<double>(bpm) / 4.0) * state->sampleRate);

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        state->n++;
        state->step_sample_count++;

        // --- A. ARPEGGIATOR SEQUENZER STEP-WECHSEL ---
        if (state->step_sample_count >= state->samples_per_step) {
            state->step_sample_count = 0;
            state->step_index = (state->step_index + 1) % 16;
            state->env = 1.0f; // Triggert Hüllkurve für Re-Sustain
        }

        // Hüllkurven-Abfall (Exponential Decay)
        state->env *= 0.9993f;

        // --- B. Q3 NEURONEN-DYNAMIK ---
        if (mic_e > 0.01f) {
            state->a += static_cast<int64_t>(mic_e * 50.0f);
        }

        int t_n = extract_3adic_trit(state->a, state->b);
        state->a = (state->a - static_cast<int64_t>(t_n) * state->b) / 3;
        if (state->a == 0) state->a = 729 + (state->n % 81);

        // --- C. ANALOG BASS OSZILLATOR (Sägezahn + Sub-Oszillator) ---
        float target_freq = BASS_ARPEGGIOM_FREQ[state->step_index];
        
        // Minor Pitch Modulation basierend auf Q3 Trit
        if (t_n == 1) target_freq *= 1.25f;  // Minor 3rd Transposition
        if (t_n == -1) target_freq *= 0.5f;   // Octave Down

        double phase_inc = (2.0 * M_PI * target_freq) / state->sampleRate;
        state->phase += phase_inc;

        // Phasen-Wrap um numerische Instabilitäten zu unterbinden
        if (state->phase >= 2.0 * M_PI) {
            state->phase -= 2.0 * M_PI;
        }

        // Sägezahn-Welle + Quadrat Sub-Oszillator
        float saw = static_cast<float>(1.0 - (state->phase / M_PI));
        float sub = (std::sin(state->phase * 0.5) > 0.0) ? 0.4f : -0.4f;
        float raw_osc = (saw * 0.7f) + sub;

        // --- D. ANALOG FILTER & STIMMEN-MODULATION ---
        float dynamic_cutoff = base_cutoff + (state->env * 600.0f) + (mic_e * 8000.0f);
        
        float filtered = process_moog_filter(raw_osc, dynamic_cutoff, res, static_cast<float>(state->sampleRate), state->stage, state->stage_delay);

        // Saturation / Warm Overdrive
        float drive_signal = filtered * (1.2f + mic_e * 4.0f);
        float saturated = std::tanh(drive_signal);

        // --- E. MASTER OUTPUT & SANITIZATION ---
        float final_sample = saturated * 0.5f;

        if (state->n < 4410) {
            final_sample *= (static_cast<float>(state->n) / 4410.0f);
        }

        if (std::isnan(final_sample) || std::isinf(final_sample)) {
            final_sample = 0.0f;
        }

        out[i * 2 + 0] = final_sample; // Links
        out[i * 2 + 1] = final_sample; // Rechts
    }
}

void setup_terminal(termios& orig_opts) {
    tcgetattr(STDIN_FILENO, &orig_opts);
    termios raw_opts = orig_opts;
    raw_opts.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_opts);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void restore_terminal(termios& orig_opts) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_opts);
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "     ANALOG BASS ARPEGGIATOR - VOICE CONTROLLED          \n";
    std::cout << "=========================================================\n\n";

    AdelicSynthState synth_state;

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 2;
    config.capture.format    = ma_format_f32;
    config.capture.channels  = 1;
    config.sampleRate        = static_cast<ma_uint32>(synth_state.sampleRate);
    config.dataCallback      = data_callback;
    config.pUserData         = &synth_state;
    config.periodSizeInFrames = 256;

    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        std::cerr << "Fehler bei Context-Init" << std::endl;
        return -1;
    }

    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info* pCaptureInfos;
    ma_uint32 captureCount;

    ma_device_id usb_device_id;
    bool found_target_dev = false;
    std::string selected_dev_name = "";

    if (ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) == MA_SUCCESS) {
        std::cout << ">> Erkannte Eingabegeräte:\n";
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            std::string dev_name = pCaptureInfos[i].name;
            std::cout << "   [" << i << "] " << dev_name << "\n";

            std::string dev_name_lower = dev_name;
            std::transform(dev_name_lower.begin(), dev_name_lower.end(), dev_name_lower.begin(), ::tolower);

            // Loopback / Monitor ignorieren
            if (dev_name_lower.find("monitor") != std::string::npos || 
                dev_name_lower.find("loopback") != std::string::npos) {
                continue;
            }

            // Prio 1: PCM2904 / USB Codec
            if (dev_name_lower.find("pcm2904") != std::string::npos ||
                dev_name_lower.find("pcm") != std::string::npos ||
                (dev_name_lower.find("usb") != std::string::npos && dev_name_lower.find("codec") != std::string::npos)) {
                
                usb_device_id = pCaptureInfos[i].id;
                config.capture.pDeviceID = &usb_device_id;
                selected_dev_name = dev_name;
                found_target_dev = true;
                break;
            }

            // Prio 2: Jedes andere Nicht-Built-in Device
            if (!found_target_dev && 
                dev_name_lower.find("built-in") == std::string::npos && 
                dev_name_lower.find("intern") == std::string::npos &&
                (dev_name_lower.find("usb") != std::string::npos || dev_name_lower.find("codec") != std::string::npos)) {
                
                usb_device_id = pCaptureInfos[i].id;
                config.capture.pDeviceID = &usb_device_id;
                selected_dev_name = dev_name;
                found_target_dev = true;
            }
        }
    }

    if (found_target_dev) {
        std::cout << "\n>> ERFOLG: gekoppelt an -> " << selected_dev_name << std::endl;
    } else {
        std::cout << "\n>> WARNUNG: Verwende Standard-Eingabegerät." << std::endl;
    }

    config.playback.pDeviceID = NULL;

    ma_device device;
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
        std::cerr << "Fehler bei Gerät-Init" << std::endl;
        ma_context_uninit(&context);
        return -1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "Fehler beim Starten" << std::endl;
        ma_device_uninit(&device);
        ma_context_uninit(&context);
        return -1;
    }

    termios orig_opts;
    setup_terminal(orig_opts);

    std::cout << "\nSynth läuft! Sprich ins Mikrofon, um den Filter zu öffnen.\n";
    std::cout << "----------------- STEUERUNG -----------------\n";
    std::cout << "[ + / - ] BPM anpassen (Tempo)\n";
    std::cout << "[ U / J ] Cutoff Frequenz hoch / runter\n";
    std::cout << "[ R ] Resonanz Umschalten (Acid/Warm)\n";
    std::cout << "[ ESC ] Beenden\n";
    std::cout << "---------------------------------------------\n" << std::flush;

    while (g_running.load()) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            switch (c) {
                case '+':
                    g_bpm.store(g_bpm.load() + 4);
                    std::cout << "\r>> Tempo: " << g_bpm.load() << " BPM        \n" << std::flush;
                    break;
                case '-':
                    if (g_bpm.load() > 40) g_bpm.store(g_bpm.load() - 4);
                    std::cout << "\r>> Tempo: " << g_bpm.load() << " BPM        \n" << std::flush;
                    break;
                case 'u': case 'U':
                    g_cutoff_base.store(g_cutoff_base.load() + 100.0f);
                    std::cout << "\r>> Cutoff Base: " << g_cutoff_base.load() << " Hz   \n" << std::flush;
                    break;
                case 'j': case 'J':
                    if (g_cutoff_base.load() > 100.0f) g_cutoff_base.store(g_cutoff_base.load() - 100.0f);
                    std::cout << "\r>> Cutoff Base: " << g_cutoff_base.load() << " Hz   \n" << std::flush;
                    break;
                case 'r': case 'R':
                    if (g_resonance.load() > 0.7f) {
                        g_resonance.store(0.4f);
                        std::cout << "\r>> Resonanz: Warm Lowpass (0.4)    \n" << std::flush;
                    } else {
                        g_resonance.store(0.85f);
                        std::cout << "\r>> Resonanz: Acid Resonance (0.85)  \n" << std::flush;
                    }
                    break;
                case 27:
                    g_running.store(false);
                    break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    restore_terminal(orig_opts);
    ma_device_uninit(&device);
    ma_context_uninit(&context);
    std::cout << "\nBass-Synth gestoppt." << std::endl;

    return 0;
}

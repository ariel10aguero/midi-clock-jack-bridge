#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <jack/transport.h>
#include <signal.h>
#include <poll.h>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <thread>
#include <unistd.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
constexpr int PULSES_PER_QUARTER = 24;
constexpr double MIN_BPM = 20.0;
constexpr double MAX_BPM = 300.0;
constexpr double SMOOTHING_FACTOR = 0.3;
constexpr double BPM_SNAP_THRESHOLD = 0.15;
constexpr int BPM_STABILITY_COUNT = 3;

// ============================================================================
// GLOBAL STATE
// ============================================================================
std::atomic<bool> g_running(true);
snd_seq_t* g_seq_handle = nullptr;
jack_client_t* g_jack_client = nullptr;

struct BPMState {
    std::atomic<double> current_bpm{120.0};
    std::atomic<int> pulse_count{0};
    std::chrono::high_resolution_clock::time_point last_pulse_time;
    std::atomic<bool> transport_rolling{false};
    std::atomic<bool> first_clock_received{false};
    
    // Stability tracking
    double last_snapped_bpm = 0.0;
    int stability_counter = 0;
    
    // Convergence tracking
    std::atomic<int> measurement_count{0};
    
    // Bar/beat tracking
    std::atomic<int> bar{1};
    std::atomic<int> beat{1};
    std::atomic<int> tick{0};  // Track ticks within beat (0-1919)
    
    // Frame tracking for JACK transport
    std::atomic<jack_nframes_t> current_frame{0};
    std::chrono::high_resolution_clock::time_point transport_start_time;
    jack_nframes_t sample_rate = 48000;
    
    // For display
    std::atomic<double> last_updated_jack_bpm{0.0};
};

BPMState g_bpm_state;

// ============================================================================
// JACK PROCESS CALLBACK - Updates frame position in real-time
// ============================================================================
int jack_process_callback(jack_nframes_t nframes, void* arg) {
    (void)arg;
    
    // Update frame counter if transport is rolling
    if (g_bpm_state.transport_rolling.load()) {
        jack_nframes_t current = g_bpm_state.current_frame.load();
        g_bpm_state.current_frame.store(current + nframes);
    }
    
    return 0;
}

// ============================================================================
// JACK TIMEBASE CALLBACK - Provides BBT info based on current position
// ============================================================================
void jack_timebase_callback(jack_transport_state_t state,
                            jack_nframes_t nframes,
                            jack_position_t *pos,
                            int new_pos,
                            void *arg) {
    (void)state;
    (void)nframes;
    (void)arg;
    
    double bpm = g_bpm_state.current_bpm.load();
    jack_nframes_t sample_rate = g_bpm_state.sample_rate;
    
    // On position change (seek, start, etc), reset to our tracked position
    if (new_pos) {
        pos->frame = g_bpm_state.current_frame.load();
    } else {
        // Normal operation: use JACK's frame counter
        g_bpm_state.current_frame.store(pos->frame);
    }
    
    // Calculate BBT position from frame position
    pos->valid = JackPositionBBT;
    pos->beats_per_bar = 4.0;
    pos->beat_type = 4.0;
    pos->ticks_per_beat = 1920.0;
    pos->beats_per_minute = bpm;
    
    // Calculate elapsed time in seconds
    double seconds_elapsed = (double)pos->frame / (double)sample_rate;
    
    // Calculate total beats elapsed
    double beats_elapsed = (bpm / 60.0) * seconds_elapsed;
    
    // Calculate bar, beat, tick from beats_elapsed
    double beats_per_bar = pos->beats_per_bar;
    double total_bars = beats_elapsed / beats_per_bar;
    
    pos->bar = (int32_t)(total_bars) + 1;  // Bars start at 1
    
    double beat_in_bar = fmod(beats_elapsed, beats_per_bar);
    pos->beat = (int32_t)(beat_in_bar) + 1;  // Beats start at 1
    
    double tick_in_beat = fmod(beat_in_bar, 1.0) * pos->ticks_per_beat;
    pos->tick = (int32_t)(tick_in_beat);
    
    // Calculate bar start tick
    pos->bar_start_tick = (pos->bar - 1) * beats_per_bar * pos->ticks_per_beat;
    
    // Calculate ticks per second for proper BBT time tracking
    double beats_per_second = bpm / 60.0;
    double ticks_per_second = beats_per_second * pos->ticks_per_beat;
    pos->ticks_per_beat = pos->ticks_per_beat;
    
    // Also store in our state for display
    g_bpm_state.bar.store(pos->bar);
    g_bpm_state.beat.store(pos->beat);
    g_bpm_state.tick.store(pos->tick);
}

// ============================================================================
// SIGNAL HANDLERS
// ============================================================================
void signal_handler(int) {
    std::cout << "\n[INFO] Received shutdown signal, exiting..." << std::endl;
    g_running = false;
}

void display_status() {
    std::cout << "\nââ€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â" << std::endl;
    std::cout << "â     MIDI Clock Sync Status             â" << std::endl;
    std::cout << "ââ€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â" << std::endl;
    
    if (g_jack_client) {
        jack_position_t pos;
        jack_transport_state_t state = jack_transport_query(g_jack_client, &pos);
        
        std::cout << "Transport State: ";
        switch(state) {
            case JackTransportStopped:  std::cout << "â¹  STOPPED" << std::endl; break;
            case JackTransportRolling:  std::cout << "â¶  PLAYING" << std::endl; break;
            case JackTransportStarting: std::cout << "â¯  STARTING" << std::endl; break;
            default: std::cout << "? UNKNOWN" << std::endl;
        }
        
        if (pos.valid & JackPositionBBT) {
            std::cout << "JACK BPM:        " << std::fixed << std::setprecision(2) 
                      << pos.beats_per_minute << std::endl;
            std::cout << "Position:        Bar " << pos.bar << ", Beat " << pos.beat 
                      << ", Tick " << pos.tick << std::endl;
            std::cout << "Frame:           " << pos.frame << std::endl;
            std::cout << "Time Signature:  " << pos.beats_per_bar << "/" << pos.beat_type << std::endl;
        } else {
            std::cout << "BBT Info:        Not available" << std::endl;
        }
    }
    
    std::cout << "Detected BPM:    " << g_bpm_state.current_bpm.load() << std::endl;
    std::cout << "Measurements:    " << g_bpm_state.measurement_count.load() << std::endl;
    std::cout << "Current Pos:     " << g_bpm_state.bar.load() << ":" 
              << g_bpm_state.beat.load() << ":" << g_bpm_state.tick.load() << std::endl;
    std::cout << "â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€â€\n" << std::endl;
}

void status_signal_handler(int) {
    display_status();
}

// ============================================================================
// BPM SNAPPING FUNCTION
// ============================================================================
double snap_bpm(double, double smoothed_bpm) {
    double nearest_int = std::round(smoothed_bpm);
    double distance = std::abs(smoothed_bpm - nearest_int);
    
    if (distance <= BPM_SNAP_THRESHOLD) {
        if (std::abs(g_bpm_state.last_snapped_bpm - nearest_int) < 0.5) {
            g_bpm_state.stability_counter++;
        } else {
            g_bpm_state.stability_counter = 1;
            g_bpm_state.last_snapped_bpm = nearest_int;
        }
        
        if (g_bpm_state.stability_counter >= BPM_STABILITY_COUNT) {
            return nearest_int;
        }
    } else {
        g_bpm_state.stability_counter = 0;
    }
    
    return smoothed_bpm;
}

// ============================================================================
// JACK TRANSPORT UPDATE
// ============================================================================
void update_jack_transport_bpm(double bpm) {
    if (!g_jack_client) return;
    
    double last_bpm = g_bpm_state.last_updated_jack_bpm.load();
    
    // Log significant changes
    if (std::abs(bpm - last_bpm) > 0.3) {
        g_bpm_state.last_updated_jack_bpm.store(bpm);
        std::cout << "[JACK] Transport BPM updated to: " << std::fixed 
                  << std::setprecision(2) << bpm << std::endl;
    }
    
    // Force timebase callback to run by relocating to current position
    // This ensures Carla and other apps see the update immediately
    jack_position_t pos;
    jack_transport_query(g_jack_client, &pos);
    jack_transport_reposition(g_jack_client, &pos);
}

// ============================================================================
// BPM CALCULATION
// ============================================================================
void calculate_and_set_bpm() {
    auto now = std::chrono::high_resolution_clock::now();
    
    int count = g_bpm_state.pulse_count.load();
    
    // Initialize timing on first clock
    if (!g_bpm_state.first_clock_received.load()) {
        g_bpm_state.first_clock_received.store(true);
        g_bpm_state.last_pulse_time = now;
        g_bpm_state.pulse_count.store(0);
        g_bpm_state.transport_start_time = now;
        
        // Auto-start transport on first clock
        if (g_jack_client && !g_bpm_state.transport_rolling.load()) {
            jack_transport_start(g_jack_client);
            g_bpm_state.transport_rolling.store(true);
            std::cout << "[MIDI] First clock received - auto-starting transport" << std::endl;
        }
        return;
    }
    
    count++;
    
    // Calculate BPM after 24 pulses (one quarter note)
    if (count >= PULSES_PER_QUARTER) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - g_bpm_state.last_pulse_time).count();
        
        if (elapsed > 0) {
            double raw_bpm = 60000000.0 / elapsed;
            raw_bpm = std::max(MIN_BPM, std::min(MAX_BPM, raw_bpm));
            
            double current = g_bpm_state.current_bpm.load();
            double smoothed_bpm;
            int mcount = g_bpm_state.measurement_count.load();
            
            // Adaptive smoothing: fast convergence initially or on large changes
            if (mcount < 5 || std::abs(raw_bpm - current) > 10.0) {
                // Fast convergence (90% new value)
                smoothed_bpm = current * 0.1 + raw_bpm * 0.9;
            } else if (mcount < 10 || std::abs(raw_bpm - current) > 3.0) {
                // Medium convergence (50% new value)
                smoothed_bpm = current * 0.5 + raw_bpm * 0.5;
            } else {
                // Normal smoothing
                smoothed_bpm = current * (1.0 - SMOOTHING_FACTOR) + raw_bpm * SMOOTHING_FACTOR;
            }
            
            double final_bpm = snap_bpm(raw_bpm, smoothed_bpm);
            
            g_bpm_state.current_bpm.store(final_bpm);
            g_bpm_state.measurement_count++;
            
            // Update JACK transport
            update_jack_transport_bpm(final_bpm);
            
            // Display
            std::string snap_indicator = (final_bpm == std::round(final_bpm)) ? " [LOCKED]" : "";
            
            std::cout << "[MIDI] " << g_bpm_state.bar << ":" << g_bpm_state.beat 
                      << " | BPM: " << std::fixed << std::setprecision(2) 
                      << final_bpm << " (raw: " << raw_bpm << ")" 
                      << snap_indicator << std::endl;
        }
        
        g_bpm_state.pulse_count.store(0);
        g_bpm_state.last_pulse_time = now;
        
        // Status display every 16 beats
        if (g_bpm_state.measurement_count % 16 == 0) {
            display_status();
        }
    } else {
        g_bpm_state.pulse_count.store(count);
    }
}

// ============================================================================
// MIDI EVENT PROCESSING
// ============================================================================
void process_midi_clock(snd_seq_event_t* ev) {
    if (!ev) return;
    
    switch (ev->type) {
        case SND_SEQ_EVENT_CLOCK:
            calculate_and_set_bpm();
            break;
            
        case SND_SEQ_EVENT_START:
            std::cout << "[MIDI] START received" << std::endl;
            if (g_jack_client) {
                // Reset to beginning
                g_bpm_state.current_frame.store(0);
                g_bpm_state.bar.store(1);
                g_bpm_state.beat.store(1);
                g_bpm_state.tick.store(0);
                
                // Relocate transport to frame 0
                jack_position_t pos;
                pos.frame = 0;
                pos.valid = (jack_position_bits_t)0;
                jack_transport_reposition(g_jack_client, &pos);
                
                // Start transport
                jack_transport_start(g_jack_client);
                g_bpm_state.transport_rolling.store(true);
            }
            g_bpm_state.pulse_count.store(0);
            g_bpm_state.measurement_count.store(0);
            g_bpm_state.first_clock_received.store(false);
            g_bpm_state.transport_start_time = std::chrono::high_resolution_clock::now();
            break;
            
        case SND_SEQ_EVENT_STOP:
            std::cout << "[MIDI] STOP received" << std::endl;
            if (g_jack_client) {
                jack_transport_stop(g_jack_client);
                g_bpm_state.transport_rolling.store(false);
            }
            g_bpm_state.pulse_count.store(0);
            g_bpm_state.first_clock_received.store(false);
            break;
            
        case SND_SEQ_EVENT_CONTINUE:
            std::cout << "[MIDI] CONTINUE received" << std::endl;
            if (g_jack_client) {
                jack_transport_start(g_jack_client);
                g_bpm_state.transport_rolling.store(true);
            }
            break;
            
        default:
            break;
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, status_signal_handler);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  MIDI Clock -> JACK Transport Sync  " << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    // ========================================================================
    // INITIALIZE ALSA SEQUENCER
    // ========================================================================
    if (snd_seq_open(&g_seq_handle, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        std::cerr << "[ERROR] Cannot open ALSA sequencer" << std::endl;
        return 1;
    }
    
    snd_seq_set_client_name(g_seq_handle, "MidiClockSync");
    
    int port = snd_seq_create_simple_port(g_seq_handle, "Input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    
    if (port < 0) {
        std::cerr << "[ERROR] Cannot create ALSA port" << std::endl;
        snd_seq_close(g_seq_handle);
        return 1;
    }
    
    int client_id = snd_seq_client_id(g_seq_handle);
    std::cout << "[ALSA] MIDI port created: " << client_id << ":" << port << std::endl;
    
    // Auto-connect to source if specified
    if (argc > 1) {
        snd_seq_addr_t sender, dest;
        if (snd_seq_parse_address(g_seq_handle, &sender, argv[1]) == 0) {
            dest.client = client_id;
            dest.port = port;
            
            if (snd_seq_connect_from(g_seq_handle, port, sender.client, sender.port) == 0) {
                std::cout << "[ALSA] Auto-connected to: " << argv[1] << std::endl;
            } else {
                std::cerr << "[WARN] Could not auto-connect to " << argv[1] << std::endl;
            }
        } else {
            std::cerr << "[WARN] Invalid MIDI address: " << argv[1] << std::endl;
        }
    } else {
        std::cout << "[INFO] Usage: " << argv[0] << " <midi_port>" << std::endl;
        std::cout << "       Example: " << argv[0] << " 32:0" << std::endl;
        std::cout << "       Use 'aconnect -l' to list available ports" << std::endl;
    }
    
    // ========================================================================
    // INITIALIZE JACK CLIENT
    // ========================================================================
    g_jack_client = jack_client_open("MidiClockSync", JackNoStartServer, nullptr);
    if (!g_jack_client) {
        std::cerr << "[ERROR] Cannot connect to JACK server" << std::endl;
        snd_seq_close(g_seq_handle);
        return 1;
    }
    
    // Get sample rate
    g_bpm_state.sample_rate = jack_get_sample_rate(g_jack_client);
    std::cout << "[JACK] Sample rate: " << g_bpm_state.sample_rate << " Hz" << std::endl;
    
    // Register process callback for frame tracking
    jack_set_process_callback(g_jack_client, jack_process_callback, nullptr);
    
    // Register as timebase master with conditional takeover
    if (jack_set_timebase_callback(g_jack_client, 1, jack_timebase_callback, nullptr) == 0) {
        std::cout << "[JACK] Registered as timebase master" << std::endl;
    } else {
        std::cerr << "[WARN] Could not become timebase master (another master exists)" << std::endl;
        std::cerr << "[INFO] Will still track BPM but won't control JACK transport BBT" << std::endl;
    }
    
    if (jack_activate(g_jack_client) != 0) {
        std::cerr << "[ERROR] Cannot activate JACK client" << std::endl;
        jack_client_close(g_jack_client);
        snd_seq_close(g_seq_handle);
        return 1;
    }
    
    std::cout << "[JACK] Client activated successfully" << std::endl;
    
    // ========================================================================
    // MAIN LOOP - PROCESS MIDI EVENTS
    // ========================================================================
    std::cout << "\n========================================" << std::endl;
    std::cout << "Waiting for MIDI Clock messages..." << std::endl;
    std::cout << "Transport will auto-start on first clock" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    std::cout << "Send SIGUSR1 for status: kill -USR1 " << getpid() << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    int npfds = snd_seq_poll_descriptors_count(g_seq_handle, POLLIN);
    struct pollfd pfds[npfds];
    snd_seq_poll_descriptors(g_seq_handle, pfds, npfds, POLLIN);
    
    snd_seq_event_t* ev = nullptr;
    
    while (g_running) {
        // Poll for MIDI events with 100ms timeout
        if (poll(pfds, npfds, 100) > 0) {
            do {
                if (snd_seq_event_input(g_seq_handle, &ev) >= 0) {
                    if (ev) {
                        process_midi_clock(ev);
                        snd_seq_free_event(ev);
                        ev = nullptr;
                    }
                }
            } while (snd_seq_event_input_pending(g_seq_handle, 0) > 0);
        }
    }
    
    // ========================================================================
    // CLEANUP
    // ========================================================================
    std::cout << "\n[INFO] Cleaning up..." << std::endl;
    
    if (g_jack_client) {
        jack_client_close(g_jack_client);
        std::cout << "[JACK] Client closed" << std::endl;
    }
    
    if (g_seq_handle) {
        snd_seq_close(g_seq_handle);
        std::cout << "[ALSA] Sequencer closed" << std::endl;
    }
    
    std::cout << "[INFO] Shutdown complete\n" << std::endl;
    return 0;
}

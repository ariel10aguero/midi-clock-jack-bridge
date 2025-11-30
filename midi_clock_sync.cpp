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
#include <termios.h>
#include <fcntl.h>

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
    std::atomic<int> tick{0};
    
    // Frame tracking for JACK transport
    std::atomic<jack_nframes_t> current_frame{0};
    std::chrono::high_resolution_clock::time_point transport_start_time;
    jack_nframes_t sample_rate = 48000;
    
    // For display
    std::atomic<double> last_updated_jack_bpm{0.0};
};

BPMState g_bpm_state;

// Terminal settings backup
struct termios g_orig_termios;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void display_status();

// ============================================================================
// TERMINAL SETUP FOR NON-BLOCKING INPUT
// ============================================================================
void setup_terminal() {
    // Save original terminal settings
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    
    struct termios term = g_orig_termios;
    term.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    term.c_cc[VMIN] = 0;  // Non-blocking read
    term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    
    // Set stdin to non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

// ============================================================================
// TRANSPORT RESET FUNCTION
// ============================================================================
void reset_transport() {
    std::cout << "\n[CMD] ⏮ Resetting transport to beginning..." << std::endl;
    
    if (g_jack_client) {
        // Stop transport first
        jack_transport_stop(g_jack_client);
        g_bpm_state.transport_rolling.store(false);
        
        // Reset frame counter
        g_bpm_state.current_frame.store(0);
        
        // Reset BBT position
        g_bpm_state.bar.store(1);
        g_bpm_state.beat.store(1);
        g_bpm_state.tick.store(0);
        
        // Relocate JACK transport to frame 0
        jack_position_t pos;
        pos.frame = 0;
        pos.valid = (jack_position_bits_t)0;
        jack_transport_reposition(g_jack_client, &pos);
        
        std::cout << "[CMD] ✓ Transport position: 0:0:0, frame: 0" << std::endl;
    }
    
    // Reset measurement tracking
    g_bpm_state.pulse_count.store(0);
    g_bpm_state.measurement_count.store(0);
    g_bpm_state.first_clock_received.store(false);
    
    std::cout << "[CMD] ✓ Reset complete" << std::endl;
}

// ============================================================================
// JACK PROCESS CALLBACK
// ============================================================================
int jack_process_callback(jack_nframes_t nframes, void* arg) {
    (void)arg;
    
    if (g_bpm_state.transport_rolling.load()) {
        jack_nframes_t current = g_bpm_state.current_frame.load();
        g_bpm_state.current_frame.store(current + nframes);
    }
    
    return 0;
}

// ============================================================================
// JACK TIMEBASE CALLBACK
// ============================================================================
void jack_timebase_callback(jack_transport_state_t state, jack_nframes_t nframes,
                            jack_position_t *pos, int new_pos, void *arg) {
    (void)state;
    (void)nframes;
    (void)arg;
    
    double bpm = g_bpm_state.current_bpm.load();
    jack_nframes_t sample_rate = g_bpm_state.sample_rate;
    
    if (new_pos) {
        pos->frame = g_bpm_state.current_frame.load();
    } else {
        g_bpm_state.current_frame.store(pos->frame);
    }
    
    pos->valid = JackPositionBBT;
    pos->beats_per_bar = 4.0;
    pos->beat_type = 4.0;
    pos->ticks_per_beat = 1920.0;
    pos->beats_per_minute = bpm;
    
    double seconds_elapsed = (double)pos->frame / (double)sample_rate;
    double beats_elapsed = (bpm / 60.0) * seconds_elapsed;
    double beats_per_bar = pos->beats_per_bar;
    double total_bars = beats_elapsed / beats_per_bar;
    
    pos->bar = (int32_t)(total_bars) + 1;
    
    double beat_in_bar = fmod(beats_elapsed, beats_per_bar);
    pos->beat = (int32_t)(beat_in_bar) + 1;
    
    double tick_in_beat = fmod(beat_in_bar, 1.0) * pos->ticks_per_beat;
    pos->tick = (int32_t)(tick_in_beat);
    
    pos->bar_start_tick = (pos->bar - 1) * beats_per_bar * pos->ticks_per_beat;
    
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
    std::cout << "\n┌────────────────────────────────────────┐" << std::endl;
    std::cout << "│ MIDI Clock Sync Status                 │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    
    if (g_jack_client) {
        jack_position_t pos;
        jack_transport_state_t state = jack_transport_query(g_jack_client, &pos);
        
        std::cout << "│ Transport State: ";
        switch(state) {
            case JackTransportStopped:
                std::cout << "⏹ STOPPED              │" << std::endl;
                break;
            case JackTransportRolling:
                std::cout << "▶ PLAYING              │" << std::endl;
                break;
            case JackTransportStarting:
                std::cout << "⏯ STARTING             │" << std::endl;
                break;
            default:
                std::cout << "? UNKNOWN              │" << std::endl;
        }
        
        if (pos.valid & JackPositionBBT) {
            std::cout << "│ JACK BPM: " << std::fixed << std::setprecision(2) 
                      << std::setw(28) << std::left << pos.beats_per_minute << "│" << std::endl;
            
            std::ostringstream oss;
            oss << "Bar " << pos.bar << ", Beat " << pos.beat << ", Tick " << pos.tick;
            std::cout << "│ Position: " << std::setw(29) << std::left << oss.str() << "│" << std::endl;
            std::cout << "│ Frame: " << std::setw(32) << std::left << pos.frame << "│" << std::endl;
        }
    }
    
    std::cout << "│ Detected BPM: " << std::fixed << std::setprecision(2) 
              << std::setw(25) << std::left << g_bpm_state.current_bpm.load() << "│" << std::endl;
    std::cout << "│ Measurements: " << std::setw(25) << std::left 
              << g_bpm_state.measurement_count.load() << "│" << std::endl;
    
    std::ostringstream pos_oss;
    pos_oss << g_bpm_state.bar.load() << ":" << g_bpm_state.beat.load() 
            << ":" << g_bpm_state.tick.load();
    std::cout << "│ Current Pos: " << std::setw(26) << std::left << pos_oss.str() << "│" << std::endl;
    std::cout << "└────────────────────────────────────────┘\n" << std::endl;
}

void status_signal_handler(int) {
    display_status();
}

void reset_signal_handler(int) {
    reset_transport();
    display_status();
}

// ============================================================================
// KEYBOARD COMMAND THREAD - Single keypress, no Enter needed
// ============================================================================
void command_thread_func() {
    char c;
    
    while (g_running) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n > 0) {
            switch(c) {
                case 'r':
                case 'R':
                    reset_transport();
                    break;
                    
                case 's':
                case 'S':
                    display_status();
                    break;
                    
                case 'p':
                case 'P':
                case ' ':  // Space bar also toggles
                    if (g_jack_client) {
                        jack_position_t pos;
                        jack_transport_state_t state = jack_transport_query(g_jack_client, &pos);
                        
                        if (state == JackTransportRolling) {
                            jack_transport_stop(g_jack_client);
                            g_bpm_state.transport_rolling.store(false);
                            std::cout << "\n[CMD] ⏹ Transport stopped" << std::endl;
                        } else {
                            jack_transport_start(g_jack_client);
                            g_bpm_state.transport_rolling.store(true);
                            std::cout << "\n[CMD] ▶ Transport started" << std::endl;
                        }
                    }
                    break;
                    
                case 'h':
                case 'H':
                case '?':
                    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
                    std::cout << "║ Keyboard Commands (no Enter needed)   ║" << std::endl;
                    std::cout << "╠════════════════════════════════════════╣" << std::endl;
                    std::cout << "║ R         - Reset to beginning         ║" << std::endl;
                    std::cout << "║ S         - Show status                ║" << std::endl;
                    std::cout << "║ P or SPACE - Play/Pause toggle         ║" << std::endl;
                    std::cout << "║ H or ?    - Show this help             ║" << std::endl;
                    std::cout << "║ Q         - Quit                       ║" << std::endl;
                    std::cout << "║ Ctrl+C    - Exit                       ║" << std::endl;
                    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;
                    break;
                    
                case 'q':
                case 'Q':
                case 3:  // Ctrl+C
                    std::cout << "\n[CMD] Exiting..." << std::endl;
                    g_running = false;
                    break;
                    
                default:
                    // Ignore other keys silently
                    break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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
    
    if (std::abs(bpm - last_bpm) > 0.3) {
        g_bpm_state.last_updated_jack_bpm.store(bpm);
        std::cout << "[JACK] Transport BPM updated to: " << std::fixed 
                  << std::setprecision(2) << bpm << std::endl;
    }
    
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
    
    if (!g_bpm_state.first_clock_received.load()) {
        g_bpm_state.first_clock_received.store(true);
        g_bpm_state.last_pulse_time = now;
        g_bpm_state.pulse_count.store(0);
        g_bpm_state.transport_start_time = now;
        
        if (g_jack_client && !g_bpm_state.transport_rolling.load()) {
            jack_transport_start(g_jack_client);
            g_bpm_state.transport_rolling.store(true);
            std::cout << "[MIDI] First clock received - auto-starting transport" << std::endl;
        }
        return;
    }
    
    count++;
    
    if (count >= PULSES_PER_QUARTER) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - g_bpm_state.last_pulse_time).count();
        
        if (elapsed > 0) {
            double raw_bpm = 60000000.0 / elapsed;
            raw_bpm = std::max(MIN_BPM, std::min(MAX_BPM, raw_bpm));
            
            double current = g_bpm_state.current_bpm.load();
            double smoothed_bpm;
            int mcount = g_bpm_state.measurement_count.load();
            
            if (mcount < 5 || std::abs(raw_bpm - current) > 10.0) {
                smoothed_bpm = current * 0.1 + raw_bpm * 0.9;
            } else if (mcount < 10 || std::abs(raw_bpm - current) > 3.0) {
                smoothed_bpm = current * 0.5 + raw_bpm * 0.5;
            } else {
                smoothed_bpm = current * (1.0 - SMOOTHING_FACTOR) + raw_bpm * SMOOTHING_FACTOR;
            }
            
            double final_bpm = snap_bpm(raw_bpm, smoothed_bpm);
            g_bpm_state.current_bpm.store(final_bpm);
            g_bpm_state.measurement_count++;
            
            update_jack_transport_bpm(final_bpm);
            
            std::string snap_indicator = (final_bpm == std::round(final_bpm)) ? " [LOCKED]" : "";
            std::cout << "[MIDI] " << g_bpm_state.bar << ":" << g_bpm_state.beat 
                      << " | BPM: " << std::fixed << std::setprecision(2) << final_bpm 
                      << " (raw: " << raw_bpm << ")" << snap_indicator << std::endl;
        }
        
        g_bpm_state.pulse_count.store(0);
        g_bpm_state.last_pulse_time = now;
        
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
                g_bpm_state.current_frame.store(0);
                g_bpm_state.bar.store(1);
                g_bpm_state.beat.store(1);
                g_bpm_state.tick.store(0);
                
                jack_position_t pos;
                pos.frame = 0;
                pos.valid = (jack_position_bits_t)0;
                jack_transport_reposition(g_jack_client, &pos);
                
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
    signal(SIGUSR2, reset_signal_handler);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << " MIDI Clock -> JACK Transport Sync " << std::endl;
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
        std::cout << "  Example: " << argv[0] << " 32:0" << std::endl;
        std::cout << "  Use 'aconnect -l' to list available ports" << std::endl;
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
    
    g_bpm_state.sample_rate = jack_get_sample_rate(g_jack_client);
    std::cout << "[JACK] Sample rate: " << g_bpm_state.sample_rate << " Hz" << std::endl;
    
    jack_set_process_callback(g_jack_client, jack_process_callback, nullptr);
    
    if (jack_set_timebase_callback(g_jack_client, 1, jack_timebase_callback, nullptr) == 0) {
        std::cout << "[JACK] Registered as timebase master" << std::endl;
    } else {
        std::cerr << "[WARN] Could not become timebase master" << std::endl;
    }
    
    if (jack_activate(g_jack_client) != 0) {
        std::cerr << "[ERROR] Cannot activate JACK client" << std::endl;
        jack_client_close(g_jack_client);
        snd_seq_close(g_seq_handle);
        return 1;
    }
    
    std::cout << "[JACK] Client activated successfully" << std::endl;
    
    // ========================================================================
    // SETUP NON-BLOCKING KEYBOARD INPUT
    // ========================================================================
    setup_terminal();
    
    std::thread cmd_thread(command_thread_func);
    cmd_thread.detach();
    
    // ========================================================================
    // MAIN LOOP
    // ========================================================================
    std::cout << "\n========================================" << std::endl;
    std::cout << "Waiting for MIDI Clock messages..." << std::endl;
    std::cout << "Transport will auto-start on first clock" << std::endl;
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║ Quick Commands (no Enter needed):     ║" << std::endl;
    std::cout << "╠════════════════════════════════════════╣" << std::endl;
    std::cout << "║ Press R       - Reset to beginning     ║" << std::endl;
    std::cout << "║ Press S       - Show status            ║" << std::endl;
    std::cout << "║ Press P/SPACE - Play/Pause toggle      ║" << std::endl;
    std::cout << "║ Press H       - Help                   ║" << std::endl;
    std::cout << "║ Press Q       - Quit                   ║" << std::endl;
    std::cout << "║                                        ║" << std::endl;
    std::cout << "║ Signal: kill -USR2 " << std::setw(5) << getpid() << " (reset)   ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;
    
    int npfds = snd_seq_poll_descriptors_count(g_seq_handle, POLLIN);
    struct pollfd pfds[npfds];
    snd_seq_poll_descriptors(g_seq_handle, pfds, npfds, POLLIN);
    
    snd_seq_event_t* ev = nullptr;
    
    while (g_running) {
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
    restore_terminal();
    
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

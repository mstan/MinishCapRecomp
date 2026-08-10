#pragma once

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace minish::foreign_world::zelda1 {

// Direct, bounded implementation of Z_00:DriveAudio/DriveSong for song $01.
// It is not a 6502 emulator.  All sequence bytes, period values and envelope
// values are read from the caller-owned, already validated PRG at runtime.
// The PRG must outlive this object (normally FirstQuestData does).
class Zelda1OverworldMusicRenderer {
public:
    static constexpr std::uint32_t kSampleRateHz = 65536;
    // This format deliberately contains all oscillator and sequencer state;
    // restore validates it before making any state visible.
    static constexpr std::size_t kSerializedSize = 79;

    bool bind_verified_prg(const VerifiedPrg& prg, std::string* error = nullptr);
    [[nodiscard]] bool ready() const { return !prg_.empty(); }

    // Starts song request $01 exactly as the next DriveAudio invocation does.
    void reset();
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const { return paused_; }

    // Fills signed 16-bit mono at exactly 65536 Hz. No allocation or I/O is
    // performed here. Rendering is deterministic across chunk boundaries.
    void render(std::span<std::int16_t> destination);

    [[nodiscard]] std::array<std::uint8_t, kSerializedSize> serialize() const;
    // Structural-only preflight for native save containers. It deliberately
    // does not dereference/bind a PRG; restore additionally verifies that the
    // live script fields agree with the caller's verified PRG.
    [[nodiscard]] static bool validate_serialized(std::span<const std::uint8_t> bytes);
    bool restore(std::span<const std::uint8_t> bytes, std::string* error = nullptr);

    // Source-facing trace state, useful for focused register-trace tests.
    [[nodiscard]] std::uint8_t phrase_index() const { return phrase_index_; }
    [[nodiscard]] std::uint8_t pulse1_period_lo() const { return pulse1_.period & 0xff; }
    [[nodiscard]] std::uint8_t pulse2_period_lo() const { return pulse2_.period & 0xff; }
    [[nodiscard]] std::uint8_t triangle_period_lo() const { return triangle_.period & 0xff; }
    [[nodiscard]] std::uint8_t noise_period_index() const { return noise_.period_index; }
    [[nodiscard]] std::uint8_t noise_volume() const { return noise_.volume; }

private:
    struct Pulse { std::uint16_t period = 0; std::uint16_t timer = 0; std::uint8_t duty = 0; std::uint8_t phase = 0; std::uint8_t volume = 0; std::uint8_t length = 0; };
    struct Triangle { std::uint16_t period = 0; std::uint16_t timer = 0; std::uint16_t note_period = 0; std::uint8_t phase = 0; std::uint8_t length = 0; std::uint8_t linear = 0; std::uint8_t linear_reload = 0; bool linear_reload_flag = false; bool linear_control = false; };
    struct Noise { std::uint16_t timer = 0; std::uint16_t shift = 1; std::uint8_t period_index = 0; std::uint8_t volume = 0; std::uint8_t length = 0; };

    bool prepare_next_phrase();
    void drive_song_frame();
    void drive_square1();
    void drive_square0();
    void drive_triangle();
    void drive_noise();
    void write_frame_counter_ff();
    void clock_quarter_frame();
    void clock_half_frame();
    void clock_cpu_cycle();
    [[nodiscard]] std::int16_t mixed_sample();
    [[nodiscard]] bool read(std::size_t offset, std::uint8_t* value) const;
    [[nodiscard]] bool script_read(std::uint8_t index, std::uint8_t* value) const;
    [[nodiscard]] std::uint8_t note_length(std::uint8_t control) const;
    [[nodiscard]] std::uint16_t note_period(std::uint8_t note) const;
    void emit_pulse(Pulse* pulse, std::uint8_t note, std::uint8_t volume);
    void emit_triangle(std::uint8_t note);

    std::span<const std::uint8_t> prg_{};
    Pulse pulse1_{}; // Z_00 Sq1 / $4004
    Pulse pulse2_{}; // Z_00 Sq0 / $4000
    Triangle triangle_{};
    Noise noise_{};
    bool paused_ = false;
    bool started_ = false;
    bool driver_fault_ = false;
    std::uint8_t phrase_index_ = 8;
    std::uint16_t script_base_ = 0;
    std::uint8_t length_table_base_ = 0;
    std::uint8_t envelope_selector_ = 0;
    std::uint8_t triangle_linear_source_ = 0;
    std::uint8_t sq1_offset_ = 0, sq0_offset_ = 0, triangle_offset_ = 0, noise_offset_ = 0, first_noise_offset_ = 0;
    std::uint8_t sq1_counter_ = 1, sq0_counter_ = 1, triangle_counter_ = 1, noise_counter_ = 1;
    // Z_00 keeps these latches separately from the decrementing counters;
    // raw (nonnegative) notes reload the counter from the preceding control
    // note's length.
    std::uint8_t sq1_note_length_ = 0, sq0_note_length_ = 0, triangle_note_length_ = 0;
    std::uint8_t sq1_envelope_ = 0, sq0_envelope_ = 0, triangle_repeat_ = 0, triangle_repeat_start_ = 0;
    std::uint8_t triangle_vibrato_ = 0;
    std::uint32_t sample_cpu_numerator_ = 0;
    std::uint32_t frame_dots_ = 0;
    std::uint32_t apu_half_cycles_ = 0;
    std::uint16_t frame_sequence_cycle_ = 0;
    std::uint8_t frame_reset_delay_ = 0;
    std::int32_t dc_estimate_ = 0;
};

}  // namespace minish::foreign_world::zelda1

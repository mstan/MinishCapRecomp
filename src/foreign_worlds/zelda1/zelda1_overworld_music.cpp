#include "foreign_worlds/zelda1/zelda1_overworld_music.h"

#include <algorithm>
#include <limits>

namespace minish::foreign_world::zelda1 {
namespace {
// PRG offsets are source addresses in bank 00 minus $8000. They name code/data
// locations only; no song, PCM, ROM, or rendered audio is carried by this TU.
constexpr std::size_t kSongTable = 0x0d60;
constexpr std::size_t kNotePeriods = 0x1f00;
constexpr std::size_t kNoteLengths = 0x1fd1;
constexpr std::size_t kCustomEnvelope = 0x1f92;
constexpr std::array<std::uint16_t, 16> kNoisePeriods{{4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068}};
constexpr std::array<std::uint8_t, 32> kApuLengths{{10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30}};
constexpr std::array<std::array<std::uint8_t, 8>, 4> kDuty{{{{0,1,0,0,0,0,0,0}}, {{0,1,1,0,0,0,0,0}}, {{0,1,1,1,1,0,0,0}}, {{1,0,0,1,1,1,1,1}}}};
constexpr std::array<std::uint8_t, 4> kNoiseVolume{{0x10, 0x1c, 0x1c, 0x1c}};
constexpr std::array<std::uint8_t, 4> kNoisePeriod{{0, 3, 10, 3}};
constexpr std::array<std::uint8_t, 4> kNoiseLength{{0, 0x18, 0x18, 0x58}};

void error(std::string* out, const char* message) { if (out) *out = message; }
std::uint8_t dec8(std::uint8_t value) { return static_cast<std::uint8_t>(value - 1); }
}  // namespace

bool Zelda1OverworldMusicRenderer::bind_verified_prg(const VerifiedPrg& prg, std::string* out_error) {
    const auto bytes = prg.bytes();
    if (bytes.size() != kVerifiedPrgSize || kSongTable + 36 > bytes.size() ||
        kNotePeriods + 142 > bytes.size() || kNoteLengths + 8 > bytes.size() ||
        kCustomEnvelope + 32 > bytes.size()) {
        error(out_error, "verified PRG lacks the Zelda 1 bank-00 audio layout");
        return false;
    }
    // The first OW phrase is reached by DriveSong: index 8 -> increment -> 9;
    // its table selector and 8-byte header must remain inside bank 00.
    const std::size_t selector = bytes[kSongTable + 8];
    if (kSongTable + selector + 8 > 0x4000 || bytes[kSongTable + selector + 2] != 0x8e) {
        error(out_error, "Zelda 1 overworld song header is not in bank 00");
        return false;
    }
    prg_ = bytes;
    reset();
    return true;
}

void Zelda1OverworldMusicRenderer::reset() {
    pulse1_ = {}; pulse2_ = {}; triangle_ = {}; noise_ = {};
    paused_ = false; started_ = false; driver_fault_ = false; phrase_index_ = 8;
    script_base_ = length_table_base_ = envelope_selector_ = triangle_linear_source_ = 0;
    sq1_offset_ = sq0_offset_ = triangle_offset_ = noise_offset_ = first_noise_offset_ = 0;
    sq1_counter_ = sq0_counter_ = triangle_counter_ = noise_counter_ = 1;
    sq1_note_length_ = sq0_note_length_ = triangle_note_length_ = 0;
    sq1_envelope_ = sq0_envelope_ = triangle_repeat_ = triangle_repeat_start_ = triangle_vibrato_ = 0;
    sample_cpu_numerator_ = frame_dots_ = apu_half_cycles_ = 0;
    frame_sequence_cycle_ = frame_reset_delay_ = 0; dc_estimate_ = 0;
}

void Zelda1OverworldMusicRenderer::set_paused(bool value) {
    if (paused_ == value) return;
    paused_ = value;
    if (paused_) pulse1_.length = pulse2_.length = triangle_.length = noise_.length = 0;
}

bool Zelda1OverworldMusicRenderer::read(std::size_t offset, std::uint8_t* value) const {
    if (offset >= prg_.size()) return false;
    *value = prg_[offset]; return true;
}
bool Zelda1OverworldMusicRenderer::script_read(std::uint8_t index, std::uint8_t* value) const {
    const auto offset = static_cast<std::size_t>(script_base_) + index;
    return offset < 0x4000 && read(offset, value);
}
std::uint8_t Zelda1OverworldMusicRenderer::note_length(std::uint8_t control) const {
    std::uint8_t value = 0;
    if (length_table_base_ + (control & 7) >= 24 ||
        !read(kNoteLengths + length_table_base_ + (control & 7), &value)) return 1;
    return value;
}
std::uint16_t Zelda1OverworldMusicRenderer::note_period(std::uint8_t note) const {
    std::uint8_t hi = 0, lo = 0;
    if (note > 140 || !read(kNotePeriods + note, &hi) || !read(kNotePeriods + note + 1, &lo)) return 0;
    return static_cast<std::uint16_t>(((hi | 8) & 7) << 8) | lo;
}

bool Zelda1OverworldMusicRenderer::prepare_next_phrase() {
    phrase_index_ = static_cast<std::uint8_t>(phrase_index_ + 1);
    // Z_00:PlayNextOverworldPhrase stores 9 on reaching 16 and falls through
    // PlayNextPhrase.  That falls-through increment loads phrase 10: phrase
    // 9 is the intro, never the loop's first phrase.
    if (phrase_index_ == 0x10) {
        phrase_index_ = 9;
        phrase_index_ = static_cast<std::uint8_t>(phrase_index_ + 1);
    }
    const std::size_t entry = kSongTable + static_cast<std::size_t>(phrase_index_ - 1);
    std::uint8_t selector = 0;
    if (!read(entry, &selector)) return false;
    const std::size_t header = kSongTable + selector;
    std::uint8_t lo = 0, hi = 0;
    if (header + 8 > 0x4000 || !read(header, &length_table_base_) || !read(header + 1, &lo) ||
        !read(header + 2, &hi) || hi < 0x80 || hi >= 0xc0 ||
        !read(header + 3, &triangle_offset_) || !read(header + 4, &sq0_offset_) ||
        !read(header + 5, &noise_offset_) || !read(header + 6, &envelope_selector_) ||
        !read(header + 7, &triangle_linear_source_)) return false;
    script_base_ = static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8) | lo) - 0x8000;
    first_noise_offset_ = noise_offset_;
    sq1_offset_ = 0; sq1_counter_ = sq0_counter_ = triangle_counter_ = noise_counter_ = 1;
    sq1_envelope_ = sq0_envelope_ = triangle_repeat_ = triangle_repeat_start_ = triangle_vibrato_ = 0;
    return true;
}

void Zelda1OverworldMusicRenderer::emit_pulse(Pulse* pulse, std::uint8_t note, std::uint8_t volume) {
    const auto period = note_period(note);
    if (!period) return; // EmitSquareNote leaves a rest's old registers intact.
    pulse->period = period; pulse->timer = period; pulse->phase = 0; pulse->duty = 2; pulse->volume = volume;
    pulse->length = kApuLengths[1]; // $4003/$4007 high byte is period-hi | $08.
}
void Zelda1OverworldMusicRenderer::emit_triangle(std::uint8_t note) {
    const auto period = note_period(note);
    if (!period) return;
    triangle_.period = triangle_.note_period = period;
    triangle_.timer = period; triangle_.phase = 0;
    triangle_.length = kApuLengths[1];
    triangle_.linear_reload_flag = true;
}

void Zelda1OverworldMusicRenderer::drive_square1() {
    sq1_counter_ = dec8(sq1_counter_);
    if (sq1_counter_ == 0) {
        std::uint8_t control = 0; if (!script_read(sq1_offset_++, &control)) { driver_fault_ = true; return; }
        if (control == 0) { if (!prepare_next_phrase()) driver_fault_ = true; drive_square1(); return; }
        if (control & 0x80) { sq1_note_length_ = note_length(control); if (!script_read(sq1_offset_++, &control)) { driver_fault_ = true; return; } }
        emit_pulse(&pulse1_, control, 0);
        sq1_counter_ = sq1_note_length_;
        sq1_envelope_ = 0x20;
    }
    if (sq1_envelope_) --sq1_envelope_;
    std::uint8_t env = 0;
    if (read(kCustomEnvelope + sq1_envelope_, &env)) pulse1_.volume = (env >> 4) & 15;
}

void Zelda1OverworldMusicRenderer::drive_square0() {
    if (!sq0_offset_) return;
    sq0_counter_ = dec8(sq0_counter_);
    if (sq0_counter_ == 0) {
        std::uint8_t control = 0; if (!script_read(sq0_offset_++, &control)) { driver_fault_ = true; return; }
        if (control & 0x80) { sq0_note_length_ = note_length(control); if (!script_read(sq0_offset_++, &control)) { driver_fault_ = true; return; } }
        emit_pulse(&pulse2_, control, 0); sq0_counter_ = sq0_note_length_; sq0_envelope_ = 0x20;
    }
    if (sq0_envelope_) --sq0_envelope_;
    std::uint8_t env = 0;
    if (read(kCustomEnvelope + sq0_envelope_, &env)) pulse2_.volume = (env >> 4) & 15;
}

void Zelda1OverworldMusicRenderer::drive_triangle() {
    if (!triangle_offset_) return;
    triangle_counter_ = dec8(triangle_counter_);
    if (triangle_counter_ == 0) {
        for (unsigned guard = 0; guard != 256; ++guard) {
            std::uint8_t control = 0; if (!script_read(triangle_offset_++, &control)) { driver_fault_ = true; return; }
            if (control == 0) { triangle_.linear = 0; return; }
            if (control >= 0xf1) { triangle_repeat_ = control - 0xf0; triangle_repeat_start_ = triangle_offset_; continue; }
            if (control == 0xf0) { if (--triangle_repeat_) triangle_offset_ = triangle_repeat_start_; continue; }
            if (control & 0x80) { triangle_note_length_ = note_length(control); if (!script_read(triangle_offset_++, &control)) { driver_fault_ = true; return; } }
            emit_triangle(control); triangle_counter_ = triangle_note_length_; break;
        }
    }
    // Z_00 applies VibratePitch on every song tick, including the first 15
    // (where it is a no-op) and after a rest.  Only $400A is rewritten: the
    // high timer bits and length counter retain the emitted note's state.
    triangle_vibrato_ = static_cast<std::uint8_t>(triangle_vibrato_ + 1);
    std::uint8_t low = static_cast<std::uint8_t>(triangle_.note_period);
    if (triangle_vibrato_ >= 0x10)
        low = static_cast<std::uint8_t>(low + ((triangle_vibrato_ & 4) ? -1 : 1));
    triangle_.period = static_cast<std::uint16_t>((triangle_.period & 0x0700) | low);

    // Header byte 7 is copied to source RAM $05f1. @ApplyTrgEffects maps a
    // negative byte to $1f and all other values to $ff before writing $4008.
    // This is a register write, not a linear-counter reload request. OW's
    // canonical $80 therefore selects $1f, while synthetic fixtures can also
    // cover the $ff/control-bit path.
    if (triangle_linear_source_ & 0x80) {
        triangle_.linear_reload = 0x1f;
        triangle_.linear_control = false;
    } else {
        triangle_.linear_reload = 0x7f;
        triangle_.linear_control = true;
    }
}

void Zelda1OverworldMusicRenderer::drive_noise() {
    if (!noise_offset_) return;
    noise_counter_ = dec8(noise_counter_);
    if (noise_counter_ == 0) {
        std::uint8_t control = 0; if (!script_read(noise_offset_++, &control)) { driver_fault_ = true; return; }
        if (control == 0) { noise_offset_ = first_noise_offset_; if (!script_read(noise_offset_++, &control)) { driver_fault_ = true; return; } }
        // Z_00 GetSongNoiseNoteLength rotates bits 0,7,6 into index bits 2,1,0.
        const auto rotated = static_cast<std::uint8_t>(((control & 1) << 2) | ((control >> 6) & 3));
        noise_counter_ = note_length(rotated);
        const std::uint8_t index = static_cast<std::uint8_t>((control & 0x3e) >> 4);
        noise_.volume = kNoiseVolume[index] & 15;
        // $400e changes the divider period but does not reload the divider.
        // The noise oscillator itself advances on the CPU clock in this
        // bounded model, rather than at output-sample or pulse-half rate.
        noise_.period_index = kNoisePeriod[index];
        noise_.length = kApuLengths[(kNoiseLength[index] >> 3) & 31];
    }
}

void Zelda1OverworldMusicRenderer::drive_song_frame() {
    if (paused_ || driver_fault_) return;
    write_frame_counter_ff();
    if (!started_) { started_ = true; if (!prepare_next_phrase()) { driver_fault_ = true; return; } }
    drive_square1(); if (driver_fault_) return;
    drive_square0(); drive_triangle(); drive_noise();
}

void Zelda1OverworldMusicRenderer::write_frame_counter_ff() {
    // $4017=$ff selects five-step mode. The divider reset takes three or four
    // CPU cycles depending on APU parity, then immediately clocks Q+H.
    frame_reset_delay_ = static_cast<std::uint8_t>((apu_half_cycles_ & 1) ? 3 : 4);
}

void Zelda1OverworldMusicRenderer::clock_quarter_frame() {
    if (triangle_.linear_reload_flag) triangle_.linear = triangle_.linear_reload;
    else if (triangle_.linear) --triangle_.linear;
    if (!triangle_.linear_control) triangle_.linear_reload_flag = false;
}

void Zelda1OverworldMusicRenderer::clock_half_frame() {
    const auto tick = [](std::uint8_t* length) { if (*length) --*length; };
    tick(&pulse1_.length); tick(&pulse2_.length);
    if (!triangle_.linear_control) tick(&triangle_.length);
    tick(&noise_.length);
}

void Zelda1OverworldMusicRenderer::clock_cpu_cycle() {
    ++apu_half_cycles_;
    if (frame_reset_delay_) {
        if (--frame_reset_delay_ == 0) {
            frame_sequence_cycle_ = 0;
            clock_quarter_frame();
            clock_half_frame();
        }
    } else {
        // $4017=$ff selects the NTSC five-step sequence: Q, Q+H, Q, Q+H,
        // then an idle/reset step.  DriveAudio writes it once per video frame,
        // but modelling the complete hardware sequence makes length and
        // triangle-linear state correct between writes as well.
        ++frame_sequence_cycle_;
        if (frame_sequence_cycle_ == 3729 || frame_sequence_cycle_ == 11186)
            clock_quarter_frame();
        else if (frame_sequence_cycle_ == 7457 || frame_sequence_cycle_ == 14915) {
            clock_quarter_frame();
            clock_half_frame();
        } else if (frame_sequence_cycle_ == 18641) {
            frame_sequence_cycle_ = 0;
        }
    }
    if (triangle_.timer == 0) { triangle_.timer = triangle_.period; triangle_.phase = static_cast<std::uint8_t>((triangle_.phase + 1) & 31); } else --triangle_.timer;
    // Noise clocks every CPU cycle, and reloads before its next full period.
    if (noise_.timer == 0) {
        noise_.timer = kNoisePeriods[noise_.period_index];
        const auto feedback = static_cast<std::uint16_t>((noise_.shift ^ (noise_.shift >> 1)) & 1);
        noise_.shift = static_cast<std::uint16_t>((noise_.shift >> 1) | (feedback << 14));
    } else --noise_.timer;
    if ((apu_half_cycles_ & 1) == 0) {
        auto step_pulse = [](Pulse* p) { if (p->timer == 0) { p->timer = p->period; p->phase = static_cast<std::uint8_t>((p->phase + 1) & 7); } else --p->timer; };
        step_pulse(&pulse1_); step_pulse(&pulse2_);
    }
    frame_dots_ += 3;
    if (frame_dots_ >= 89342) { frame_dots_ -= 89342; drive_song_frame(); }
}

std::int16_t Zelda1OverworldMusicRenderer::mixed_sample() {
    const auto pulse = [](const Pulse& p) { return p.length && p.period >= 8 && kDuty[p.duty & 3][p.phase] ? p.volume : 0; };
    const int p = pulse(pulse1_) + pulse(pulse2_);
    const int t = triangle_.length && triangle_.linear ? (triangle_.phase < 16 ? 15 - triangle_.phase : triangle_.phase - 16) : 0;
    const int n = noise_.length && !(noise_.shift & 1) ? noise_.volume : 0;
    const double pulse_mix = p ? 95.88 / ((8128.0 / p) + 100.0) : 0.0;
    const double tnd_mix = (t || n) ? 159.79 / (1.0 / (t / 8227.0 + n / 12241.0) + 100.0) : 0.0;
    const auto input = static_cast<std::int32_t>((pulse_mix + tnd_mix) * 32767.0);
    dc_estimate_ += (input - dc_estimate_) >> 9; // deterministic 1-pole DC blocker
    const auto result = std::clamp(input - dc_estimate_, -32768, 32767);
    return static_cast<std::int16_t>(result);
}

void Zelda1OverworldMusicRenderer::render(std::span<std::int16_t> destination) {
    if (!ready()) { std::fill(destination.begin(), destination.end(), 0); return; }
    for (auto& sample : destination) {
        // NTSC CPU = 78,750,000 / 44 Hz.  Each output sample advances that
        // rational clock exactly, avoiding 60 Hz drift at the fixed 65536 rate.
        sample_cpu_numerator_ += 78750000;
        constexpr std::uint32_t kDenominator = 44 * kSampleRateHz;
        while (sample_cpu_numerator_ >= kDenominator) { sample_cpu_numerator_ -= kDenominator; clock_cpu_cycle(); }
        sample = paused_ ? 0 : mixed_sample();
    }
}

std::array<std::uint8_t, Zelda1OverworldMusicRenderer::kSerializedSize> Zelda1OverworldMusicRenderer::serialize() const {
    std::array<std::uint8_t, kSerializedSize> out{}; std::size_t at = 0;
    auto u8 = [&](std::uint8_t v) { out[at++] = v; };
    auto u16 = [&](std::uint16_t v) { u8(v & 255); u8(v >> 8); };
    auto u32 = [&](std::uint32_t v) { u16(v & 0xffff); u16(v >> 16); };
    auto pulse = [&](const Pulse& p) { u16(p.period); u16(p.timer); u8(p.duty); u8(p.phase); u8(p.volume); u8(p.length); };
    pulse(pulse1_); pulse(pulse2_);
    u16(triangle_.period); u16(triangle_.timer); u16(triangle_.note_period);
    u8(triangle_.phase); u8(triangle_.length); u8(triangle_.linear);
    u8(triangle_.linear_reload); u8(triangle_.linear_reload_flag ? 1 : 0); u8(triangle_.linear_control ? 1 : 0);
    u16(noise_.timer); u16(noise_.shift); u8(noise_.period_index);
    u8(noise_.volume); u8(noise_.length);
    u8(paused_ ? 1 : 0); u8(started_ ? 1 : 0); u8(driver_fault_ ? 1 : 0);
    u8(phrase_index_); u16(script_base_); u8(length_table_base_); u8(envelope_selector_);
    u8(sq1_offset_); u8(sq0_offset_); u8(triangle_offset_); u8(noise_offset_); u8(first_noise_offset_);
    u8(sq1_counter_); u8(sq0_counter_); u8(triangle_counter_); u8(noise_counter_);
    u8(sq1_note_length_); u8(sq0_note_length_); u8(triangle_note_length_);
    u8(sq1_envelope_); u8(sq0_envelope_); u8(triangle_repeat_); u8(triangle_repeat_start_); u8(triangle_vibrato_);
    u32(sample_cpu_numerator_); u32(frame_dots_); u32(apu_half_cycles_);
    u16(frame_sequence_cycle_); u8(frame_reset_delay_); u32(static_cast<std::uint32_t>(dc_estimate_));
    return out;
}

bool Zelda1OverworldMusicRenderer::validate_serialized(std::span<const std::uint8_t> in) {
    if (in.size() != kSerializedSize) return false;
    std::size_t at = 0;
    const auto u8 = [&]() { return in[at++]; };
    const auto u16 = [&]() { const auto lo = u8(); return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(u8()) << 8)); };
    const auto u32 = [&]() { const auto lo = u16(); return static_cast<std::uint32_t>(lo | (static_cast<std::uint32_t>(u16()) << 16)); };
    const auto pulse_valid = [&]() {
        const auto period = u16(), timer = u16();
        const auto duty = u8(), phase = u8(), volume = u8();
        (void)u8(); // length counter is a complete 8-bit hardware register.
        return period <= 0x7ff && timer <= 0x7ff && duty <= 3 && phase <= 7 && volume <= 15;
    };
    if (!pulse_valid() || !pulse_valid()) return false;
    const auto triangle_period = u16(), triangle_timer = u16(), triangle_note = u16();
    const auto triangle_phase = u8(), triangle_length = u8(), triangle_linear = u8();
    const auto triangle_reload = u8(), triangle_flag = u8(), triangle_control = u8();
    (void)triangle_length;
    const auto noise_timer = u16(), noise_shift = u16();
    const auto noise_index = u8(), noise_volume = u8(), noise_length = u8();
    (void)noise_length;
    const auto paused = u8(), started = u8(), fault = u8();
    const auto phrase = u8(); const auto script_base = u16(); const auto length_base = u8(); const auto envelope = u8();
    const auto sq1_offset = u8(), sq0_offset = u8(), triangle_offset = u8(), noise_offset = u8(), first_noise = u8();
    (void)u8(); (void)u8(); (void)u8(); (void)u8(); // decrementing source note counters
    (void)u8(); (void)u8(); (void)u8(); // persistent source note-length latches
    const auto sq1_envelope = u8(), sq0_envelope = u8(), triangle_repeat = u8(), triangle_repeat_start = u8();
    (void)triangle_repeat_start; (void)u8(); // triangle vibrato counter
    const auto sample_numerator = u32(), frame_dots = u32();
    (void)u32(); // CPU-cycle parity counter
    const auto frame_sequence = u16(); const auto frame_reset_delay = u8();
    (void)u32(); // DC blocker accumulator is signed and unrestricted.
    if (at != in.size() || paused > 1 || started > 1 || fault > 1 || triangle_flag > 1 || triangle_control > 1 ||
        triangle_period > 0x7ff || triangle_timer > 0x7ff || triangle_note > 0x7ff || triangle_phase > 31 ||
        triangle_linear > 0x7f || triangle_reload > 0x7f || noise_timer > kNoisePeriods.back() ||
        noise_shift == 0 || (noise_shift & 0x8000) != 0 || noise_index > 15 || noise_volume > 15 ||
        sq1_envelope > 0x20 || sq0_envelope > 0x20 || triangle_repeat > 15 || frame_dots >= 89342 ||
        sample_numerator >= 44 * kSampleRateHz || frame_sequence >= 18641 || frame_reset_delay > 4)
        return false;
    if (!started)
        return phrase == 8 && script_base == 0 && length_base == 0 && envelope == 0 && sq1_offset == 0 &&
            sq0_offset == 0 && triangle_offset == 0 && noise_offset == 0 && first_noise == 0;
    return phrase >= 9 && phrase <= 15 && script_base < 0x4000 && length_base + 7 < 24 &&
        static_cast<std::size_t>(script_base) + sq1_offset < 0x4000 &&
        static_cast<std::size_t>(script_base) + sq0_offset < 0x4000 &&
        static_cast<std::size_t>(script_base) + triangle_offset < 0x4000 &&
        static_cast<std::size_t>(script_base) + noise_offset < 0x4000;
}

bool Zelda1OverworldMusicRenderer::restore(std::span<const std::uint8_t> in, std::string* out_error) {
    if (!ready() || !validate_serialized(in)) { error(out_error, "invalid Zelda 1 overworld music state"); return false; }
    Zelda1OverworldMusicRenderer candidate = *this;
    std::size_t at = 0;
    const auto u8 = [&]() { return in[at++]; };
    const auto u16 = [&]() { const auto lo = u8(); return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(u8()) << 8)); };
    const auto u32 = [&]() { const auto lo = u16(); return static_cast<std::uint32_t>(lo | (static_cast<std::uint32_t>(u16()) << 16)); };
    const auto pulse = [&](Pulse* p) { p->period=u16(); p->timer=u16(); p->duty=u8(); p->phase=u8(); p->volume=u8(); p->length=u8(); };
    pulse(&candidate.pulse1_); pulse(&candidate.pulse2_);
    candidate.triangle_.period=u16(); candidate.triangle_.timer=u16(); candidate.triangle_.note_period=u16();
    candidate.triangle_.phase=u8(); candidate.triangle_.length=u8(); candidate.triangle_.linear=u8();
    candidate.triangle_.linear_reload=u8(); const auto linear_flag=u8(); const auto linear_control=u8();
    candidate.noise_.timer=u16(); candidate.noise_.shift=u16(); candidate.noise_.period_index=u8(); candidate.noise_.volume=u8(); candidate.noise_.length=u8();
    const auto paused=u8(), started=u8(), fault=u8();
    candidate.phrase_index_=u8(); candidate.script_base_=u16(); candidate.length_table_base_=u8(); candidate.envelope_selector_=u8();
    candidate.sq1_offset_=u8(); candidate.sq0_offset_=u8(); candidate.triangle_offset_=u8(); candidate.noise_offset_=u8(); candidate.first_noise_offset_=u8();
    candidate.sq1_counter_=u8(); candidate.sq0_counter_=u8(); candidate.triangle_counter_=u8(); candidate.noise_counter_=u8();
    candidate.sq1_note_length_=u8(); candidate.sq0_note_length_=u8(); candidate.triangle_note_length_=u8();
    candidate.sq1_envelope_=u8(); candidate.sq0_envelope_=u8(); candidate.triangle_repeat_=u8(); candidate.triangle_repeat_start_=u8(); candidate.triangle_vibrato_=u8();
    candidate.sample_cpu_numerator_=u32(); candidate.frame_dots_=u32(); candidate.apu_half_cycles_=u32(); candidate.frame_sequence_cycle_=u16(); candidate.frame_reset_delay_=u8(); candidate.dc_estimate_=static_cast<std::int32_t>(u32());
    candidate.paused_=paused != 0; candidate.started_=started != 0; candidate.driver_fault_=fault != 0;
    candidate.triangle_.linear_reload_flag=linear_flag != 0;
    candidate.triangle_.linear_control=linear_control != 0;
    // A $400a/$400e write changes a timer's period register without resetting
    // its divider.  The live timer may therefore be greater than the current
    // period (after triangle vibrato or a shorter noise period); validate the
    // hardware counter range rather than an invalid timer<=period invariant.
    const auto bad_pulse = [](const Pulse& p) { return p.period > 0x7ff || p.timer > 0x7ff || p.duty > 3 || p.phase > 7 || p.volume > 15; };
    bool valid = at == in.size() && paused <= 1 && started <= 1 && fault <= 1 && linear_flag <= 1 && linear_control <= 1 &&
        !bad_pulse(candidate.pulse1_) && !bad_pulse(candidate.pulse2_) && candidate.triangle_.period <= 0x7ff && candidate.triangle_.note_period <= 0x7ff && candidate.triangle_.timer <= 0x7ff && candidate.triangle_.phase <= 31 && candidate.triangle_.linear <= 0x7f && candidate.triangle_.linear_reload <= 0x7f && candidate.noise_.shift != 0 && (candidate.noise_.shift & 0x8000) == 0 && candidate.noise_.period_index <= 15 && candidate.noise_.volume <= 15 && candidate.noise_.timer <= kNoisePeriods.back() && candidate.sq1_envelope_ <= 0x20 && candidate.sq0_envelope_ <= 0x20 && candidate.triangle_repeat_ <= 15 && candidate.frame_dots_ < 89342 && candidate.sample_cpu_numerator_ < 44 * kSampleRateHz && candidate.frame_sequence_cycle_ < 18641 && candidate.frame_reset_delay_ <= 4;
    if (valid && candidate.started_) {
        valid = candidate.phrase_index_ >= 9 && candidate.phrase_index_ <= 15 && candidate.length_table_base_ + 7 < 24;
        // Tie restored script state back to the verified source phrase header,
        // rather than accepting any in-range PRG address or header fields.
        const auto entry = kSongTable + static_cast<std::size_t>(candidate.phrase_index_ - 1);
        std::uint8_t selector = 0, length_base = 0, lo = 0, hi = 0, trg = 0, sq0 = 0, noise = 0, envelope = 0, triangle_source = 0;
        const bool header_read = valid && read(entry, &selector) &&
            read(kSongTable + selector, &length_base) && read(kSongTable + selector + 1, &lo) &&
            read(kSongTable + selector + 2, &hi) && read(kSongTable + selector + 3, &trg) &&
            read(kSongTable + selector + 4, &sq0) && read(kSongTable + selector + 5, &noise) &&
            read(kSongTable + selector + 6, &envelope) && read(kSongTable + selector + 7, &triangle_source);
        const auto expected_base = static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8) | lo) - 0x8000;
        valid = header_read && hi >= 0x80 && hi < 0xc0 && candidate.script_base_ == expected_base &&
            candidate.length_table_base_ == length_base && candidate.envelope_selector_ == envelope &&
            candidate.first_noise_offset_ == noise && candidate.sq0_offset_ >= sq0 && candidate.triangle_offset_ >= trg && candidate.noise_offset_ >= noise &&
            static_cast<std::size_t>(candidate.script_base_) + candidate.sq1_offset_ < 0x4000 &&
            static_cast<std::size_t>(candidate.script_base_) + candidate.sq0_offset_ < 0x4000 &&
            static_cast<std::size_t>(candidate.script_base_) + candidate.triangle_offset_ < 0x4000 &&
            static_cast<std::size_t>(candidate.script_base_) + candidate.noise_offset_ < 0x4000;
        candidate.triangle_linear_source_ = triangle_source;
    } else if (valid) {
        valid = candidate.phrase_index_ == 8 && candidate.script_base_ == 0 && candidate.length_table_base_ == 0 &&
            candidate.sq1_note_length_ == 0 && candidate.sq0_note_length_ == 0 && candidate.triangle_note_length_ == 0;
        candidate.triangle_linear_source_ = 0;
    }
    if (!valid) { error(out_error, "out-of-range Zelda 1 overworld music state"); return false; }
    *this = candidate;
    return true;
}

}  // namespace minish::foreign_world::zelda1

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_overworld_music.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;
namespace {
// Fixed offsets in the explicitly versionless, fixed-size renderer state.
// Keeping names here makes adversarial restore checks resilient to additions
// in the test fixture above them.
constexpr std::size_t kNoiseTimer = 28;
constexpr std::size_t kNoiseShift = 30;
constexpr std::size_t kNoisePeriodIndex = 32;
constexpr std::size_t kPaused = 35;
constexpr std::size_t kSq1NoteLength = 52;
constexpr std::size_t kSq1Envelope = 55;
constexpr std::size_t kSampleCpuNumerator = 60;
constexpr std::size_t kFrameResetDelay = 74;
int fail(const char* message) { std::cerr << "FAIL: " << message << '\n'; return 1; }
void put16(std::ofstream& out, std::uint16_t v) { out.put(v & 255); out.put(v >> 8); }
void put32(std::ofstream& out, std::uint32_t v) { put16(out, v & 0xffff); put16(out, v >> 16); }
void write_artifacts(const std::vector<std::int16_t>& pcm, const char* wav_name,
                     const char* spectrum_name) {
    std::ofstream wav(wav_name, std::ios::binary);
    wav.write("RIFF", 4); put32(wav, 36 + static_cast<std::uint32_t>(pcm.size() * 2)); wav.write("WAVEfmt ", 8);
    put32(wav, 16); put16(wav, 1); put16(wav, 1); put32(wav, z1::Zelda1OverworldMusicRenderer::kSampleRateHz);
    put32(wav, z1::Zelda1OverworldMusicRenderer::kSampleRateHz * 2); put16(wav, 2); put16(wav, 16); wav.write("data", 4); put32(wav, static_cast<std::uint32_t>(pcm.size() * 2));
    wav.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    // A compact amplitude spectrogram proxy accompanies the WAV for CI/manual
    // inspection. It intentionally derives only from this synthetic test PRG.
    std::ofstream pgm(spectrum_name, std::ios::binary);
    pgm << "P5\n256 64\n255\n";
    for (unsigned y = 0; y != 64; ++y) for (unsigned x = 0; x != 256; ++x) {
        std::int32_t peak = 0; const std::size_t begin = x * pcm.size() / 256;
        const std::size_t end = (x + 1) * pcm.size() / 256;
        for (std::size_t i = begin; i < end; ++i) peak = std::max(peak, std::abs(static_cast<std::int32_t>(pcm[i])));
        const unsigned level = static_cast<unsigned>(peak >> 8);
        pgm.put(static_cast<char>(y >= 63 - std::min(63u, level / 4) ? 255 : 0));
    }
}

bool validate_optional_canonical_prg() {
    const char* path = std::getenv("ZELDA1_PRG0_INES");
    if (!path || !*path) return true;
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(input)), {});
    if (image.size() != z1::kVerifiedPrgSize + 16 || image[0] != 'N' || image[1] != 'E' || image[2] != 'S' || image[3] != 0x1a) return false;
    image.erase(image.begin(), image.begin() + 16);
    std::string error;
    auto prg = z1::VerifiedPrg::from_verified_bytes(std::move(image), &error);
    if (!prg) return false;
    z1::Zelda1OverworldMusicRenderer music;
    if (!music.bind_verified_prg(*prg, &error)) return false;
    // Follow actual source phrase transitions rather than treating nonzero
    // register writes as proof of a correct song.  The direct DriveSong
    // translation must traverse the intro 9..15 and then loop at 10.
    std::array<std::int16_t, 1024> trace_pcm{};
    std::vector<std::uint8_t> phrase_trace;
    std::uint8_t previous = 8;
    for (unsigned chunk = 0; chunk != 90 * 64 && phrase_trace.size() != 8; ++chunk) {
        music.render(trace_pcm);
        if (music.phrase_index() != previous) {
            phrase_trace.push_back(music.phrase_index());
            previous = music.phrase_index();
        }
    }
    constexpr std::array<std::uint8_t, 8> kExpectedPhraseTrace{{9, 10, 11, 12, 13, 14, 15, 10}};
    const bool trace_ok = phrase_trace.size() == kExpectedPhraseTrace.size() &&
        std::equal(phrase_trace.begin(), phrase_trace.end(), kExpectedPhraseTrace.begin());
    if (!trace_ok) {
        std::cerr << "canonical phrases:";
        for (const auto phrase : phrase_trace) std::cerr << ' ' << unsigned(phrase);
        std::cerr << '\n';
    }
    std::vector<std::int16_t> pcm(z1::Zelda1OverworldMusicRenderer::kSampleRateHz * 8);
    music.render(pcm);
    if (trace_ok) write_artifacts(pcm, "zelda1_overworld_music_canonical.wav",
                                  "zelda1_overworld_music_canonical_spectrogram.pgm");
    return trace_ok;
}
}

int main() {
    // This contains deliberately invented test cues, not Zelda song/PCM/ROM
    // data. It proves the production renderer obtains every cue at runtime.
    std::vector<std::uint8_t> bytes(z1::kVerifiedPrgSize, 1);
    constexpr std::size_t table = 0x0d60, header = 0x0dd5, header2 = 0x0ddd, script = 0x0e70, script2 = 0x0f80;
    bytes[table + 8] = static_cast<std::uint8_t>(header - table);
    bytes[header] = 0; bytes[header + 1] = 0x70; bytes[header + 2] = 0x8e;
    bytes[header + 3] = 8; bytes[header + 4] = 4; bytes[header + 5] = 12; bytes[header + 6] = 1;
    bytes[table + 9] = static_cast<std::uint8_t>(header2 - table);
    bytes[header2] = 0; bytes[header2 + 1] = 0x80; bytes[header2 + 2] = 0x8f;
    bytes[header2 + 3] = 8; bytes[header2 + 4] = 4; bytes[header2 + 5] = 12; bytes[header2 + 6] = 1;
    bytes[script + 0] = 0x83; bytes[script + 1] = 0x10; bytes[script + 2] = 0;
    bytes[script + 4] = 0x83; bytes[script + 5] = 0x14; bytes[script + 6] = 0x16;
    bytes[script + 8] = 0x83; bytes[script + 9] = 0x18; bytes[script + 10] = 0x1a;
    bytes[script + 12] = 0x83;
    // Phrase 10 deliberately has distinct long-running tones/noise.  The
    // boundary check below catches an implementation that delays Sq0,
    // triangle, or noise until a later DriveSong call after Sq1 ends phrase 9.
    bytes[script2 + 0] = 0x87; bytes[script2 + 1] = 0x1a;
    bytes[script2 + 2] = 0x18; // raw note: reuse the preceding $87 length.
    bytes[script2 + 4] = 0x87; bytes[script2 + 5] = 0x16;
    bytes[script2 + 8] = 0x87; bytes[script2 + 9] = 0x12;
    bytes[script2 + 12] = 0x94;
    bytes[0x1fd1 + 2] = 0x40; bytes[0x1fd1 + 3] = 4;
    bytes[0x1fd1 + 6] = 4; bytes[0x1fd1 + 7] = 0x40;
    for (std::uint8_t n : {0x10,0x12,0x14,0x16,0x18,0x1a}) { bytes[0x1f00 + n] = 0; bytes[0x1f00 + n + 1] = static_cast<std::uint8_t>(0x40 + n); }
    for (unsigned i = 0; i != 32; ++i) bytes[0x1f92 + i] = 0xf0;
    std::string error;
    auto prg = z1::VerifiedPrg::from_verified_bytes(std::move(bytes), &error);
    if (!prg) return fail("synthetic verified PRG rejected");
    z1::Zelda1OverworldMusicRenderer all_at_once, chunked;
    if (!all_at_once.bind_verified_prg(*prg, &error) || !chunked.bind_verified_prg(*prg, &error)) return fail("music bind rejected synthetic layout");
    z1::Zelda1OverworldMusicRenderer phrase_boundary;
    if (!phrase_boundary.bind_verified_prg(*prg, &error)) return fail("phrase-boundary renderer bind failed");
    // Five source ticks reaches the Sq1 zero terminator.  Z_00 immediately
    // falls through PlayNextPhrase and services all remaining song channels
    // from phrase 10 in that *same* DriveSong invocation.
    std::vector<std::int16_t> boundary_pcm(5500);
    phrase_boundary.render(boundary_pcm);
    if (phrase_boundary.phrase_index() != 10 || phrase_boundary.pulse1_period_lo() != 0x5a ||
        phrase_boundary.pulse2_period_lo() != 0x56 || phrase_boundary.triangle_period_lo() != 0x52 ||
        phrase_boundary.noise_period_index() != 3 || phrase_boundary.noise_volume() != 12)
        return fail("phrase boundary did not service every channel from phrase 10");
    // The phrase-10 raw Sq1 note above must reload from the preceding $87
    // latch, rather than leave a zero counter that wraps to $ff next tick.
    std::vector<std::int16_t> raw_note_pcm(80000);
    phrase_boundary.render(raw_note_pcm);
    if (phrase_boundary.phrase_index() != 10 || phrase_boundary.pulse1_period_lo() != 0x58 ||
        phrase_boundary.serialize()[kSq1NoteLength] != 0x40)
        return fail("raw song note did not retain/reload its channel note length");
    std::vector<std::int16_t> one(32768), split(32768);
    all_at_once.render(one);
    chunked.render(std::span<std::int16_t>(split).first(997));
    const auto checkpoint = chunked.serialize();
    z1::Zelda1OverworldMusicRenderer restored;
    if (!restored.bind_verified_prg(*prg, &error) || !restored.restore(checkpoint, &error)) return fail("strict state restore failed");
    restored.render(std::span<std::int16_t>(split).subspan(997));
    if (one != split) return fail("rendering changed across serialization boundary");
    if (all_at_once.phrase_index() != 10 || all_at_once.pulse1_period_lo() == 0 || all_at_once.pulse2_period_lo() == 0 || all_at_once.triangle_period_lo() == 0) return fail("DriveSong OW phrase/register trace diverged");
    bool audible = false; for (const auto sample : one) audible |= sample != 0;
    if (!audible) return fail("runtime-derived synthetic cue was silent");
    // Restore is strictly transactional: every rejected byte pattern leaves
    // the active renderer bit-for-bit unchanged.
    const auto stable = all_at_once.serialize();
    if (!z1::Zelda1OverworldMusicRenderer::validate_serialized(stable))
        return fail("structural music-state preflight rejected a live state");
    auto zero_lfsr = stable; zero_lfsr[kNoiseShift] = zero_lfsr[kNoiseShift + 1] = 0;
    if (z1::Zelda1OverworldMusicRenderer::validate_serialized(zero_lfsr))
        return fail("structural preflight accepted zero noise LFSR");
    if (all_at_once.restore(zero_lfsr, &error)) return fail("zero noise LFSR accepted");
    if (all_at_once.serialize() != stable) return fail("zero-LFSR restore mutated renderer");
    for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint8_t>, 4>{{
             {kPaused, 2},       // non-canonical paused bool
             {kSq1Envelope, 0xff},    // envelope cursor outside Z_00's $20 range
             {1, 0xff},     // pulse period above 11-bit APU range
             {kFrameResetDelay, 5},       // invalid $4017 delayed-reset countdown
         }}) {
        auto corrupt = stable; corrupt[offset] = value;
        if (all_at_once.restore(corrupt, &error)) return fail("corrupt music blob accepted");
        if (all_at_once.serialize() != stable) return fail("failed restore mutated renderer");
    }
    // The noise divider is CPU-clocked (not pulse half-rate). Set its period
    // index 0 and timer to zero, then one 65536-Hz sample advances 27 CPU
    // cycles. The timer's reload-plus-decrement convention shifts at cycles
    // 1,6,11,16,21,26: exactly six LFSR steps.
    auto noise_state = stable;
    noise_state[kNoiseTimer] = noise_state[kNoiseTimer + 1] = 0;
    noise_state[kNoiseShift] = 1; noise_state[kNoiseShift + 1] = 0;
    noise_state[kNoisePeriodIndex] = 0;
    noise_state[kSampleCpuNumerator] = noise_state[kSampleCpuNumerator + 1] = noise_state[kSampleCpuNumerator + 2] = noise_state[kSampleCpuNumerator + 3] = 0;
    if (!all_at_once.restore(noise_state, &error)) return fail("noise cadence fixture rejected");
    std::array<std::int16_t, 1> one_sample{}; all_at_once.render(one_sample);
    auto advanced = all_at_once.serialize(); std::uint16_t expected_shift = 1;
    for (unsigned i = 0; i != 6; ++i) {
        const auto feedback = static_cast<std::uint16_t>((expected_shift ^ (expected_shift >> 1)) & 1);
        expected_shift = static_cast<std::uint16_t>((expected_shift >> 1) | (feedback << 14));
    }
    const auto actual_shift = static_cast<std::uint16_t>(advanced[kNoiseShift] | (static_cast<std::uint16_t>(advanced[kNoiseShift + 1]) << 8));
    if (actual_shift != expected_shift) return fail("noise divider was not clocked once per CPU cycle");
    all_at_once.set_paused(true); std::array<std::int16_t, 32> silence{}; all_at_once.render(silence);
    for (const auto sample : silence) if (sample) return fail("pause did not silence renderer");
    if (!validate_optional_canonical_prg()) return fail("canonical PRG source/register trace diverged");
    write_artifacts(one, "zelda1_overworld_music_synthetic.wav",
                    "zelda1_overworld_music_synthetic_spectrogram.pgm");
    std::cout << "Zelda 1 direct DriveSong renderer, strict state, WAV and spectrogram artifact passed\n";
}

#include "PlayMode.hpp"

#include "DrawLines.hpp"
#include "DrawTris.hpp"
#include "PathFont.hpp"
#include "chart.hpp"
#include "gl_errors.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace {

// Rendered once by the first PlayMode; kept alive until the program exits because the mixer thread reads them
// until Sound::shutdown(), which main() calls only after the mode is gone
std::vector< Sound::Sample > melody;      // one sample per voice, all the same length
std::map< int, Sound::Sample > bell;      // confirmation bell per target pitch, rung only when the press is on time

//----- layout (world units; the window shows the W x HT world with letterboxing, y down like a canvas) -----
constexpr float W = 900.0f, ROLL_H = 64.0f, TOP = 130.0f, H = 460.0f, HT = TOP + H;  // piano roll, text bar, then the bird canvas at TOP
constexpr float BRANCH = 300.0f, KEY_Y[2] = { 34.0f, 426.0f }, CUE_Y = 90.0f, STEP_Y = 386.0f;  // inside the bird canvas
constexpr float WINDOW = 0.225f, PERFECT = 0.075f;  // timing windows in seconds: a press counts within WINDOW, rings the bell within PERFECT
constexpr float PI = 3.14159265f;
constexpr int RATE = 48000, TABLE = 4096;
constexpr float LEVEL = 0.15f;

// sRGB hex colour -> linear (the framebuffer re-encodes to sRGB, see main.cpp)
glm::u8vec4 rgb(uint32_t hex) {
	auto lin = [](uint32_t c) { float s = c / 255.0f; return uint8_t(std::round(255.0f * (s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f)))); };
	return glm::u8vec4(lin(hex >> 16 & 255), lin(hex >> 8 & 255), lin(hex & 255), 255);
}
const glm::u8vec4 FG = rgb(0x000000), BG = rgb(0xffffff), BLUE = rgb(0x2563eb), GRAY = rgb(0xa3a3a3), LIGHT = rgb(0xd4d4d4),
	RED = rgb(0xef4444), WOOD = rgb(0x92400e), LEAF = rgb(0x16a34a), BEAK = rgb(0x9a3412);

//----- synthesis -----
// One period of a waveform with the given harmonic amplitudes, scaled so it peaks at 1 (like the browser's PeriodicWave)
std::vector< float > wavetable(std::vector< float > const &harmonics) {
	std::vector< float > w(TABLE, 0.0f);
	float peak = 0.0f;
	for (int i = 0; i < TABLE; ++i) {
		for (size_t n = 0; n < harmonics.size(); ++n) w[i] += harmonics[n] * std::sin(2.0f * PI * (n + 1) * i / TABLE);
		peak = std::max(peak, std::abs(w[i]));
	}
	for (auto &s : w) s /= peak;
	return w;
}

// Add one note to 'out': 5 ms attack, exponential decay (time constant 0.3 s), 30 ms release at the end of the note value, 0.2 s tail.
// The envelope is a running product (one multiply per sample), the waveform a table lookup: no sin or exp inside the loop.
void render(std::vector< float > &out, std::vector< float > const &wave, double start, int pitch, double dur, float amp) {
	const float k_decay = std::exp(-1.0f / (RATE * 0.3f)), k_release = std::exp(-1.0f / (RATE * 0.03f));
	const int attack = RATE * 5 / 1000, release = int(dur * RATE), len = release + RATE / 5;
	const size_t s0 = size_t(start * RATE);
	const double step = 440.0 * std::pow(2.0, (pitch - 69) / 12.0) * TABLE / RATE;
	double phase = 0.0;
	float env = 1.0f;
	for (int n = 0; n < len && s0 + n < out.size(); ++n) {
		int idx = int(phase);
		float s = wave[idx] + (wave[(idx + 1) & (TABLE - 1)] - wave[idx]) * float(phase - idx);
		phase += step;
		if (phase >= TABLE) phase -= TABLE;
		float e = env;
		if (n < attack) e *= float(n) / attack; else env *= (n < release ? k_decay : k_release);
		out[s0 + n] += amp * e * s;
	}
}

//----- drawing -----
struct Xf {  // bird-local coordinates (feet at the origin, y up is negative) -> world: scale, rotate, then place
	glm::vec2 o; float ang, sx, sy;
	glm::vec2 operator()(float x, float y) const { x *= sx; y *= sy; float c = std::cos(ang), s = std::sin(ang); return o + glm::vec2(c * x - s * y, s * x + c * y); }
};
const Xf IDENTITY = { glm::vec2(0.0f), 0.0f, 1.0f, 1.0f };

void ellipse(DrawTris &t, glm::vec2 c, float rx, float ry, float rot, glm::u8vec4 col, Xf const &xf = IDENTITY) {  // a 32-gon
	auto pt = [&](int i) { float a = 2.0f * PI * i / 32; glm::vec2 p(rx * std::cos(a), ry * std::sin(a)); float cr = std::cos(rot), sr = std::sin(rot); return xf(c.x + cr * p.x - sr * p.y, c.y + sr * p.x + cr * p.y); };
	for (int i = 0; i < 32; ++i) { glm::vec2 a = pt(i), b = pt(i + 1); t.tri(glm::vec3(xf(c.x, c.y), 0.0f), glm::vec3(a, 0.0f), glm::vec3(b, 0.0f), col); }
}
void circle(DrawTris &t, glm::vec2 c, float r, glm::u8vec4 col, Xf const &xf = IDENTITY) { ellipse(t, c, r, r, 0.0f, col, xf); }
void annulus(DrawTris &t, glm::vec2 c, float r, float width, glm::u8vec4 col) {
	for (int i = 0; i < 32; ++i) {
		float a = 2.0f * PI * i / 32, b = 2.0f * PI * (i + 1) / 32;
		glm::vec2 ua(std::cos(a), std::sin(a)), ub(std::cos(b), std::sin(b));
		t.quad(glm::vec3(c + ua * (r - width), 0.0f), glm::vec3(c + ua * r, 0.0f), glm::vec3(c + ub * r, 0.0f), glm::vec3(c + ub * (r - width), 0.0f), col);
	}
}
void line(DrawTris &t, glm::vec2 a, glm::vec2 b, float width, glm::u8vec4 col, bool round = false) {
	glm::vec2 n = glm::normalize(glm::vec2(a.y - b.y, b.x - a.x)) * (width / 2.0f);
	t.quad(glm::vec3(a + n, 0.0f), glm::vec3(b + n, 0.0f), glm::vec3(b - n, 0.0f), glm::vec3(a - n, 0.0f), col);
	if (round) { circle(t, a, width / 2.0f, col); circle(t, b, width / 2.0f, col); }
}
float text_width(std::string const &s) {
	float w = 0.0f;
	for (char c : s) { auto f = PathFont::font.glyph_map.find(std::string(1, c)); w += f == PathFont::font.glyph_map.end() ? 0.6f : PathFont::font.glyph_widths[f->second]; }
	return w;
}
// align: 0 = anchor is the left end of the baseline, 0.5 = centre, 1 = right end
void text(DrawLines &l, std::string const &s, glm::vec2 anchor, float size, glm::u8vec4 col, float align = 0.0f) {
	anchor.x -= align * size * text_width(s);
	l.draw_text(s, glm::vec3(anchor, 0.0f), glm::vec3(size, 0.0f, 0.0f), glm::vec3(0.0f, -size, 0.0f), col);
}

// world -> clip: fit the W x HT world into the window, y flipped
void scales(float aspect, float &sx, float &sy) { if (aspect > W / HT) { sy = 2.0f / HT; sx = sy / aspect; } else { sx = 2.0f / W; sy = sx * aspect; } }
glm::mat4 world_to_clip(float aspect) {
	float sx, sy; scales(aspect, sx, sy);
	return glm::mat4(sx, 0.0f, 0.0f, 0.0f,  0.0f, -sy, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f, 0.0f,  -W / 2.0f * sx, HT / 2.0f * sy, 0.0f, 1.0f);
}
glm::vec2 to_world(float mx, float my, glm::uvec2 const &ws) {
	float sx, sy; scales(float(ws.x) / float(ws.y), sx, sy);
	return glm::vec2((2.0f * mx / ws.x - 1.0f) / sx + W / 2.0f, HT / 2.0f - (1.0f - 2.0f * my / ws.y) / sy);
}

int last_le(std::vector< float > const &arr, float x) { return int(std::upper_bound(arr.begin(), arr.end(), x) - arr.begin()) - 1; }  // last index with arr[i] <= x, or -1

}  // namespace

//----- chart and audio -----

PlayMode::PlayMode() {
	spb = 60.0f / chart::BPM;

	std::vector< bool > is_target[3], is_cue[3];
	for (int v = 0; v < 3; ++v) { is_target[v].assign(chart::VOICES[v]->size(), false); is_cue[v].assign(chart::VOICES[v]->size(), false); }
	auto note = [](int v, int i) -> chart::Note const & { return (*chart::VOICES[v])[i]; };
	auto add_target = [&](int v, int i, int row) { is_target[v][i] = true; targets.push_back({ v, row, note(v, i).t, note(v, i).dur, note(v, i).p }); return int(targets.size()) - 1; };
	for (auto const &m : chart::MOTIFS) {  // three cue notes (do-si-do), then the target
		Group g = { m.v, {}, false, 0 };
		for (int k = 0; k < 3; ++k) { g.t.push_back(note(m.v, m.i + k).t); is_cue[m.v][m.i + k] = true; }
		g.tg = add_target(m.v, m.i + 3, 0);
		motifs.push_back(g);
	}
	for (auto const &s : chart::SCALES) {  // steps i .. j-1, then the target note j
		Group g = { s.v, {}, note(s.v, s.j).p > note(s.v, s.i).p, 0 };
		for (int k = s.i; k < s.j; ++k) { g.t.push_back(note(s.v, k).t); is_cue[s.v][k] = true; }
		g.tg = add_target(s.v, s.j, 1);
		scales.push_back(g);
	}
	for (int v = 0; v < 3; ++v) for (size_t i = 0; i < chart::VOICES[v]->size(); ++i) {
		chart::Note const &n = note(v, int(i));
		end = std::max(end, n.t + n.dur); lo = std::min(lo, n.p); hi = std::max(hi, n.p);
		if (!is_target[v][i]) onsets[v].push_back(n.t);
	}

	if (melody.empty()) {
		// Render the three voices. Cue notes are simply 1.33x louder; the sum of the voices is scaled to stay well below clipping.
		std::vector< float > saw = wavetable([]{ std::vector< float > h; for (int n = 1; n <= 16; ++n) h.push_back(1.0f / n); return h; }());
		std::vector< float > chime = wavetable({ 1.0f, 0.7f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.35f });  // only octave partials, so the low register still reads as the same pitch
		std::vector< float > buf[3];
		for (int v = 0; v < 3; ++v) {
			buf[v].assign(size_t((end * spb + 0.5f) * RATE), 0.0f);
			for (size_t i = 0; i < chart::VOICES[v]->size(); ++i) { chart::Note const &n = note(v, int(i)); render(buf[v], saw, n.t * spb, n.p, n.dur * spb, LEVEL * (is_cue[v][i] ? 1.33f : 1.0f)); }
		}
		float peak = 0.0f;
		for (size_t n = 0; n < buf[0].size(); ++n) peak = std::max(peak, std::abs(buf[0][n] + buf[1][n] + buf[2][n]));
		for (auto &b : buf) for (auto &s : b) s *= std::min(1.0f, 0.6f / peak);
		melody.reserve(3);
		for (auto &b : buf) melody.emplace_back(b);

		// Confirmation bells, one per target pitch, always two beats long
		for (auto const &tg : targets) {
			if (bell.count(tg.p)) continue;
			std::vector< float > b(size_t((2.0f * spb + 0.3f) * RATE), 0.0f);
			render(b, chime, 0.0, tg.p, 2.0 * spb, LEVEL);
			bell.emplace(tg.p, Sound::Sample(b));
		}
	}

	// The mixer's read position runs ahead of the ear by one device buffer plus whatever the OS adds; guess 10 ms for the latter
	SDL_AudioSpec spec; int frames = 1024;
	if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) frames = 1024;
	latency = frames / double(RATE) + 0.01;
	std::cout << "Audio device buffer: " << frames << " frames; assuming " << int(latency * 1000) << " ms output latency. " << targets.size() << " targets." << std::endl;

	birds[0] = { 150.0f, 46.0f, rgb(0xf97316) };
	birds[1] = { 450.0f, 36.0f, rgb(0xeab308) };
	birds[2] = { 750.0f, 28.0f, rgb(0x22c55e) };
}

PlayMode::~PlayMode() {
}

//----- transport -----

// Start the three voices at song position 'sec'. Everything happens under one lock so the mixer never sees the new
// samples at position 0; they fade in over a frame so the jump makes no click.
void PlayMode::start_voices(double sec) {
	uint32_t pos = uint32_t(std::clamp(sec, 0.0, double(melody[0].data.size() - 1) / RATE) * RATE);
	Sound::lock();
	for (int v = 0; v < 3; ++v) {
		voice[v] = Sound::play(melody[v], 0.0f, (v - 1) * 0.5f);
		voice[v]->i = pos;
		voice[v]->set_volume(1.0f, 1.0f / 60.0f);
	}
	Sound::unlock();
	song_start = wall - pos / double(RATE);
}

void PlayMode::set_playing(bool on) {
	if (on == playing) return;
	playing = on;
	if (on) start_voices(beat * spb);
	else { Sound::stop_all_samples(); for (auto &v : voice) v.reset(); }  // fades the voices and any ringing bells over a frame
}

// Jump anywhere: targets ahead become playable again, targets skipped over count as neither hit nor missed
void PlayMode::seek(float b, bool demo_) {
	demo = demo_;
	beat = b;
	for (auto &tg : targets) { if (tg.t > b) { tg.done = false; tg.hit = -1; } else if (!tg.done) tg.done = true; }
	for (auto &bird : birds) { bird.pose = Pose::None; bird.sing_t = -9; }
	if (playing) { Sound::stop_all_samples(); start_voices(b * spb); }
}

// Space bar / demo: pause, resume, or restart from the top when finished
void PlayMode::toggle(bool demo_) {
	if (demo_ || (!playing && beat >= end)) { seek(0.0f, demo_); set_playing(true); }
	else set_playing(!playing);
}

void PlayMode::pose(int v, Pose kind) { birds[v].pose = kind; birds[v].pose_t = wall; }

void PlayMode::judge(Target &tg, float const *err) {
	tg.done = true;
	tg.hit = err ? 1 : 0;
	if (!err) pose(tg.v, Pose::Miss);
	else pose(tg.v, std::abs(*err) < PERFECT ? Pose::Ok : Pose::Meh);
}

void PlayMode::press(int v, int row, double sec) {
	if (!playing) return;
	Bird &b = birds[v];
	b.key_t[row] = wall;  // the key cap itself reacts to every press
	Target *tg = nullptr;
	for (auto &x : targets) if (x.v == v && x.row == row && !x.done && std::abs(sec - x.t * spb) < WINDOW) { tg = &x; break; }
	if (!tg) { pose(v, Pose::Huh); return; }
	float err = float(sec - tg->t * spb);
	if (std::abs(err) < PERFECT) { Sound::play(bell.at(tg->p), 1.0f, (v - 1) * 0.5f); b.sing_t = wall; }  // on time: ring the bell and sing
	judge(*tg, &err);
}

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {
	// song position the ear was hearing when this event happened: its timestamp is on the same clock as 'wall'
	double heard = evt.common.timestamp / 1e9 - song_start - latency;
	if (evt.type == SDL_EVENT_KEY_DOWN) {
		static const SDL_Keycode KEYS[2][3] = { { SDLK_Q, SDLK_W, SDLK_E }, { SDLK_A, SDLK_S, SDLK_D } };
		for (int row = 0; row < 2; ++row) for (int v = 0; v < 3; ++v) if (evt.key.key == KEYS[row][v]) { press(v, row, heard); return true; }
		if (evt.key.key == SDLK_SPACE) { toggle(false); return true; }
		if (evt.key.key == SDLK_RETURN) { toggle(true); return true; }
	} else if (evt.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
		glm::vec2 p = to_world(evt.button.x, evt.button.y, window_size);
		if (p.x >= 0.0f && p.x < W && p.y < ROLL_H) { seek(p.x / W * end, false); set_playing(true); return true; }  // click the piano roll: jump there and play
	}
	return false;
}

void PlayMode::update(float) {
	wall = SDL_GetTicksNS() / 1e9;
	if (!playing) return;
	Sound::lock();
	uint32_t i = voice[0]->i;
	bool stopped = voice[0]->stopped;
	Sound::unlock();
	double mixed = i / double(RATE), heard = mixed - latency;
	song_start = wall - mixed;
	beat = float(heard / spb);
	for (auto &tg : targets) {
		if (tg.done) continue;
		double t = tg.t * spb;
		if (demo && heard >= t - 1.0 / 30.0) press(tg.v, tg.row, t - 1.0 / 30.0);  // demo: press every key a frame early, like a keen player would
		else if (heard > t + WINDOW) judge(tg, nullptr);
	}
	if (stopped) { set_playing(false); beat = end; }
}

//----- drawing -----

// The latest do-si-do group (or scale run) of this voice that has started and whose target is still open; k = notes heard so far
struct Cue { int k, n; bool up; };
static std::optional< Cue > cue(std::vector< PlayMode::Group > const &list, std::vector< PlayMode::Target > const &targets, int v, float beat) {
	PlayMode::Group const *cur = nullptr;
	for (auto const &g : list) if (g.v == v && g.t[0] <= beat) cur = &g;
	if (!cur || targets[cur->tg].done) return std::nullopt;
	return Cue{ int(std::count_if(cur->t.begin(), cur->t.end(), [&](float t) { return t <= beat; })), int(cur->t.size()), cur->up };
}

// Key cap: on a press it pops 35% bigger and flashes inverted for one frame, then settles. It never says when to press.
static void keycap(DrawTris &tris, DrawLines &lines, char key, glm::vec2 c, double press_age) {
	float pop = float(std::exp(-press_age * 12.0)), r = 28.0f * (1.0f + 0.35f * pop), size = 24.0f * (1.0f + 0.35f * pop);
	bool flash = press_age < 0.06;
	if (flash) circle(tris, c, r, FG); else annulus(tris, c, r, 3.0f, FG);
	text(lines, std::string(1, key), c + glm::vec2(0.0f, size * 0.36f), size, flash ? BG : FG, 0.5f);  // capitals are about 0.72 em tall: this centres them
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glm::mat4 m = world_to_clip(float(drawable_size.x) / float(drawable_size.y));
	DrawLines lines(m);  // text; flushed after the shapes below (destroyed last), so it always ends up on top
	DrawTris tris(m);

	{ // piano roll: every note in its bird's colour, one semitone tall (never under 1 unit); targets recoloured; a playhead
		float sh = std::max(1.0f, (ROLL_H - 8.0f) / (hi - lo + 1));
		auto px = [&](float b) { return b / end * W; };
		auto note = [&](float t, float dur, int p, glm::u8vec4 col) { float y = ROLL_H - 4.0f - (p - lo + 1) * sh; tris.rect(glm::vec2(px(t), y), glm::vec2(px(t) + std::max(2.0f, px(dur) - 1.0f), y + sh), col); };
		for (int v = 0; v < 3; ++v) for (auto const &n : *chart::VOICES[v]) note(n.t, n.dur, n.p, birds[v].color);
		for (auto const &tg : targets) note(tg.t, tg.dur, tg.p, tg.hit == 1 ? FG : tg.hit == 0 ? RED : tg.done ? GRAY : BLUE);
		float hx = px(std::clamp(beat, 0.0f, end));
		tris.rect(glm::vec2(hx - 1.0f, 0.0f), glm::vec2(hx + 1.0f, ROLL_H), FG);
		if (!playing) tris.tri(glm::vec3(hx + 4.0f, 2.0f, 0.0f), glm::vec3(hx + 16.0f, 9.0f, 0.0f), glm::vec3(hx + 4.0f, 16.0f, 0.0f), FG);
	}
	{ // text bar
		int hits = int(std::count_if(targets.begin(), targets.end(), [](Target const &t) { return t.hit == 1; }));
		bool done = !playing && beat >= end;
		text(lines, "Bach - Fugue in C minor", glm::vec2(20.0f, 94.0f), 22.0f, FG);
		text(lines, (done ? "Done: " : "") + std::to_string(hits) + " / " + std::to_string(targets.size()), glm::vec2(W - 20.0f, 94.0f), 22.0f, FG, 1.0f);
		text(lines, "Space: play / pause    Enter: demo", glm::vec2(20.0f, 120.0f), 15.0f, GRAY);
	}
	{ // branch with a few leaves
		line(tris, glm::vec2(40.0f, TOP + BRANCH + 5.0f), glm::vec2(W - 40.0f, TOP + BRANCH + 5.0f), 12.0f, WOOD, true);
		for (auto const &[lx, side] : { std::pair(300.0f, 1.0f), std::pair(600.0f, -1.0f), std::pair(850.0f, 1.0f) })
			ellipse(tris, glm::vec2(lx, TOP + BRANCH + 5.0f + side * 12.0f), 16.0f, 8.0f, side * 0.5f, LEAF);
	}
	for (int v = 0; v < 3; ++v) {
		Bird &b = birds[v];
		const float x = b.x, r = b.r;
		const double age = wall - b.pose_t;
		const Pose p = age < 0.8 ? b.pose : Pose::None;
		const int on = last_le(onsets[v], beat);
		const double note_age = on < 0 ? 9.0 : (beat - onsets[v][on]) * spb;
		const double sing_age = std::min(note_age, wall - b.sing_t);  // last melody note or last key press, whichever is more recent
		const auto mo = cue(motifs, targets, v, beat), sc = cue(scales, targets, v, beat);
		// every note: hop up within one frame, then fall back fast. On time: stretched big on the first frame, then decays; slightly off: squashed flat, then springs back
		float ang = 0.0f, dy = -8.0f * float(std::exp(-sing_age * 18.0)), sx = 1.0f, sy = 1.0f;
		if (p == Pose::Ok) { float k = float(std::exp(-age * 10.0)); ang += 0.35f * k; sx += 0.6f * k; sy += 0.6f * k; }
		if (p == Pose::Meh) { float q = float(std::exp(-age * 8.0)); ang += 0.25f * float(std::cos(age * 40.0)) * q; sx += 0.4f * q; sy -= 0.4f * q; }
		Xf xf = { glm::vec2(x, TOP + BRANCH - 8.0f + dy), ang, sx, sy };  // origin at the feet
		line(tris, xf(-0.3f * r, -2.0f), xf(-0.3f * r, 8.0f), 3.0f, BEAK);
		line(tris, xf(0.3f * r, -2.0f), xf(0.3f * r, 8.0f), 3.0f, BEAK);
		circle(tris, glm::vec2(0.0f, -r), r, b.color, xf);
		const float open = sing_age < 0.12 ? r * 0.2f : 0.0f;  // beak opens while singing
		tris.tri(glm::vec3(xf(0.8f * r, -1.25f * r), 0.0f), glm::vec3(xf(0.8f * r, -1.1f * r), 0.0f), glm::vec3(xf(1.5f * r, -1.1f * r - open), 0.0f), BEAK);
		tris.tri(glm::vec3(xf(0.8f * r, -1.1f * r), 0.0f), glm::vec3(xf(0.8f * r, -0.95f * r), 0.0f), glm::vec3(xf(1.5f * r, -1.1f * r + open), 0.0f), BEAK);
		const glm::vec2 e(0.35f * r, -1.35f * r);
		circle(tris, e, 0.28f * r, BG, xf);
		if (p == Pose::Miss) {
			line(tris, xf(e.x - 6.0f, e.y - 6.0f), xf(e.x + 6.0f, e.y + 6.0f), 3.0f, FG);
			line(tris, xf(e.x + 6.0f, e.y - 6.0f), xf(e.x - 6.0f, e.y + 6.0f), 3.0f, FG);
		} else {
			circle(tris, e, 0.12f * r, FG, xf);
			circle(tris, e + glm::vec2(0.04f * r, -0.04f * r), 0.05f * r, BG, xf);  // catchlight
		}
		if (p == Pose::Huh) text(lines, "?", glm::vec2(x, TOP + BRANCH - 2.0f * r - 20.0f), 28.0f, FG, 0.5f);

		// 3 circles above the head (do-si-do), steps under the feet (scale run): blue = not sung yet, gray = sung.
		keycap(tris, lines, "QWE"[v], glm::vec2(x, TOP + KEY_Y[0]), wall - b.key_t[0]);
		for (int i = 0; i < 3; ++i) {
			glm::vec2 c(x + (i - 1) * 28.0f, TOP + CUE_Y);
			if (mo) circle(tris, c, 10.0f, i < mo->k ? GRAY : BLUE); else annulus(tris, c, 9.0f, 2.0f, LIGHT);
		}
		keycap(tris, lines, "ASD"[v], glm::vec2(x, TOP + KEY_Y[1]), wall - b.key_t[1]);
		if (sc) for (int i = 0; i < sc->n; ++i) {
			float h = 12.0f + 8.0f * (sc->up ? i : sc->n - 1 - i), x0 = x - (sc->n * 26.0f - 4.0f) / 2.0f + i * 26.0f;
			tris.rect(glm::vec2(x0, TOP + STEP_Y - h), glm::vec2(x0 + 22.0f, TOP + STEP_Y), i < sc->k ? GRAY : BLUE);
		}
	}
	GL_ERRORS();
}

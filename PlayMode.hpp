#include "Mode.hpp"
#include "Sound.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

// Three birds sing the three voices of Bach's C minor fugue (BWV 847). Whenever a voice sings the do-si-do motif
// (three cue notes) or a scale run, press that bird's key on the note that follows: Q W E for do-si-do, A S D for scales.
// The cues are in the sound; the picture only shows what has already been heard.
//
// Audio: the whole fugue is rendered once, in the constructor, into one sample per voice (see render() in PlayMode.cpp)
// and played with Sound::play, so the mixer's read position PlayingSample::i is the game clock: sample-accurate, no drift.
// Confirmation bells are separate short samples played on each key press. Pause and seek stop the voices and start them
// again from the wanted position (PlayingSample::i is written under Sound::lock(), as Sound.hpp allows).
struct PlayMode : Mode {
	PlayMode();
	virtual ~PlayMode();

	virtual bool handle_event(SDL_Event const &, glm::uvec2 const &window_size) override;
	virtual void update(float elapsed) override;
	virtual void draw(glm::uvec2 const &drawable_size) override;

	//----- chart (immutable after the constructor) -----
	struct Target { int v, row; float t, dur; int p; bool done = false; int hit = -1; };  // hit: 1 hit, 0 missed, -1 open or skipped over by a seek
	struct Group { int v; std::vector< float > t; bool up; int tg; };  // cue notes (do-si-do: 3 notes; scale run: its steps) heard before targets[tg]
	std::vector< Target > targets;
	std::vector< Group > motifs, scales;
	std::vector< float > onsets[3];  // beats of every non-target note per voice: the bird hops on them by itself
	float end = 0.0f, spb = 0.0f;    // end of the last note in beats; seconds per beat
	int lo = 127, hi = 0;            // pitch range, for the piano roll

	//----- audio -----
	// The samples themselves (melody, bell) are statics in PlayMode.cpp: the mixer thread keeps reading them until
	// Sound::shutdown() in main(), which runs after this mode has been destroyed
	std::shared_ptr< Sound::PlayingSample > voice[3];
	double latency = 0.0;  // seconds between the mixer reading a sample and the ear hearing it (one device buffer + a guess)

	//----- transport -----
	bool playing = false, demo = false;
	float beat = 0.0f;        // position heard right now (frozen while paused)
	double song_start = 0.0;  // wall-clock second at which song position 0 was mixed; valid while playing
	double wall = 0.0;        // wall-clock seconds, read once per frame; animation ages are measured against it

	enum class Pose { None, Ok, Meh, Miss, Huh };
	struct Bird { float x, r; glm::u8vec4 color; Pose pose = Pose::None; double pose_t = -9, sing_t = -9, key_t[2] = { -9, -9 }; };
	Bird birds[3];

	void start_voices(double sec);
	void set_playing(bool on);
	void seek(float beat, bool demo);
	void toggle(bool demo);
	void press(int v, int row, double sec);  // sec: song position heard when the key went down
	void judge(Target &tg, float const *err);
	void pose(int v, Pose kind);
};

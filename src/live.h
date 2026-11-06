/* Live on/off for the effect plugins MPC is running: dlsym("VSTPluginMain") is interposed (and handed on to another
 * addin that interposes it, such as mpc-addin-commander), and each effect's processReplacing is wrapped so that, when
 * off, its input goes straight to its output, with a crossfade over one block. MPC's project still says the plugin is
 * on: APPLY writes it into the project. */
#ifndef LIVE_H
#define LIVE_H

enum { LIVE_NONE = -1, LIVE_ON = 0, LIVE_OFF = 1 };

/* Start wrapping the plugins MPC loads from now on. */
void live_enable(void);
/* The state of the one running instance of this plugin library; LIVE_NONE when none is processing audio (or more
 * than one is: it can't tell them apart). */
int live_state(const char *file);
/* Turn it off (1) or on (0) now. 0, or -1 when live_state is LIVE_NONE. */
int live_set(const char *file, int off);

#endif

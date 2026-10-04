/*
 * weaselwayd's audio: what the desktop plays goes to the client's speakers
 * (rdpsnd), and the client's microphone comes back (audin). The Linux end of
 * both is PipeWire, through the two protocol-simple servers that
 * pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf sets up.
 *
 * Ported from mutter's src/backends/rdp/meta-rdp-audio.c.
 */

#ifndef WEASELWAY_AUDIO_H
#define WEASELWAY_AUDIO_H

#include <stdbool.h>

#include <winpr/wtypes.h>

struct audio_out;
struct audio_in;

/* Writes what the RDP channels have queued to the socket. Called from the
 * playback thread after every packet: the queue is otherwise only drained by
 * the main loop, and audio would stutter whenever a frame took long. */
typedef void (*audio_flush_func)(void *data);

/*
 * Opens the rdpsnd channel on @vcm and forwards the desktop's audio to the
 * client. NULL if the channel cannot be opened; the session goes on without
 * sound.
 *
 * PipeWire need not be reachable for this to succeed: a thread connects to it
 * once the client has agreed to a format, and keeps trying.
 */
struct audio_out *audio_out_new(HANDLE vcm, audio_flush_func flush, void *flush_data,
                                bool verbose);
void audio_out_free(struct audio_out *audio_out);

/*
 * Opens the audin channel on @vcm and forwards the client's microphone to
 * PipeWire. NULL on failure. The connection to PipeWire is what makes the
 * microphone exist there, so it is held for as long as this lives.
 */
struct audio_in *audio_in_new(HANDLE vcm);
void audio_in_free(struct audio_in *audio_in);

#endif

# 0010. Audio goes through PipeWire's protocol-simple servers

Status: Accepted

## Context

The RDP client plays audio (rdpsnd) and records the microphone (audin). The
session plays through PipeWire. mutter's RDP backend bridged the two with raw
PCM over TCP. wslg before it used PulseAudio's module-rdp-sink and
module-rdp-source, which had a socket protocol of their own.

## Decision

- PipeWire runs with its PulseAudio server and WirePlumber. PulseAudio itself
  is disabled.
- `pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf` defines a permanent
  null sink, "Remote Desktop Audio", and two
  `libpipewire-module-protocol-simple` servers on 127.0.0.1: port 4711 for
  playback (S16LE, stereo, 44.1 kHz) and 4712 for the microphone (S16LE, mono,
  44.1 kHz).
- weaselwayd is a TCP client of both while a viewer with audio is attached
  (`audio.c`), and retries when PipeWire is not up or restarts.
- weaselwayd links a FreeRDP with a patch for its FFmpeg DSP backend, which
  treated 16-bit PCM as unsigned and made speech unintelligible
  (`weaselway-freerdp`).

## Consequences

- No PipeWire code in weaselwayd, and stock PipeWire modules.
- The format is fixed in two places, the drop-in and `audio.c`, which must
  agree. protocol-simple can neither negotiate nor announce it.
- There is no latency feedback, so the client gets no timestamps to
  synchronise video with.
- Any local process can connect to the ports and record what the session
  plays, or feed the microphone.
- Audio from browsers crackles. The cause is not found.
- PipeWire leaves a "Remote Desktop Microphone" node behind after each
  reconnect.

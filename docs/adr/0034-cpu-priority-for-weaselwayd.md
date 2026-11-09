# 0034. weaselwayd runs at nice -11, on a fully preemptible kernel

Status: Accepted

## Context

weaselwayd's frame and audio threads run in the same VM as everything the
user starts, builds included, and compete with it for the CPU. rtkit, which PipeWire
uses, raises single threads only and sets `SCHED_RESET_ON_FORK`, so threads
that Mesa and FreeRDP start inside weaselwayd would stay at nice 0. WSL's
kernel boots with voluntary preemption, so a thread that wakes up can wait
for a long kernel path of another task to finish.

## Decision

- weaselwayd sets its nice value to -11 before it starts any thread, so all
  its threads inherit it, Mesa's and FreeRDP's included. -11 is the level
  PipeWire gets through rtkit.
- `LimitNICE=-11` on `user@.service` allows this. Without it, weaselwayd
  logs a message and runs at nice 0.
- `prep-session.sh` sets the scheduler's preemption mode to `full` through
  `/sys/kernel/debug/sched/preempt`. Without debugfs it only warns.
- The GPU side is in 0033.

## Consequences

- Every process of the user manager may lower its nice value to -11, not
  only weaselwayd.
- Full preemption applies to the whole VM, and to every distro running in
  it, not only to the session.
- Both the NixOS module and the Ubuntu install set them up.
- Running the Nix daemon at idle CPU and I/O priority was tried and
  reverted (`3d11d75`, `ab035f1`). The reason was not recorded.

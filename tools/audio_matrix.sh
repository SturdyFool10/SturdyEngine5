#!/bin/bash
# Runs the audio playback probe (Audio/tests/AudioDevicePlaybackTest) inside clean containers, one per audio setup a Linux user might have,
# so "works on my machine" is checked against machines that look nothing like the developer's: older glibc, and every combination of
# sound server and client library. The probe is a self-contained executable (needs only glibc >= 2.34), so it is simply bind-mounted in.
#
#   tools/audio_matrix.sh <path-to-AudioDevicePlaybackTest> [scenario ...]     # no scenario = all
#
# Needs podman and network access (images and packages are downloaded). Each scenario installs its audio stack, starts the daemons with a
# virtual (null) output so no sound hardware is needed, and runs the probe with --require-audio. Results: one line per scenario, and the
# full output in $MATRIX_OUT (default /tmp/audio-matrix). Exit status is the number of failed scenarios.
#
# The probe's exit codes: 0 audio flows, 2 no device could be started, 3 only the silent Null backend, 4 started but delivered nothing.
set -u
probe=${1:?usage: audio_matrix.sh <AudioDevicePlaybackTest> [scenario ...]}
shift
out=${MATRIX_OUT:-/tmp/audio-matrix}
mkdir -p "$out"
probe=$(readlink -f "$probe")

# run <name> <image> <expected-exit> <root setup script> <script to run as the unprivileged user>
run() {
    local name=$1 image=$2 expect=$3 setup=$4 as_user=$5
    local log="$out/$name.log"
    podman run --rm --network=host -v "$probe":/probe:ro,Z "$image" bash -c "
        export DEBIAN_FRONTEND=noninteractive
        if command -v apt-get >/dev/null; then pkg='apt-get install -y -qq --no-install-recommends'; apt-get update -qq >/dev/null; fi
        if command -v dnf >/dev/null; then pkg='dnf install -y -q --setopt=install_weak_deps=False'; fi
        id tester >/dev/null 2>&1 || useradd -m tester
        $setup
        mkdir -p /run/user/1000 && chown tester /run/user/1000 && chmod 700 /run/user/1000
        echo \"[glibc] \$(ldd --version 2>&1 | head -1)\"
        runuser -u tester -- env XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/tester bash -c '$as_user'
        echo \"[exit] \$?\"
    " >"$log" 2>&1
    local code summary
    code=$(sed -n 's/^\[exit\] //p' "$log" | tail -1)
    summary=$(grep -E '^audio: ' "$log" | grep -E '^audio: (backend|no device|only|.*delivered)' | tail -1)
    if [ "${code:-?}" = "$expect" ]; then status=PASS; else status=FAIL; failed=$((failed + 1)); fi
    printf '%-4s %-26s exit %-2s (want %s)  %s\n' "$status" "$name" "${code:-?}" "$expect" "$summary"
}

failed=0
# A null PipeWire sink for daemons that have no hardware.
pw_conf='context.objects = [ { factory = adapter args = { factory.name = support.null-audio-sink node.name = test-sink media.class = Audio/Sink object.linger = true audio.position = [ FL FR ] } } ]'
pw_setup="mkdir -p /home/tester/.config/pipewire/pipewire.conf.d && printf '%s\n' '$pw_conf' > /home/tester/.config/pipewire/pipewire.conf.d/10-null.conf && chown -R tester /home/tester"
start_pw='dbus-run-session -- bash -c "pipewire >/tmp/pw.log 2>&1 & sleep 1; wireplumber >/tmp/wp.log 2>&1 & sleep 3; %s"'

want() { [ ${#selected[@]} -eq 0 ] && return 0; for s in "${selected[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
selected=("$@")

# Nothing at all: must not crash; reports the silent fallback.
want none-debian12      && run none-debian12      debian:12-slim 3 "true" "/probe --require-audio"
want none-ubuntu2204    && run none-ubuntu2204    ubuntu:22.04   3 "true" "/probe --require-audio"
want none-ubi9          && run none-ubi9          registry.access.redhat.com/ubi9/ubi 3 "\$pkg util-linux shadow-utils >/dev/null 2>&1" "/probe --require-audio"
# One sound system each. Two known artefacts of these hardware-less setups, not of the sink: ALSA's null pcm is unpaced (the probe says so, and the
# engine clock runs far faster than real time), and a PulseAudio daemon with only a null sink delivers its first block ~2 s late (pacat, an
# unrelated client, shows the same 2 s; PipeWire's Pulse server and PipeWire itself start in under 50 ms).
want alsa-only          && run alsa-only          debian:12-slim 0 "\$pkg libasound2 >/dev/null 2>&1; printf 'pcm.!default { type null }\nctl.!default { type hw card 0 }\n' > /home/tester/.asoundrc" "/probe --require-audio"
want pulseaudio-only    && run pulseaudio-only    debian:12-slim 0 "\$pkg pulseaudio >/dev/null 2>&1" "pulseaudio --start --exit-idle-time=-1 >/dev/null 2>&1; sleep 2; /probe --require-audio"
want jack-only          && run jack-only          debian:12-slim 0 "\$pkg jackd2 libjack-jackd2-0 >/dev/null 2>&1" "jackd -r -d dummy -r 48000 -p 512 >/tmp/jackd.log 2>&1 & sleep 3; /probe --require-audio"
# PipeWire with its Pulse compatibility server (a default desktop), and PipeWire alone (nothing else installed to speak to it).
want pipewire-with-pulse && run pipewire-with-pulse debian:12-slim 0 "\$pkg pipewire pipewire-pulse libspa-0.2-modules wireplumber dbus-daemon dbus-user-session libpulse0 >/dev/null 2>&1; $pw_setup" \
    "$(printf "$start_pw" 'pipewire-pulse >/tmp/pwp.log 2>&1 & sleep 2; /probe --require-audio')"
want pipewire-only      && run pipewire-only      debian:12-slim 0 "\$pkg pipewire libspa-0.2-modules wireplumber dbus-daemon dbus-user-session >/dev/null 2>&1; $pw_setup" \
    "$(printf "$start_pw" '/probe --require-audio')"
# PipeWire as the only thing that can work: libpulse and libasound deliberately absent.
want pipewire-no-compat-libs && run pipewire-no-compat-libs debian:12-slim 0 "\$pkg pipewire libspa-0.2-modules wireplumber dbus-daemon dbus-user-session >/dev/null 2>&1; find / -xdev \\( -name 'libpulse.so*' -o -name 'libasound.so*' \\) -delete 2>/dev/null; ldconfig; $pw_setup" \
    "$(printf "$start_pw" '/probe --require-audio')"
# Other distributions' glibc and package sets.
want fedora-pipewire    && run fedora-pipewire    fedora:latest  0 "\$pkg pipewire pipewire-pulseaudio wireplumber dbus-daemon util-linux >/dev/null 2>&1; $pw_setup" \
    "$(printf "$start_pw" 'pipewire-pulse >/tmp/pwp.log 2>&1 & sleep 2; /probe --require-audio')"
want ubuntu2204-pulse   && run ubuntu2204-pulse   ubuntu:22.04   0 "\$pkg pulseaudio >/dev/null 2>&1" "pulseaudio --start --exit-idle-time=-1 >/dev/null 2>&1; sleep 2; /probe --require-audio"
# A named-device request on a system that does not have that device must fall through, not stop audio.
want forced-backend-missing && run forced-backend-missing debian:12-slim 3 "true" "STURDY_AUDIO_BACKEND=pipewire,pulse,null /probe --require-audio"

echo "$failed scenario(s) failed; logs in $out"
exit "$failed"

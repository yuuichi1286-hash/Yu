#!/usr/bin/env python3
"""Audio-level tests for libmicns.so, driven through test/harness.

  audio_test.py fifo   HARNESS LIB [RUNNER...]   bit-exact latency/FIFO checks
  audio_test.py denoise HARNESS LIB CLEAN.wav NOISE.wav... [--rates ...]
  audio_test.py deep   HARNESS LIB SPEECH.wav...   deep voice pitch checks

FIFO checks write /vendor/etc/micns.conf (strength=0, hpf=0) so the effect
reduces to "output = input delayed"; any FIFO underrun or slicing bug then
shows up as a sample mismatch. Denoise checks mix clean speech with noise
and report noise reduction plus speech SI-SDR before/after. Deep voice
checks compare the pitch of the voice variant with deep=1 against deep=0.
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

CONF = "/vendor/etc/micns.conf"


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64)


def resample(x, fs_in, fs_out):
    if fs_in == fs_out:
        return x.copy()
    n_out = int(round(len(x) * fs_out / fs_in))
    spec = np.fft.rfft(x)
    out = np.zeros(n_out // 2 + 1, dtype=complex)
    k = min(len(spec), len(out))
    out[:k] = spec[:k]
    return np.fft.irfft(out, n_out) * (n_out / len(x))


def write_conf(text):
    os.makedirs(os.path.dirname(CONF), exist_ok=True)
    with open(CONF, "w") as f:
        f.write(text)


def run(runner, harness, lib, variant, rate, ch, pattern, mode, x):
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in.raw"), os.path.join(d, "out.raw")
        np.clip(np.round(x), -32768, 32767).astype("<i2").tofile(src)
        cmd = runner + [harness, lib, "run", variant, str(rate), str(ch), pattern, mode, src, dst]
        env = dict(os.environ, MICNS_QUIET="1")
        subprocess.run(cmd, check=True, env=env)
        return np.fromfile(dst, "<i2").astype(np.float64)


def fifo_tests(harness, lib, runner):
    write_conf("strength=0\nhpf=0\n")
    fails = 0
    rng = np.random.default_rng(7)
    try:
        # 48 kHz: output must be the input delayed by exactly one RNNoise
        # block (480) plus the primed FIFO (480 + 2 + 48), sample for sample.
        x = rng.integers(-20000, 20000, 48000 * 3).astype(np.float64)
        delay = 480 + 530
        for pattern in ["fixed:480", "fixed:1", "fixed:7", "fixed:4096", "rand:3:5000", "rand:9:300"]:
            for mode in ["sep", "inplace"]:
                y = run(runner, harness, lib, "pure", 48000, 1, pattern, mode, x)
                ok = np.array_equal(y[delay:], x[: len(x) - delay]) and not y[:delay].any()
                fails += not ok
                print(f"  48k mono  {pattern:12s} {mode:7s} exact={'yes' if ok else 'NO'}")
        xs = rng.integers(-20000, 20000, (48000 * 2, 2)).astype(np.float64)
        y = run(runner, harness, lib, "pure", 48000, 2, "rand:5:3000", "inplace", xs.reshape(-1)).reshape(-1, 2)
        ok = np.array_equal(y[delay:], xs[: len(xs) - delay])
        fails += not ok
        print(f"  48k stereo rand inplace exact={'yes' if ok else 'NO'}")

        # Other rates go through the resamplers: compare band-limited noise
        # (content below 0.4*fs and below 18 kHz, since processing runs at
        # 48 kHz) after aligning by FFT cross-correlation.
        for rate in [8000, 11025, 16000, 22050, 32000, 44100, 96000]:
            n = rate * 3
            spec = np.fft.rfft(rng.standard_normal(n))
            f = np.fft.rfftfreq(n, 1 / rate)
            spec[(f > min(0.4 * rate, 18000)) | (f < 100)] = 0
            sig = np.fft.irfft(spec, n)
            sig *= 6000 / np.std(sig)
            outs = []
            for pattern in ["fixed:%d" % (rate // 100), "rand:11:4000", "fixed:1"]:
                outs.append(run(runner, harness, lib, "pure", rate, 1, pattern, "sep", sig))
            same = all(np.array_equal(outs[0], o) for o in outs[1:])
            y = outs[0]
            # Non-integer rate ratios leave a fractional-sample latency, so
            # estimate it on a 16x interpolated cross-correlation and shift
            # the reference by exactly that much before comparing.
            up = 16
            cross = np.fft.rfft(y, 2 * n) * np.conj(np.fft.rfft(sig, 2 * n))
            xc = np.fft.irfft(cross, 2 * n * up)
            tau = int(np.argmax(xc[: n * up // 2])) / up
            lag = int(np.floor(tau))
            shifted = np.fft.irfft(np.fft.rfft(sig, 2 * n) *
                                   np.exp(-2j * np.pi * np.fft.rfftfreq(2 * n) * (tau - lag)), 2 * n)[:n]
            seg = slice(rate // 2, n - lag - rate // 10)
            ref = shifted[seg]
            got = y[seg.start + lag: seg.stop + lag]
            err = np.sqrt(np.mean((got - ref) ** 2)) / np.sqrt(np.mean(ref ** 2))
            ok = same and err < 0.03
            fails += not ok
            print(f"  {rate:6d} Hz latency={tau:8.2f} ({1000 * tau / rate:5.1f} ms) rel.err={err:.4f} "
                  f"slicing-invariant={same} -> {'ok' if ok else 'FAIL'}")
    finally:
        os.remove(CONF)
    return fails


def si_sdr(est, ref):
    ref = ref - ref.mean()
    est = est - est.mean()
    a = np.dot(est, ref) / np.dot(ref, ref)
    e = est - a * ref
    return 10 * np.log10(np.dot(a * ref, a * ref) / np.dot(e, e))


def align(y, ref, max_lag):
    best, lag_best = -1e30, 0
    for lag in range(0, max_lag):
        c = np.dot(y[lag: lag + len(ref) // 2], ref[: len(ref) // 2])
        if c > best:
            best, lag_best = c, lag
    return lag_best


def denoise_tests(harness, lib, runner, clean_path, noise_paths, rates, snr_db=5.0, variant="pure"):
    fs0, clean = read_wav(clean_path)
    # Voice-note shape: silence before, between and after the talking. The
    # steady part of those gaps (from 0.6 s after speech ends) shows how
    # quiet "nobody is talking" gets; speech frames show what happens to the
    # voice itself.
    gap_s = 1.5
    gap = np.zeros(int(gap_s * fs0))
    half = len(clean) // 2
    pieces = [gap, clean[:half], gap, clean[half:], gap]
    starts, pos = [], 0
    for p in pieces:
        starts.append(pos)
        pos += len(p)
    gaps = [(starts[0] / fs0 + 0.3, starts[1] / fs0), (starts[2] / fs0 + 0.6, starts[3] / fs0),
            (starts[4] / fs0 + 0.6, pos / fs0 - 0.1)]
    clean = np.concatenate(pieces)
    print(f"  clean: {os.path.basename(clean_path)} {len(clean) / fs0:.1f}s with {gap_s} s gaps, SNR {snr_db} dB")
    results = []
    for npath in noise_paths:
        fsn, noise = read_wav(npath)
        assert fsn == fs0
        reps = int(np.ceil(len(clean) / len(noise)))
        noise = np.tile(noise, reps)[: len(clean)]
        noise *= np.sqrt(np.mean(clean[clean != 0] ** 2) / np.mean(noise ** 2)) / (10 ** (snr_db / 20))
        for rate in rates:
            s = resample(clean, fs0, rate)
            n = resample(noise, fs0, rate)
            x = s + n
            peak = np.max(np.abs(x))
            if peak > 28000:
                s, n, x = s * 28000 / peak, n * 28000 / peak, x * 28000 / peak
            y = run(runner, harness, lib, variant, rate, 1, "fixed:%d" % (rate // 100), "sep", x)
            lag = align(y[int(gap_s * rate):], s[int(gap_s * rate):], int(0.06 * rate))
            y = y[lag:]
            s_, x_ = s[: len(y)], x[: len(y)]
            sel = np.zeros(len(y), bool)
            for a, b in gaps:
                sel[int(a * rate): int(b * rate)] = True
            gap_db = 10 * np.log10((np.mean(x_[sel] ** 2) + 1e-9) / (np.mean(y[sel] ** 2) + 1e-9))
            fl = rate // 50
            m = len(y) // fl
            se = np.array([np.mean(s_[i * fl:(i + 1) * fl] ** 2) for i in range(m)])
            loud = se > np.max(se) * 10 ** (-20 / 10)
            ys = np.concatenate([y[i * fl:(i + 1) * fl] for i in range(m) if loud[i]])
            ss = np.concatenate([s_[i * fl:(i + 1) * fl] for i in range(m) if loud[i]])
            speech_db = 10 * np.log10(np.mean(ys ** 2) / np.mean(ss ** 2))
            sdr_in, sdr_out = si_sdr(x_, s_), si_sdr(y, s_)
            name = os.path.basename(npath).replace("ms_noise_", "").replace(".wav", "")
            print(f"  {name:22s} {rate:6d} Hz  gaps -{gap_db:5.1f} dB  voice {speech_db:+5.1f} dB  "
                  f"SI-SDR {sdr_in:5.1f} -> {sdr_out:5.1f} dB")
            results.append((name, rate, gap_db, speech_db, sdr_in, sdr_out))
    return results


def f0_track(y, fs=48000, fl=1920, hop=480):
    """Pitch per 10 ms (0 = unvoiced) by normalised autocorrelation, 80-500 Hz."""
    out = []
    for s in range(0, len(y) - fl - 600, hop):
        a = y[s:s + fl] - np.mean(y[s:s + fl])
        e0 = np.dot(a, a)
        if e0 < 1e3 * fl:
            out.append(0.0)
            continue
        c = [np.dot(a, y[s + l:s + l + fl]) / np.sqrt(e0 * np.dot(y[s + l:s + l + fl], y[s + l:s + l + fl]) + 1e-9)
             for l in range(96, 601)]
        k = int(np.argmax(c))
        out.append(fs / (96 + k) if c[k] > 0.75 else 0.0)
    return np.array(out)


def deep_tests(harness, lib, runner, clips, semitones=-2.0, latency=1664):
    """deep=1 must lower the pitch by `semitones` and keep the level."""
    want = 2 ** (semitones / 12)
    fails = 0
    for path in clips:
        fs, s = read_wav(path)
        x = resample(s, fs, 48000)
        x = np.concatenate([np.zeros(24000), x * 12000 / np.max(np.abs(x)), np.zeros(24000)])
        ys = []
        for deep in (0, 1):
            write_conf("gate=0\nleveler=0\ndeep=%d\ndeep_pitch=%g\n" % (deep, semitones))
            ys.append(run(runner, harness, lib, "voice", 48000, 1, "fixed:480", "sep", x))
        os.remove(CONF)
        y0, y1 = ys[0], ys[1][latency:]
        n = min(len(y0), len(y1))
        y0, y1 = y0[:n], y1[:n]
        f0a, f0b = f0_track(y0), f0_track(y1)
        both = (f0a > 0) & (f0b > 0)
        ratio = float(np.median(f0b[both] / f0a[both])) if both.any() else 0.0
        level = 10 * np.log10(np.mean(y1 ** 2) / np.mean(y0 ** 2))
        ok = abs(ratio - want) < 0.015 and both.sum() >= 0.6 * np.sum(f0a > 0) and abs(level) < 3
        fails += not ok
        print(f"  {os.path.basename(path):22s} f0 {np.median(f0a[f0a > 0]):5.1f} Hz  ratio {ratio:.3f} "
              f"(want {want:.3f})  voiced {both.sum()}/{np.sum(f0a > 0)}  level {level:+.1f} dB  {'ok' if ok else 'FAIL'}")
    return fails


def main():
    mode, harness, lib = sys.argv[1], sys.argv[2], sys.argv[3]
    rest = sys.argv[4:]
    runner = []
    if "--runner" in rest:
        i = rest.index("--runner")
        runner = rest[i + 1].split()
        rest = rest[:i] + rest[i + 2:]
    if mode == "fifo":
        fails = fifo_tests(harness, lib, runner)
        print(f"fifo: {fails} failure(s)")
        sys.exit(1 if fails else 0)
    if mode == "deep":
        fails = deep_tests(harness, lib, runner, rest)
        print(f"deep: {fails} failure(s)")
        sys.exit(1 if fails else 0)
    rates = [16000, 48000]
    if "--rates" in rest:
        i = rest.index("--rates")
        rates = [int(r) for r in rest[i + 1].split(",")]
        rest = rest[:i] + rest[i + 2:]
    variant = "pure"
    if "--variant" in rest:
        i = rest.index("--variant")
        variant = rest[i + 1]
        rest = rest[:i] + rest[i + 2:]
    res = denoise_tests(harness, lib, runner, rest[0], rest[1:], rates, variant=variant)
    # Steady noise (fans, AC, vacuum, machines) must all but vanish from the
    # gaps. Speech-like or clicky noise (other talkers, keyboards) is
    # RNNoise's known weak spot and is only reported. The voice itself must
    # survive: pure within 3 dB, voice may be lifted by the leveler.
    steady = ("ac", "VacuumCleaner_1", "CopyMachine_1")
    floor = 20.0 if variant == "voice" else 12.0
    bad = [r for r in res if (r[0] in steady and r[2] < floor) or r[3] < -3.0
           or (variant == "pure" and r[3] > 3.0) or r[3] > 12.5]
    print(f"denoise: {len(bad)} case(s) below expectations")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

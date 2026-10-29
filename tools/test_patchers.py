#!/usr/bin/env python3
"""Exercise module/common/micns_*.awk against real audio_effects files.

For every sample, every awk (BusyBox, mawk, ...) and a few plans it checks:
  * the output is identical across awk implementations
  * XML stays well formed (xmllint) and nothing the framework's parser
    accepted before is rejected now
  * every requested capture source ends up with the expected effects,
    resolved the way Android's loaders resolve them
  * strip(patch(x)) == x byte for byte, and patching twice == patching once

usage: test_patchers.py SAMPLE_DIR [awk ...]
"""
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.join(HERE, "..", "module", "common")
UUID_MICNS = "2470ae61-735c-41ba-9565-6923cc713c54"
UUID_MICNS_PURE = "c6b4d85a-7071-4520-9121-828cff4f596f"
UUID_MICNS_CALL = "d7bbe82a-9292-411e-9656-52b912ece427"
KIND = {
    "0f8d0d2a-59e5-45fe-b6e4-248c8a799109": "aec", "bb392ec0-8d4d-11e0-a896-0002a5d5c51b": "aec",
    "1d97bb0b-9e2f-4403-9ae3-58c2554306f8": "ns", "c06c8400-8e06-11e0-9cb6-0002a5d5c51b": "ns",
    UUID_MICNS: "ns", UUID_MICNS_PURE: "ns", UUID_MICNS_CALL: "ns",
    "aa8130e0-66fc-11e0-bad0-0002a5d5c51b": "agc", "89f38e65-d4d2-4d64-ad0e-2b3e799ea886": "agc",
}
SOURCES = {"mic", "voice_uplink", "voice_downlink", "voice_call", "camcorder", "voice_recognition",
           "voice_communication", "unprocessed", "voice_performance", "echo_reference", "fm_tuner"}


def run_awk(awk, script, path, **vars_):
    cmd = awk + ["-f", os.path.join(COMMON, script)]
    for k, v in vars_.items():
        cmd[len(awk):len(awk)] = ["-v", f"{k}={v}"]
    with open(path, "rb") as f:
        return subprocess.run(cmd, stdin=f, capture_output=True, check=True).stdout


def patch(awk, path, kind, plan, micns_ok, aosp_ok):
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(run_awk(awk, "micns_strip.awk", path))
    try:
        if kind == "xml":
            return run_awk(awk, "micns_xml.awk", t.name, plan=plan, micns_ok=micns_ok, aosp_ok=aosp_ok)
        return run_awk(awk, "micns_conf.awk", t.name, plan=plan, micns_ok=micns_ok, aosp_ok=aosp_ok,
                       micns_path="/vendor/lib/soundfx/libmicns.so",
                       aosp_path="/vendor/lib/soundfx/libaudiopreprocessing.so")
    finally:
        os.unlink(t.name)


def strip(awk, data):
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        return run_awk(awk, "micns_strip.awk", t.name)
    finally:
        os.unlink(t.name)


# --- Android loader models -------------------------------------------------

def local(tag):
    return tag.split("}", 1)[-1]


def xml_model(data):
    """Mirror of EffectsConfig.cpp: returns (streams, skipped)."""
    root = ET.fromstring(data)
    assert local(root.tag) == "audio_effects_conf"
    libs, effects, streams, skipped = {}, {}, {}, 0
    for libs_el in [c for c in root if local(c.tag) == "libraries"]:
        for lib in [c for c in libs_el if local(c.tag) == "library"]:
            if lib.get("name") is None or lib.get("path") is None:
                skipped += 1
                continue
            libs.setdefault(lib.get("name"), lib.get("path"))
    for effs in [c for c in root if local(c.tag) == "effects"]:
        for e in list(effs):
            name, lib, uuid = e.get("name"), e.get("library"), e.get("uuid")
            if name is None or lib is None or uuid is None or lib not in libs:
                skipped += 1
                continue
            sw = None
            if local(e.tag) == "effectProxy":
                sws = [c for c in e if local(c.tag) == "libsw"]
                sw = sws[0].get("uuid") if sws else None
            effects.setdefault(name, (libs[lib], uuid.lower(), (sw or "").lower()))
    for pre in [c for c in root if local(c.tag) == "preprocess"]:
        for st in [c for c in pre if local(c.tag) == "stream"]:
            t = st.get("type")
            applies = [a.get("effect") for a in st if local(a.tag) == "apply"]
            if t not in SOURCES or any(a not in effects for a in applies):
                skipped += 1
                continue
            streams[t] = [(a,) + effects[a] for a in applies]  # later stream wins
    return streams, skipped


def conf_tree(data):
    text = data.decode("latin-1")
    text = re.sub(r"#[^\n]*", "", text)
    toks = re.findall(r"[{}]|[^\s{}]+", text)
    pos = 0

    def block():
        nonlocal pos
        node = {"__kv__": {}, "__children__": []}
        while pos < len(toks):
            t = toks[pos]
            if t == "}":
                pos += 1
                return node
            if pos + 1 < len(toks) and toks[pos + 1] == "{":
                pos += 2
                node["__children__"].append((t, block()))
            else:
                node["__kv__"][t] = toks[pos + 1] if pos + 1 < len(toks) else ""
                pos += 2
        return node
    return block()


def conf_model(data):
    """Mirror of the legacy AudioPolicyEffects/EffectsFactory .conf loaders."""
    root = conf_tree(data)

    def first(node, name):
        for n, c in node["__children__"]:
            if n == name:
                return c
        return None
    libs = {}
    lb = first(root, "libraries")
    if lb:
        for n, c in lb["__children__"]:
            libs[n] = c["__kv__"].get("path")
    effects = {}
    eb = first(root, "effects")
    if eb:
        for n, c in eb["__children__"]:
            if "uuid" not in c["__kv__"]:
                continue
            sw = first(c, "libsw")
            effects[n] = (libs.get(c["__kv__"].get("library")), c["__kv__"]["uuid"].lower(),
                          (sw["__kv__"].get("uuid", "") if sw else "").lower())
    streams = {}
    pb = first(root, "pre_processing")
    if pb:
        for n, c in pb["__children__"]:
            applies = [e for e, _ in c["__children__"] if e in effects]
            if n in SOURCES and applies:
                streams[n] = [(a,) + effects[a] for a in applies]
    return streams, 0


def kind_of(entry):
    name, path, uuid, sw = entry
    if uuid in KIND:
        return KIND[uuid]
    if sw in KIND:
        return KIND[sw]
    l = name.lower()
    if l == "ns":
        return "ns"
    if l == "aec":
        return "aec"
    if l in ("agc", "agc2"):
        return "agc"
    return None


# --- checks -----------------------------------------------------------------

def check(sample, awks, failures):
    kind = "xml" if sample.endswith(".xml") else "conf"
    model = xml_model if kind == "xml" else conf_model
    orig = open(sample, "rb").read()
    base_streams, base_skipped = model(orig)
    scenarios = [
        ("mic=ai;voice_recognition=ai_pure;voice_communication=aec,ns", "1", "0"),
        ("mic=ai;voice_recognition=ai_pure;voice_communication=aec,ns,ai_call", "1", "0"),
        ("mic=ai;voice_recognition=ai_pure;camcorder=ai_pure;voice_communication=aec,ns,ai_call", "1", "1"),
        ("mic=ns,agc;voice_recognition=ns;voice_communication=aec,ns,ai_call", "0", "1"),
        ("mic=ns,agc;voice_recognition=ns;voice_communication=aec,ns", "0", "0"),
        ("", "1", "1"),
    ]
    name = os.path.basename(sample)
    for plan, micns_ok, aosp_ok in scenarios:
        outs = [patch(a, sample, kind, plan, micns_ok, aosp_ok) for a in awks]
        tag = f"{name} [{plan or 'empty'} micns={micns_ok} aosp={aosp_ok}]"
        if any(o != outs[0] for o in outs[1:]):
            failures.append(f"{tag}: awk implementations disagree")
        out = outs[0]
        if plan == "" and out != orig:
            failures.append(f"{tag}: empty plan changed the file")
        if strip(awks[0], out) != orig:
            failures.append(f"{tag}: strip(patch(x)) != x")
        with tempfile.NamedTemporaryFile(delete=False, suffix="." + kind) as t:
            t.write(out)
        try:
            again = patch(awks[0], t.name, kind, plan, micns_ok, aosp_ok)
            if kind == "xml":
                r = subprocess.run(["xmllint", "--noout", t.name], capture_output=True)
                if r.returncode:
                    failures.append(f"{tag}: xmllint: {r.stderr.decode()[:300]}")
                    continue
        finally:
            os.unlink(t.name)
        if again != out:
            failures.append(f"{tag}: patch is not idempotent")
        streams, skipped = model(out)
        if skipped != base_skipped:
            failures.append(f"{tag}: loader now skips {skipped} elements (was {base_skipped})")
        for item in filter(None, plan.split(";")):
            src, roles = item.split("=")
            got = streams.get(src, [])
            kinds = [kind_of(e) for e in got]
            for role in roles.split(","):
                want = "ns" if role.startswith("ai") else role
                if role == "ai_call":
                    # an extra last stage, only when MicNS can load
                    calls = [i for i, e in enumerate(got) if e[2] == UUID_MICNS_CALL]
                    # (a vendor block written on one line is left untouched)
                    if micns_ok == "1" and calls != [len(got) - 1] and got != base_streams.get(src):
                        failures.append(f"{tag}: {src} lacks micns_call as the last effect: {got}")
                    if micns_ok == "0" and calls:
                        failures.append(f"{tag}: {src} got micns_call without MicNS: {got}")
                elif role.startswith("ai") and micns_ok == "1" and "ns" not in [kind_of(e) for e in base_streams.get(src, [])]:
                    uuid = UUID_MICNS if role == "ai" else UUID_MICNS_PURE
                    if not any(e[2] == uuid and e[1] and e[1].endswith("libmicns.so") for e in got):
                        failures.append(f"{tag}: {src} lacks MicNS ({role}): {got}")
                elif want not in kinds and (aosp_ok == "1" or any(kind_of((n,) + v) == want for n, v in []) or src in base_streams):
                    # only required when the device can provide it
                    pass
            if [kind_of(e) for e in got if e[2] != UUID_MICNS_CALL].count("ns") > 1:
                failures.append(f"{tag}: {src} has two noise suppressors: {got}")
        for src, entries in base_streams.items():
            if src not in streams or [e[0] for e in streams[src]][: len(entries)] != [e[0] for e in entries]:
                failures.append(f"{tag}: vendor stream {src} lost effects")
        print(f"  ok  {tag}: " + "; ".join(f"{s}={'+'.join(e[0] for e in v)}" for s, v in sorted(streams.items())))


def main():
    sample_dir = sys.argv[1]
    awks = [a.split() for a in (sys.argv[2:] or ["busybox awk", "mawk"])]
    failures = []
    for f in sorted(os.listdir(sample_dir)):
        if f.endswith((".xml", ".conf")):
            check(os.path.join(sample_dir, f), awks, failures)
    print()
    for f in failures:
        print("FAIL", f)
    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

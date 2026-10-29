# MicNS patcher for the legacy audio_effects.conf format (Android 7 and
# devices that still ship it). Same plan as micns_xml.awk; insertions are
# whole lines between "# micns:L" and "# micns:/L" so micns_strip.awk can
# take them out again.
#
# Variables (-v): plan, micns_ok, aosp_ok, report (see micns_xml.awk) and
#   micns_path  library path to write for libmicns.so (32-bit style, the
#               loader maps /lib/ to /lib64/ by itself)
#   aosp_path   path of libaudiopreprocessing.so to use if needed
#
# Runs on BusyBox awk: POSIX features only.

BEGIN {
    RS = "\001"
    UUID_MICNS = "2470ae61-735c-41ba-9565-6923cc713c54"
    UUID_MICNS_PURE = "c6b4d85a-7071-4520-9121-828cff4f596f"
    UUID_MICNS_CALL = "d7bbe82a-9292-411e-9656-52b912ece427"
    kind["0f8d0d2a-59e5-45fe-b6e4-248c8a799109"] = "aec"
    kind["bb392ec0-8d4d-11e0-a896-0002a5d5c51b"] = "aec"
    kind["7b491460-8d4d-11e0-bd61-0002a5d5c51b"] = "aec"
    kind["1d97bb0b-9e2f-4403-9ae3-58c2554306f8"] = "ns"
    kind["c06c8400-8e06-11e0-9cb6-0002a5d5c51b"] = "ns"
    kind["58b4b260-8e06-11e0-aa8e-0002a5d5c51b"] = "ns"
    kind[UUID_MICNS] = "ns"
    kind[UUID_MICNS_PURE] = "ns"
    kind[UUID_MICNS_CALL] = "ns"
    kind["aa8130e0-66fc-11e0-bad0-0002a5d5c51b"] = "agc"
    kind["89f38e65-d4d2-4d64-ad0e-2b3e799ea886"] = "agc"
    kind["0a8abfe0-654c-11e0-ba26-0002a5d5c51b"] = "agc"
    aosp_uuid["aec"] = "bb392ec0-8d4d-11e0-a896-0002a5d5c51b"
    aosp_uuid["ns"] = "c06c8400-8e06-11e0-9cb6-0002a5d5c51b"
    aosp_uuid["agc"] = "aa8130e0-66fc-11e0-bad0-0002a5d5c51b"
}

# Read the file whole and split it into lines ourselves so the output can
# be joined back exactly (a missing final newline stays missing).
{ doc = (NR > 1) ? doc RS $0 : $0 }

function name_kind(nm,    l) {
    l = tolower(nm)
    if (l == "ns" || l ~ /noise_?supp/) return "ns"
    if (l == "aec" || l ~ /echo_?cancel/) return "aec"
    if (l == "agc" || l == "agc2" || l ~ /gain_?control/) return "agc"
    return ""
}

function effect_kind(nm,    u) {
    u = tolower(eff_uuid[nm])
    if (u in kind) return kind[u]
    u = tolower(eff_sw[nm])
    if (u in kind) return kind[u]
    return name_kind(nm)
}

function unique(base, taken,    nm, k) {
    nm = base
    k = 2
    while (index(taken, " " nm " ")) nm = base "_" k++
    return nm
}

function say(msg) {
    if (report != "") print msg >> report
}

# Lines to put before line i, indented one step deeper than that line.
function insert_before(i, body) {
    if (i in ins) ins[i] = ins[i] "\001" body
    else ins[i] = body
}

function lead(s) {
    match(s, /^[ \t]*/)
    return substr(s, 1, RLENGTH)
}

END {
    n = split(doc, L, "\n")
    cr = (n && L[1] ~ /\r$/) ? "\r" : ""
    depth = 0
    prev = ""
    for (i = 1; i <= n; i++) {
        line = L[i]
        p = index(line, "#")
        if (p) line = substr(line, 1, p - 1)
        gsub(/\{/, " { ", line)
        gsub(/\}/, " } ", line)
        nt = split(line, tok, /[ \t\r]+/)
        first = 1
        for (k = 1; k <= nt; k++) {
            t = tok[k]
            if (t == "") continue
            if (t == "{") {
                depth++
                stack[depth] = prev
                prev = ""
                if (depth == 1) top_open[stack[1]] = i
                if (depth == 2 && stack[1] == "pre_processing") src_seen[stack[2]] = 1
                if (depth == 3 && stack[1] == "pre_processing") src_apply[stack[2]] = src_apply[stack[2]] " " stack[3]
                first = 0
                continue
            }
            if (t == "}") {
                if (depth == 1 && first) top_close[stack[1]] = i
                if (depth == 2 && stack[1] == "pre_processing") src_close[stack[2]] = first ? i : -1
                if (depth > 0) depth--
                prev = ""
                first = 0
                continue
            }
            first = 0
            if (prev == "") { prev = t; continue }
            # key value
            if (depth == 2 && stack[1] == "libraries" && prev == "path") lib_path[stack[2]] = t
            if (depth == 2 && stack[1] == "effects" && prev == "uuid") { eff_uuid[stack[2]] = t; eff_order[++neff] = stack[2] }
            if (depth == 3 && stack[1] == "effects" && stack[3] == "libsw" && prev == "uuid") eff_sw[stack[2]] = t
            prev = ""
        }
    }

    taken_libs = " "
    for (e in lib_path) taken_libs = taken_libs e " "
    taken_effs = " "
    for (k = 1; k <= neff; k++) taken_effs = taken_effs eff_order[k] " "

    for (k = 1; k <= neff; k++) {
        e = eff_order[k]
        t = effect_kind(e)
        u = tolower(eff_uuid[e])
        if (u == UUID_MICNS) micns_eff = e
        if (u == UUID_MICNS_PURE) micns_pure_eff = e
        if (u == UUID_MICNS_CALL) micns_call_eff = e
        if (t == "" || (t in sys_eff) || u == UUID_MICNS || u == UUID_MICNS_PURE || u == UUID_MICNS_CALL) continue
        sys_eff[t] = e
    }
    for (e in lib_path) {
        if (lib_path[e] ~ /\/libmicns\.so$/) micns_lib = e
        if (lib_path[e] ~ /\/libaudiopreprocessing\.so$/) aosp_lib = e
    }

    new_libs = ""
    new_effs = ""
    nsrc = split(plan, items, ";")
    for (q = 1; q <= nsrc; q++) {
        if (items[q] == "") continue
        eq = index(items[q], "=")
        src = substr(items[q], 1, eq - 1)
        nroles = split(substr(items[q], eq + 1), roles, ",")
        split("", have)
        if (src in src_seen) {
            na = split(src_apply[src], ap, " ")
            for (k = 1; k <= na; k++) if (ap[k] != "") {
                t = effect_kind(ap[k])
                if (t != "") have[t] = 1
                if (tolower(eff_uuid[ap[k]]) == UUID_MICNS_CALL) have["call"] = 1
            }
        }
        adds = ""
        missing = 0
        for (k = 1; k <= nroles; k++) {
            role = roles[k]
            if (role == "") continue
            want_kind = (role == "ai" || role == "ai_pure") ? "ns" : role == "ai_call" ? "call" : role
            if (want_kind in have) continue
            nm = ""
            if (role == "ai_call") {
                # an extra stage on top of the platform effects, never replaced
                if (!micns_ok) continue
                if (micns_lib == "") {
                    micns_lib = unique("micns", taken_libs)
                    taken_libs = taken_libs micns_lib " "
                    new_libs = new_libs "\001" micns_lib " {\001\002path " micns_path "\001}"
                }
                if (micns_call_eff == "") {
                    micns_call_eff = unique("micns_call", taken_effs)
                    taken_effs = taken_effs micns_call_eff " "
                    new_effs = new_effs "\001" micns_call_eff " {\001\002library " micns_lib "\001\002uuid " UUID_MICNS_CALL "\001}"
                }
                nm = micns_call_eff
            } else if ((role == "ai" || role == "ai_pure") && micns_ok) {
                if (micns_lib == "") {
                    micns_lib = unique("micns", taken_libs)
                    taken_libs = taken_libs micns_lib " "
                    new_libs = new_libs "\001" micns_lib " {\001\002path " micns_path "\001}"
                }
                if (role == "ai") {
                    if (micns_eff == "") {
                        micns_eff = unique("micns", taken_effs)
                        taken_effs = taken_effs micns_eff " "
                        new_effs = new_effs "\001" micns_eff " {\001\002library " micns_lib "\001\002uuid " UUID_MICNS "\001}"
                    }
                    nm = micns_eff
                } else {
                    if (micns_pure_eff == "") {
                        micns_pure_eff = unique("micns_pure", taken_effs)
                        taken_effs = taken_effs micns_pure_eff " "
                        new_effs = new_effs "\001" micns_pure_eff " {\001\002library " micns_lib "\001\002uuid " UUID_MICNS_PURE "\001}"
                    }
                    nm = micns_pure_eff
                }
            } else if (want_kind in sys_eff) {
                nm = sys_eff[want_kind]
            } else if (aosp_ok && aosp_path != "") {
                if (aosp_lib == "") {
                    aosp_lib = unique("pre_processing", taken_libs)
                    taken_libs = taken_libs aosp_lib " "
                    new_libs = new_libs "\001" aosp_lib " {\001\002path " aosp_path "\001}"
                }
                nm = unique(want_kind, taken_effs)
                taken_effs = taken_effs nm " "
                new_effs = new_effs "\001" nm " {\001\002library " aosp_lib "\001\002uuid " aosp_uuid[want_kind] "\001}"
                sys_eff[want_kind] = nm
            }
            if (nm == "") { say("  - " src ": no " want_kind " effect available"); missing = 1; continue }
            have[want_kind] = 1
            adds = adds " " nm
        }
        if (adds == "") { if (!missing) say("  = " src ": already handled by the device"); continue }
        if ((src in src_seen) && src_close[src] <= 0) {
            say("  ! " src ": vendor block is written on one line, left as is")
            continue
        }
        shown = substr(adds, 2)
        gsub(/ /, ",", shown)
        say("  + " src ":" ((src in src_seen) ? " (vendor stream) " : " ") shown)
        na = split(adds, ap, " ")
        body = ""
        for (k = 1; k <= na; k++) if (ap[k] != "") body = body "\001" ap[k] " {\001}"
        if (src in src_seen) insert_before(src_close[src], body)
        else {
            blk = "\001" src " {"
            nb = split(body, bl, "\001")
            for (k = 1; k <= nb; k++) if (bl[k] != "") blk = blk "\001\002" bl[k]
            new_srcs = new_srcs blk "\001}"
        }
    }

    tail = ""
    tail = tail add_top("libraries", new_libs)
    tail = tail add_top("effects", new_effs)
    tail = tail add_top("pre_processing", new_srcs)

    out = ""
    for (i = 1; i <= n; i++) {
        # a trailing newline leaves an empty last element: the end-of-file
        # block goes before it so the file still ends with a newline
        if (i == n && L[i] == "" && tail != "") { out = out emit("", tail, 0); tail = "" }
        if (i in ins) out = out emit(lead(L[i]), ins[i], 1)
        out = out L[i] (i < n ? "\n" : "")
    }
    if (tail != "") out = out "\n" substr(emit("", tail, 0), 1, length(emit("", tail, 0)) - 1)
    printf "%s", out
}

# Children for a top-level block: inside it when its closing brace has a
# line of its own, as a new block at the end of the file when it does not
# exist. A top-level block closed mid-line cannot be extended safely (the
# loader only reads the first block of a name), so it is left alone.
function add_top(name, body) {
    if (body == "") return ""
    if (name in top_close) { insert_before(top_close[name], body); return "" }
    if (name in top_open) { say("  ! " name " block is closed mid-line, not extended"); return "" }
    return "\001" name " {" deepen(body) "\001}"
}

function deepen(body,    out, k, nb, parts) {
    out = ""
    nb = split(body, parts, "\001")
    for (k = 1; k <= nb; k++) if (parts[k] != "") out = out "\001\002" parts[k]
    return out
}

# One marker-wrapped group of lines. "\002" adds a level of indentation;
# `child` puts the whole group one level below `ind`.
# Returns the lines, each terminated by a newline.
function emit(ind, body, child,    k, nb, parts, s, li, step, out) {
    step = (ind ~ /\t/) ? "\t" : "  "
    out = ind "# micns:L" cr "\n"
    nb = split(body, parts, "\001")
    for (k = 1; k <= nb; k++) {
        s = parts[k]
        if (s == "") continue
        li = child ? ind step : ind
        while (substr(s, 1, 1) == "\002") { li = li step; s = substr(s, 2) }
        out = out li s cr "\n"
    }
    return out ind "# micns:/L" cr "\n"
}

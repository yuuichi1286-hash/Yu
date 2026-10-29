# MicNS patcher for audio_effects.xml / audio_effects_config.xml.
#
# Adds input pre-processing (noise suppression and friends) to the capture
# sources the module covers, reusing the effects the device already ships
# and, where allowed, registering the MicNS RNNoise library.
#
# Only ever *inserts* text, wrapped in markers, so a previous run can be
# undone byte for byte (see micns_strip.awk):
#   line mode:   whole lines between "<!--micns:L-->" and "<!--/micns:L-->"
#   inline mode: "<!--micns:I-->...<!--/micns:I-->" inside a line
#
# Variables (-v):
#   plan      "source=role,role;source=role" with roles ai, ai_pure, ai_call,
#             ns, aec, agc (ai_call: micns_call appended after the other
#             effects, only when micns_ok)
#   micns_ok  1 if libmicns.so is installed and loadable for this file
#   aosp_ok   1 if the AOSP pre-processing library exists on the device
#   aosp_file its file name (default libaudiopreprocessing.so; the AIDL
#             effect HAL ships libpreprocessingaidl.so instead)
#   report    file to append a human-readable summary to (optional)
#
# Runs on BusyBox awk: POSIX features only.

BEGIN {
    RS = "<"
    if (aosp_file == "") aosp_file = "libaudiopreprocessing.so"
    n = 0
    pending = ""
    UUID_MICNS = "2470ae61-735c-41ba-9565-6923cc713c54"
    UUID_MICNS_PURE = "c6b4d85a-7071-4520-9121-828cff4f596f"
    UUID_MICNS_CALL = "d7bbe82a-9292-411e-9656-52b912ece427"
    # implementation (and type) UUIDs of known pre-processing effects
    kind["0f8d0d2a-59e5-45fe-b6e4-248c8a799109"] = "aec"   # Qualcomm
    kind["bb392ec0-8d4d-11e0-a896-0002a5d5c51b"] = "aec"   # AOSP WebRTC
    kind["7b491460-8d4d-11e0-bd61-0002a5d5c51b"] = "aec"   # type
    kind["1d97bb0b-9e2f-4403-9ae3-58c2554306f8"] = "ns"    # Qualcomm
    kind["c06c8400-8e06-11e0-9cb6-0002a5d5c51b"] = "ns"    # AOSP WebRTC
    kind["58b4b260-8e06-11e0-aa8e-0002a5d5c51b"] = "ns"    # type
    kind[UUID_MICNS] = "ns"
    kind[UUID_MICNS_PURE] = "ns"
    kind[UUID_MICNS_CALL] = "ns"
    kind["aa8130e0-66fc-11e0-bad0-0002a5d5c51b"] = "agc"   # AOSP WebRTC
    kind["89f38e65-d4d2-4d64-ad0e-2b3e799ea886"] = "agc"   # AOSP AGC2
    kind["0a8abfe0-654c-11e0-ba26-0002a5d5c51b"] = "agc"   # type
    aosp_uuid["aec"] = "bb392ec0-8d4d-11e0-a896-0002a5d5c51b"
    aosp_uuid["ns"] = "c06c8400-8e06-11e0-9cb6-0002a5d5c51b"
    aosp_uuid["agc"] = "aa8130e0-66fc-11e0-bad0-0002a5d5c51b"
}

# Records are split on "<". A comment may contain "<"; glue it back.
{
    if (pending != "") {
        pending = pending "<" $0
        if (index(pending, "-->")) { R[++n] = pending; pending = "" }
        next
    }
    if (NR > 1 && substr($0, 1, 3) == "!--" && !index($0, "-->")) { pending = $0; next }
    R[++n] = $0
}

function attr(tag, name,    s, re, v) {
    s = " " tag
    re = "[ \t\r\n]" name "[ \t\r\n]*=[ \t\r\n]*(\"[^\"]*\"|'[^']*')"
    if (!match(s, re)) return ""
    v = substr(s, RSTART, RLENGTH)
    sub(/^[^=]*=[ \t\r\n]*/, "", v)
    return substr(v, 2, length(v) - 2)
}

function name_kind(nm,    l) {
    l = tolower(nm)
    if (l == "ns" || l ~ /noise_?supp/) return "ns"
    if (l == "aec" || l ~ /echo_?cancel/) return "aec"
    if (l == "agc" || l == "agc2" || l ~ /gain_?control/) return "agc"
    return ""
}

function effect_kind(nm,    u, k) {
    u = tolower(eff_uuid[nm])
    if (u in kind) return kind[u]
    u = tolower(eff_sw[nm])
    if (u in kind) return kind[u]
    k = name_kind(nm)
    return k
}

# Text that follows the tag of record i (after its ">").
function tail_text(i,    r, p) {
    r = R[i]
    if (i == 1) return r
    if (substr(r, 1, 3) == "!--") {
        p = index(r, "-->")
        return p ? substr(r, p + 3) : ""
    }
    p = index(r, ">")
    return p ? substr(r, p + 1) : ""
}

# Indentation of the line holding record i's tag, or "" if it is not the
# first thing on its line.
function line_indent(i,    t, k, c) {
    t = tail_text(i - 1)
    for (k = length(t); k > 0; k--) {
        c = substr(t, k, 1)
        if (c == "\n") return "@" substr(t, k + 1)
        if (c != " " && c != "\t") return ""
    }
    return (i - 1 == 1) ? "@" t : ""
}

function child_indent(ind) {
    if (ind ~ /\t$/) return ind "\t"
    return ind "    "
}

# Queue `lines` (already indented, each ending without newline, joined by
# "\001") to be inserted right before record i.
function insert_before(i, lines) {
    if (i in ins) ins[i] = ins[i] "\001" lines
    else ins[i] = lines
}

function unique_name(base,    nm, k) {
    nm = base
    k = 2
    while ((nm in eff_uuid) || (nm in used_name)) nm = base "_" k++
    used_name[nm] = 1
    return nm
}

function unique_lib(base,    nm, k) {
    nm = base
    k = 2
    while ((nm in lib_path) || (nm in used_lib)) nm = base "_" k++
    used_lib[nm] = 1
    return nm
}

function say(msg) {
    if (report != "") print msg >> report
}

END {
    if (pending != "") R[++n] = pending
    nl = index(R[1] R[2] R[3], "\r\n") ? "\r\n" : "\n"
    depth = 0
    root_close = libs_close = effs_close = pre_close = 0
    nstreams = 0
    for (i = 2; i <= n; i++) {
        r = R[i]
        c1 = substr(r, 1, 1)
        if (c1 == "!" || c1 == "?") continue
        p = index(r, ">")
        if (!p) continue
        tag = substr(r, 1, p - 1)
        if (c1 == "/") {
            nm = substr(tag, 2)
            sub(/[ \t\r\n].*$/, "", nm)
            if (depth == 1 && nm == "audio_effects_conf") root_close = i
            if (depth == 2 && stack[2] == "libraries" && !libs_close) libs_close = i
            if (depth == 2 && stack[2] == "effects" && !effs_close) effs_close = i
            if (depth == 2 && stack[2] == "preprocess" && !pre_close) pre_close = i
            if (depth == 3 && stack[2] == "preprocess" && stack[3] == "stream") s_close[cur_stream] = i
            if (depth > 0) depth--
            continue
        }
        selfc = (substr(tag, length(tag), 1) == "/")
        if (selfc) tag = substr(tag, 1, length(tag) - 1)
        nm = tag
        sub(/[ \t\r\n].*$/, "", nm)
        d = depth + 1
        if (d == 2 && stack[1] == "audio_effects_conf") {
            if (nm == "libraries") has_libs = 1
            if (nm == "effects") has_effs = 1
            if (nm == "preprocess" && !selfc) has_pre = 1
        }
        if (d == 3 && stack[2] == "libraries" && nm == "library") {
            ln = attr(tag, "name")
            lib_path[ln] = attr(tag, "path")
        }
        if (d == 3 && stack[2] == "effects" && (nm == "effect" || nm == "effectProxy")) {
            cur_eff = attr(tag, "name")
            eff_uuid[cur_eff] = attr(tag, "uuid")
            eff_lib[cur_eff] = attr(tag, "library")
            eff_order[++neff] = cur_eff
        }
        if (d == 4 && stack[2] == "effects" && stack[3] == "effectProxy" && nm == "libsw")
            eff_sw[cur_eff] = attr(tag, "uuid")
        if (d == 3 && stack[2] == "preprocess" && nm == "stream") {
            cur_stream = ++nstreams
            s_type[cur_stream] = attr(tag, "type")
            s_open[cur_stream] = i
            s_self[cur_stream] = selfc
        }
        if (d == 4 && stack[2] == "preprocess" && stack[3] == "stream" && nm == "apply")
            s_apply[cur_stream] = s_apply[cur_stream] "," attr(tag, "effect")
        if (!selfc) { depth = d; stack[d] = nm }
    }

    if (!root_close) {
        say("  ! not an audio_effects_conf document, left untouched")
        for (i = 1; i <= n; i++) printf "%s%s", (i > 1 ? "<" : ""), R[i]
        exit
    }

    # Effects available per kind, first declared wins (skip our own).
    for (k = 1; k <= neff; k++) {
        e = eff_order[k]
        t = effect_kind(e)
        if (t == "" || (t in sys_eff)) continue
        u = tolower(eff_uuid[e])
        if (u == UUID_MICNS || u == UUID_MICNS_PURE || u == UUID_MICNS_CALL) continue
        sys_eff[t] = e
    }
    for (e in lib_path) if (lib_path[e] ~ /(^|\/)libmicns\.so$/) micns_lib = e
    for (e in lib_path) {
        f = lib_path[e]
        sub(/^.*\//, "", f)
        if (f == aosp_file) aosp_lib = e
    }
    for (k = 1; k <= neff; k++) {
        u = tolower(eff_uuid[eff_order[k]])
        if (u == UUID_MICNS) micns_eff = eff_order[k]
        if (u == UUID_MICNS_PURE) micns_pure_eff = eff_order[k]
        if (u == UUID_MICNS_CALL) micns_call_eff = eff_order[k]
    }

    nsrc = split(plan, items, ";")
    new_libs = ""
    new_effs = ""
    for (q = 1; q <= nsrc; q++) {
        if (items[q] == "") continue
        eq = index(items[q], "=")
        src = substr(items[q], 1, eq - 1)
        nroles = split(substr(items[q], eq + 1), roles, ",")
        # stream already declared by the vendor?
        sidx = 0
        for (k = 1; k <= nstreams; k++) if (s_type[k] == src && !s_self[k]) sidx = k
        split("", have)
        if (sidx) {
            na = split(s_apply[sidx], ap, ",")
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
                    micns_lib = unique_lib("micns")
                    new_libs = new_libs "\001<library name=\"" micns_lib "\" path=\"libmicns.so\"/>"
                }
                if (micns_call_eff == "") {
                    micns_call_eff = unique_name("micns_call")
                    new_effs = new_effs "\001<effect name=\"" micns_call_eff "\" library=\"" micns_lib "\" uuid=\"" UUID_MICNS_CALL "\"/>"
                }
                nm = micns_call_eff
            } else if ((role == "ai" || role == "ai_pure") && micns_ok) {
                if (micns_lib == "") {
                    micns_lib = unique_lib("micns")
                    new_libs = new_libs "\001<library name=\"" micns_lib "\" path=\"libmicns.so\"/>"
                }
                if (role == "ai") {
                    if (micns_eff == "") {
                        micns_eff = unique_name("micns")
                        new_effs = new_effs "\001<effect name=\"" micns_eff "\" library=\"" micns_lib "\" uuid=\"" UUID_MICNS "\"/>"
                    }
                    nm = micns_eff
                } else {
                    if (micns_pure_eff == "") {
                        micns_pure_eff = unique_name("micns_pure")
                        new_effs = new_effs "\001<effect name=\"" micns_pure_eff "\" library=\"" micns_lib "\" uuid=\"" UUID_MICNS_PURE "\"/>"
                    }
                    nm = micns_pure_eff
                }
            } else if (want_kind in sys_eff) {
                nm = sys_eff[want_kind]
            } else if (aosp_ok) {
                if (aosp_lib == "") {
                    aosp_lib = unique_lib("pre_processing")
                    new_libs = new_libs "\001<library name=\"" aosp_lib "\" path=\"" aosp_file "\"/>"
                }
                nm = unique_name(want_kind)
                new_effs = new_effs "\001<effect name=\"" nm "\" library=\"" aosp_lib "\" uuid=\"" aosp_uuid[want_kind] "\"/>"
                sys_eff[want_kind] = nm
            }
            if (nm == "") { say("  - " src ": no " want_kind " effect available"); missing = 1; continue }
            have[want_kind] = 1
            adds = adds "," nm
        }
        if (adds == "") { if (!missing) say("  = " src ": already handled by the device"); continue }
        say("  + " src ":" (sidx ? " (vendor stream)" : "") " " substr(adds, 2))
        if (sidx) stream_adds[sidx] = adds
        else new_streams = new_streams ";" src "=" adds
    }

    # ---- queue insertions -------------------------------------------------
    if (new_libs != "") {
        if (libs_close) queue_children(libs_close, new_libs)
        else queue_block(root_close, "libraries", new_libs)
    }
    if (new_effs != "") {
        if (effs_close) queue_children(effs_close, new_effs)
        else queue_block(root_close, "effects", new_effs)
    }
    for (k = 1; k <= nstreams; k++) {
        if (!(k in stream_adds)) continue
        na = split(stream_adds[k], ap, ",")
        body = ""
        for (j = 1; j <= na; j++) if (ap[j] != "") body = body "\001<apply effect=\"" ap[j] "\"/>"
        queue_children(s_close[k], body)
    }
    if (new_streams != "") {
        ns = split(new_streams, sl, ";")
        body = ""
        for (k = 1; k <= ns; k++) {
            if (sl[k] == "") continue
            eq = index(sl[k], "=")
            body = body "\001<stream type=\"" substr(sl[k], 1, eq - 1) "\">"
            na = split(substr(sl[k], eq + 1), ap, ",")
            for (j = 1; j <= na; j++) if (ap[j] != "") body = body "\001\002<apply effect=\"" ap[j] "\"/>"
            body = body "\001</stream>"
        }
        if (has_pre && pre_close) queue_children(pre_close, body)
        else queue_block(root_close, "preprocess", body)
    }

    # Splice everything in first (an insertion edits the record before its
    # target), then print.
    for (i = n; i >= 2; i--) if (i in ins) emit_insert(i)
    for (i = 1; i <= n; i++) printf "%s%s", (i > 1 ? "<" : ""), R[i]
}

# Children go right before the closing tag at record i, one level deeper.
function queue_children(i, body) {
    insert_before(i, "\003" body)
}

# A whole new <name> element with children, before the closing tag at i.
function queue_block(i, name, body,    b, k, nb, parts) {
    b = "\003\001<" name ">"
    nb = split(body, parts, "\001")
    for (k = 1; k <= nb; k++) if (parts[k] != "") b = b "\001\002" parts[k]
    b = b "\001</" name ">"
    insert_before(i, b)
}

# Render queued lines for record i in line mode when its tag starts a line,
# otherwise inline. "\001" separates lines, "\002" adds one indent level,
# "\003" marks the start of a group whose lines are one level deeper than
# the closing tag they precede.
function emit_insert(i,    ind, li, nlines, lines, k, s, lvl, out, t, cut) {
    ind = line_indent(i)
    nlines = split(ins[i], lines, "\001")
    if (ind != "") {
        ind = substr(ind, 2)
        out = ind "<!--micns:L-->" nl
        for (k = 1; k <= nlines; k++) {
            s = lines[k]
            if (s == "" || s == "\003") continue
            if (substr(s, 1, 1) == "\003") s = substr(s, 2)
            li = child_indent(ind)
            while (substr(s, 1, 1) == "\002") { li = child_indent(li); s = substr(s, 2) }
            out = out li s nl
        }
        out = out ind "<!--/micns:L-->" nl
        # put the block at the start of the closing tag's line
        t = R[i - 1]
        cut = length(t) - length(ind)
        R[i - 1] = substr(t, 1, cut) out substr(t, cut + 1)
    } else {
        out = "<!--micns:I-->"
        for (k = 1; k <= nlines; k++) {
            s = lines[k]
            gsub("\002", "", s)
            gsub("\003", "", s)
            out = out s
        }
        out = out "<!--/micns:I-->"
        # inline: emitted straight before the "<" of record i
        R[i - 1] = R[i - 1] out
    }
}

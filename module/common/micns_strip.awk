# Undo a previous MicNS patch (audio_effects .xml or .conf): drop the
# marker-wrapped lines and inline spans the patchers inserted. Everything
# else is passed through byte for byte (the file is read whole, so even a
# missing final newline survives).
BEGIN { RS = "\001" }
{ doc = (NR > 1) ? doc RS $0 : $0 }
END {
    n = split(doc, line, "\n")
    out = ""
    kept = 0
    skipping = 0
    for (i = 1; i <= n; i++) {
        l = line[i]
        if (skipping) {
            if (index(l, "<!--/micns:L-->") || l ~ /^[ \t]*# micns:\/L[ \t\r]*$/) skipping = 0
            continue
        }
        if (index(l, "<!--micns:L-->") || l ~ /^[ \t]*# micns:L[ \t\r]*$/) {
            skipping = 1
            continue
        }
        while ((s = index(l, "<!--micns:I-->")) > 0) {
            e = index(l, "<!--/micns:I-->")
            if (e < s) break
            l = substr(l, 1, s - 1) substr(l, e + 15)
        }
        out = kept++ ? out "\n" l : l
    }
    printf "%s", out
}

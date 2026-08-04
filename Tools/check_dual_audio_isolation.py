#!/usr/bin/env python3
"""Structural check for the "dual-audio-server" test category (see finlink's
test-categorization project notes): confirms finlink's audio-forwarding hook
(GBAStreamHost::ForwardAudioSamples) is only ever called from the
per-GBA-slot audio callback (GBACore.cpp's ReadAudioBufferIntoMixer), and
that no GBAStreamHost/finlink reference exists anywhere in the GameCube's
own audio hardware emulation (DSP/AudioInterface/StreamADPCM/AudioCommon) --
i.e. a connected finlink client only ever diverts its own GBA slot's audio,
the GameCube's own TV/DSP audio output (and every other GBA slot) keeps
playing locally, completely untouched.

Like Cemu's equivalent check (tools/check_dual_audio_isolation.py there),
this stays static/structural rather than a runtime test: GBAStreamHost.cpp
needs a real, connected mGBA core to exercise for real, and this property
(WHICH code path finlink hooks into) is architectural, not data-dependent --
provable by confirming which files/functions reference the symbol, no
execution required.

Exit code is non-zero (with the offending file/function printed) if a
finlink/GBAStreamHost reference is ever found in the GameCube's own audio
path, or if ReadAudioBufferIntoMixer stops referencing ForwardAudioSamples
at all (the hook silently regressing away).
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GBA_CORE_CPP = REPO_ROOT / "Source" / "Core" / "Core" / "HW" / "GBACore.cpp"

# Files making up the GameCube's OWN audio hardware emulation -- must never
# reference GBAStreamHost/finlink at all. Globs, evaluated relative to
# REPO_ROOT.
GC_AUDIO_PATH_GLOBS = [
    "Source/Core/AudioCommon/*.cpp",
    "Source/Core/Core/HW/DSP.cpp",
    "Source/Core/Core/HW/DSPLLE/*.cpp",
    "Source/Core/Core/HW/DSPHLE/*.cpp",
    "Source/Core/Core/HW/AudioInterface.cpp",
    "Source/Core/Core/HW/StreamADPCM.cpp",
]

FINLINK_RE = re.compile(r"GBAStreamHost|ForwardAudioSamples|[Ff]inlink")

LINE_COMMENT_RE = re.compile(r"//.*$", re.MULTILINE)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(code: str) -> str:
    return LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", code))


def extract_function_body(text: str, func_name_pattern: str):
    """Naive brace-counting extraction, same approach (and same reasoning)
    as Cemu's check_dual_audio_isolation.py."""
    lines = text.splitlines()
    start_re = re.compile(func_name_pattern)
    for i, line in enumerate(lines):
        if start_re.search(line):
            depth = 0
            started = False
            body_lines = []
            j = i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    started = True
                body_lines.append(lines[j])
                j += 1
                if started and depth == 0:
                    break
            return "\n".join(body_lines)
    return None


def main() -> int:
    failures = []

    if not GBA_CORE_CPP.is_file():
        sys.exit(f"error: {GBA_CORE_CPP} does not exist")
    text = GBA_CORE_CPP.read_text(encoding="utf-8")
    body = extract_function_body(text, r"^static void ReadAudioBufferIntoMixer\(")
    if body is None:
        failures.append(f"ReadAudioBufferIntoMixer: function not found in {GBA_CORE_CPP} (renamed/removed?)")
    elif not FINLINK_RE.search(strip_comments(body)):
        failures.append(
            "ReadAudioBufferIntoMixer: expected to reference ForwardAudioSamples "
            "(this is where per-GBA-slot audio forwarding hooks in) but doesn't -- "
            "did the hook get moved or dropped?"
        )

    for glob in GC_AUDIO_PATH_GLOBS:
        matched_any = False
        for path in REPO_ROOT.glob(glob):
            if not path.is_file():
                continue
            matched_any = True
            content = strip_comments(path.read_text(encoding="utf-8", errors="ignore"))
            if FINLINK_RE.search(content):
                failures.append(
                    f"{path.relative_to(REPO_ROOT)}: references finlink/GBAStreamHost but is part of "
                    f"the GameCube's own audio path -- a connected finlink client must never intercept "
                    f"TV/DSP audio, only its own GBA slot's audio"
                )
        if not matched_any:
            failures.append(f"{glob}: matched no files (renamed/removed path -- update this script)")

    if failures:
        print("Dual-audio-server isolation check failed:\n")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("OK: finlink's audio hook is confined to ReadAudioBufferIntoMixer, GameCube's own audio path is untouched.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

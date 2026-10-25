#pragma once
#include <JuceHeader.h>

// Drag-in (Andrew, 10-17): audio files dropped on the tape rack are copied into the
// current bank folder. The rules live here, apart from any UI, so the tests drive
// them on plain files: RackDropTarget (the native drop view) asks check(), UiBridge's
// drop worker runs copyAll() and rescans, and the bridge picks.
namespace loopnest::bankdrop
{
    // The extensions the bank scan accepts (LoopNestProcessor::rescanFolder and the
    // drum extractor's gather): one list, so a drop never copies in a file the rack
    // can't show. Matched case-insensitively, as the scan does on macOS.
    inline constexpr const char* kAudioFileWildcard = "*.wav;*.aiff;*.aif;*.mp3";

    // An existing file (not a folder) whose name matches kAudioFileWildcard. A link counts
    // by its own name when it leads to a file; a dangling one doesn't.
    bool isAudioFile(const juce::File&);

    // The file a link leads to, through a chain of links; anything else as it is.
    juce::File resolveLinks(juce::File);

    // Whether a drop may land: a bank folder that exists and at least one audio file
    // among the items. Folders and other files ride along and are skipped (Andrew, 10-18:
    // a pack dragged from Finder often carries a readme or a subfolder).
    enum class Verdict { ok, noBank, nothing };
    Verdict check(const juce::Array<juce::File>& items, const juce::File& bank);

    // One file's copy into the bank's top level:
    //   copied   a new file at dest: `name.ext`, else the first free `name 2.ext`,
    //            `name 3.ext`, ... (a taken name with different bytes is never overwritten)
    //   skipped  that name (or one of its numbered names) already holds the same bytes:
    //            nothing written, dest is that file
    //   inBank   the file is already somewhere in the bank's tree: used where it is
    //   failed   the copy failed, or src leads nowhere (dest empty, no partial file left behind)
    // A symlink is copied as its target's bytes under the link's name (and is inBank when
    // its target is); a link already at a name in the bank counts as taken.
    enum class Outcome { copied, skipped, inBank, failed };
    struct Result
    {
        juce::File dest;
        Outcome outcome = Outcome::failed;
    };
    Result copyInto(const juce::File& src, const juce::File& bank);

    // check() first, then copyInto() for each audio item in order. Empty when refused.
    // Run on a juce::Thread that has been asked to exit, it stops before the next file
    // (UiBridge's drop worker: a closing editor waits only on the file in hand, its copy
    // or its same-bytes compare).
    std::vector<Result> copyAll(const juce::Array<juce::File>& items, const juce::File& bank);
}

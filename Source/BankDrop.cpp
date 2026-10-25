#include "BankDrop.h"

namespace loopnest::bankdrop
{
bool isAudioFile(const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    const auto name = f.getFileName();
    for (const auto& w : juce::StringArray::fromTokens(kAudioFileWildcard, ";", ""))
        if (name.matchesWildcard(w, true))
            return true;
    return false;
}

Verdict check(const juce::Array<juce::File>& items, const juce::File& bank)
{
    if (! bank.isDirectory()) return Verdict::noBank;
    for (const auto& f : items)
        if (isAudioFile(f)) return Verdict::ok;
    return Verdict::nothing;
}

juce::File resolveLinks(juce::File f)
{
    // A bounded walk: a link loop ends on a link, which existsAsFile() then refuses.
    for (int hops = 0; hops < 32 && f.isSymbolicLink(); ++hops)
    {
        const auto t = f.getNativeLinkedTarget();
        // A relative target is relative to the link's folder ("./": not a home path).
        f = t.startsWithChar('/') ? juce::File(t) : f.getParentDirectory().getChildFile("./" + t);
    }
    return f;
}

Result copyInto(const juce::File& src, const juce::File& bank)
{
    // A link copies its target's bytes under the link's own name (U-M1): copying the
    // link itself leaves one the bank scan skips (and a relative one dangles).
    const auto from = resolveLinks(src);
    if (! from.existsAsFile()) return {};
    if (from.isAChildOf(bank)) return { from, Outcome::inBank };

    const auto stem = src.getFileNameWithoutExtension();
    const auto ext  = src.getFileExtension();
    for (int n = 1; n < 1000; ++n)
    {
        // "./" first: getChildFile reads a name that starts with '~' as a home path.
        const auto dest = bank.getChildFile("./" + (n == 1 ? src.getFileName() : stem + " " + juce::String(n) + ext));
        // A link at that name (even a dangling one) is taken, and never the same bytes.
        if (! dest.exists() && ! dest.isSymbolicLink())
        {
            if (from.copyFileTo(dest)) return { dest, Outcome::copied };
            dest.deleteFile();
            return {};
        }
        if (dest.existsAsFile() && ! dest.isSymbolicLink() && dest.hasIdenticalContentTo(from))
            return { dest, Outcome::skipped };
    }
    return {};
}

std::vector<Result> copyAll(const juce::Array<juce::File>& items, const juce::File& bank)
{
    std::vector<Result> out;
    if (check(items, bank) != Verdict::ok) return out;
    for (const auto& f : items)
    {
        if (juce::Thread::currentThreadShouldExit()) break;   // see the header
        if (isAudioFile(f))
            out.push_back(copyInto(f, bank));
    }
    return out;
}
}

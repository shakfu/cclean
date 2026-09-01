#ifndef CCLEAN_GLOB_HPP
#define CCLEAN_GLOB_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cclean/target.hpp"

namespace cclean {

namespace fs = std::filesystem;

// A glob compiled to tokens. std::regex dominated the scan: on a 64k-entry
// tree it cost 257 ms of a 414 ms run, because every entry is tested against
// every pattern. Matching is open-coded instead, with exact-, prefix-, and
// suffix-shaped patterns taking a string compare rather than the token walk.
//
// Supported wildcards: '*' (any sequence, '/' included), '?' (any single
// character), and "**/" (zero or more directory levels).
class Glob {
public:
    enum class Scope {
        NameOnly,   // Tested against the final path component only.
        NameOrPath  // Also tested against the path relative to ROOT.
    };

    // `reason` is what a target matching this pattern reports; it is carried
    // here so the walk never has to work it out from a pattern's position.
    Glob(const std::string& pattern,
         Scope scope,
         Reason reason = Reason::CommandLine);

    Scope scope() const { return scope_; }
    Reason reason() const { return reason_; }

    bool matches(std::string_view value) const;

private:
    struct Token {
        enum class Type {
            Character,  // A literal character.
            AnyOne,     // '?'
            AnySequence,// '*', which also spans '/'.
            AnyDirs     // "**/", i.e. nothing, or anything ending in '/'.
        };

        Type type;
        char character;
    };

    enum class Kind { Literal, Prefix, Suffix, General };

    std::vector<Token> tokens_;
    std::string text_;
    Kind kind_ = Kind::General;
    Scope scope_ = Scope::NameOrPath;
    Reason reason_ = Reason::CommandLine;

    void parse(const std::string& glob);
    void classify();
    void add_closure(std::vector<std::uint64_t>& states,
                     std::size_t index) const;
    bool match_tokens(std::string_view value) const;
};

// True when any pattern matches. The relative path is only built if some
// path-scoped pattern actually needs it.
bool matches_any(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns);

// The first matching pattern, or nullptr. The caller reads its reason().
const Glob* first_match(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns);

}  // namespace cclean

#endif  // CCLEAN_GLOB_HPP

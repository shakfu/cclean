#ifndef CCLEAN_TEXT_HPP
#define CCLEAN_TEXT_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace cclean {

namespace fs = std::filesystem;

// Clamps at the maximum rather than wrapping. A directory tree can report
// more bytes than the total can hold, and a wrapped total understates what a
// removal reclaims.
std::uintmax_t saturating_add(std::uintmax_t a, std::uintmax_t b);

// Binary units, two decimals above bytes: "1.50 KiB".
std::string format_size(std::uintmax_t bytes);

// A POSIX filename is a byte string: it can hold a newline, an ESC, or a
// complete CSI sequence, and it need not be valid UTF-8. Both matter here,
// because the matched list is exactly what the user reads before confirming a
// permanent deletion -- a name carrying a newline and some spaces can forge a
// second entry in that list, and one carrying ESC[2K can erase a real entry
// that was already printed.
//
// Every C0 control and DEL is therefore shown as \xNN, and a literal backslash
// is doubled so the escape is unambiguous. High bytes are passed through: they
// carry ordinary non-ASCII names, and a terminal does not act on them.
std::string display(const std::string& value);
std::string display(const fs::path& path);

// JSON is defined over text, not bytes, so emitting a filename verbatim
// produces output that a conforming parser rejects outright: a single
// undecodable byte in one name fails the whole document, warnings and totals
// included. Bytes that are not valid UTF-8 become U+FFFD, one per byte, which
// is the substitution the Unicode standard prescribes. The result includes the
// surrounding quotes.
std::string json_string(const std::string& value);

}  // namespace cclean

#endif  // CCLEAN_TEXT_HPP

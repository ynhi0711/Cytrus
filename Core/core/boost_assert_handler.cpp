// xappify fork: route boost's BOOST_ASSERT / BOOST_ASSERT_MSG through a throwing handler
// instead of the default assert() -> abort(). The boost asserts we actually hit are inside
// binary_iarchive deserialization of save states — e.g. the "trap invalid uninitialized
// boolean" in basic_binary_iprimitive.hpp when a stale or corrupt save produces a misaligned
// stream (observed as a GeometryEmitter bool trap during 3DS auto-resume). abort() is not
// catchable and kills the whole app; RunLoop already wraps System::LoadState in
// try/catch(std::exception) and degrades to ResultStatus::ErrorSavestate, so converting the
// abort into a throw lets that recovery run instead. Enabled by BOOST_ENABLE_ASSERT_HANDLER,
// defined for the Cytrus target only in Package.swift.
//
// Note: the primary guard against layout-drifted saves is the savestate-layout version in
// BuildStrings.mm (kCytrusFallbackRevision), which rejects stale saves BEFORE the destructive
// Shutdown/Init in System::serialize. This handler is the defense-in-depth net for a save that
// shares the current revision yet is still corrupt.

#include <stdexcept>
#include <string>

namespace boost {

void assertion_failed_msg(char const* expr, char const* msg, char const* function,
                          char const* file, long line) {
    std::string what = "boost assertion failed: ";
    what += (expr ? expr : "(null)");
    if (msg && *msg) {
        what += " — ";
        what += msg;
    }
    what += " in ";
    what += (function ? function : "(unknown)");
    what += " at ";
    what += (file ? file : "(unknown)");
    what += ':';
    what += std::to_string(line);
    throw std::runtime_error(what);
}

void assertion_failed(char const* expr, char const* function, char const* file, long line) {
    assertion_failed_msg(expr, nullptr, function, file, line);
}

} // namespace boost

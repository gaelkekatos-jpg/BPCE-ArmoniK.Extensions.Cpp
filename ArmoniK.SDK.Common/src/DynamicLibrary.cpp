#include <armonik/sdk/common/DynamicLibrary.h>

namespace ArmoniK {
namespace Sdk {
namespace Common {

// Out-of-class definitions required before C++17 for ODR-use of static constexpr members
// (implicitly inline since C++17, where these redeclarations are deprecated)
#if __cplusplus < 201703L && (!defined(_MSVC_LANG) || _MSVC_LANG < 201703L)
constexpr const char *DynamicLibrary::KeyConventionVersion;
constexpr const char *DynamicLibrary::KeyLibraryPath;
constexpr const char *DynamicLibrary::KeySymbol;
constexpr const char *DynamicLibrary::KeyLibraryBlobId;
constexpr const char *DynamicLibrary::ConventionVersion;
#endif

} // namespace Common
} // namespace Sdk
} // namespace ArmoniK

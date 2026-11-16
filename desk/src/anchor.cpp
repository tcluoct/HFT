// desk/src/anchor.cpp — anchor translation unit for the desk's header-only
// lineup composition (desk/pack.hpp). The pack is a compile-time type list;
// this TU gives the static_lib project an object and a target the platform
// binaries can link.
namespace desk {
int desk_anchor = 0;
}

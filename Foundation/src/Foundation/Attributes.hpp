#pragma once







#if defined(__has_cpp_attribute)
    #if __has_cpp_attribute(msvc::no_unique_address)
        #define STURDY_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
    #elif __has_cpp_attribute(no_unique_address)
        #define STURDY_NO_UNIQUE_ADDRESS [[no_unique_address]]
    #else
        #define STURDY_NO_UNIQUE_ADDRESS
    #endif
#else
    #define STURDY_NO_UNIQUE_ADDRESS
#endif

#pragma once

#include <stddef.h>
#include <unicode/udata.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ICU data symbols - defined in an object file
extern const unsigned char icu_data[];
extern const size_t icu_data_size;

// Function to initialize ICU with the embedded data
UBool icu_data_initializer(void);

// Helper function for external file loading
static bool loadICUDataFromFile(const char* filePath) {
    if (!filePath) return false;
    
    UErrorCode status = U_ZERO_ERROR;
    
    // Set the data directory
    u_setDataDirectory(filePath);
    
    // Prefer files over embedded data
    udata_setFileAccess(UDATA_FILES_FIRST, &status);
    if (U_FAILURE(status)) {
        return false;
    }
    
    // Test if ICU is properly initialized
    UErrorCode testStatus = U_ZERO_ERROR;
    UChar test[10];
    u_strFromUTF8(test, 10, NULL, "test", 4, &testStatus);
    
    return U_SUCCESS(testStatus);
}

#ifdef __cplusplus
}
#endif
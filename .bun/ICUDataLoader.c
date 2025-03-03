#include <stddef.h>
#include <unicode/udata.h>

// Declare the ICU data symbols - these are defined in the object file
extern const unsigned char icu_data[];
extern const size_t icu_data_size;

// This function initializes ICU with the embedded data
UBool icu_data_initializer(void) {
    UErrorCode status = U_ZERO_ERROR;
    
    // Set the ICU data source
    udata_setCommonData(icu_data, &status);
    
    // Return success or failure
    return U_SUCCESS(status);
}
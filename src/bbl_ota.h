#ifndef __c613c508_c470_4aad_8a41_e8f007b634c2__
#define __c613c508_c470_4aad_8a41_e8f007b634c2__

#include <stdbool.h>
#include <stdlib.h>

bool bbl_ota_refresh_info();
bool bbl_ota_update_available();
bool bbl_ota_download_update();
bool bbl_ota_get_changelog(char *buf, size_t len);

#endif

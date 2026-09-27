#pragma once
struct esp_partition_t { const char* label; };
const esp_partition_t* esp_ota_get_running_partition(void);

#pragma once
// Stand-in: frame_test can treat every pointer as IRAM to run the word-wise copy paths
inline bool fakeIram = false;
inline bool esp_ptr_in_iram(const void *) { return fakeIram; }

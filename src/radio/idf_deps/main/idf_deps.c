/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * This application is never flashed or executed. Declaring the radio
 * components in main/CMakeLists.txt makes ESP-IDF compile the exact archive
 * closure that src/radio later relinks into the Linux kernel payload.
 */
void app_main(void)
{
}

/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LIBRETRODROID_CORE_H
#define LIBRETRODROID_CORE_H

#include <string>

#include "../../libretro-common/include/libretro.h"

namespace libretrodroid {

struct retro_screenshot_info
{
    void *pixels;
    unsigned width;
    unsigned height;
    unsigned format;
    unsigned size;
    unsigned stride;
};

class Core {
public:
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    unsigned (*retro_api_version)(void);
    void (*retro_cheat_reset)(void);
    void (*retro_cheat_set)(unsigned index, bool enabled, const char *code);
    void (*retro_get_system_info)(struct retro_system_info *info);
    void (*retro_get_system_av_info)(struct retro_system_av_info *info);
    void (*retro_set_controller_port_device)(unsigned port, unsigned device);
    void (*retro_reset)(void);
    void (*retro_run)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *data, size_t size);
    bool (*retro_unserialize)(const void *data, size_t size);
    void (*retro_get_pixels)(struct retro_screenshot_info *info);
    size_t (*retro_get_memory_size)(unsigned id);
    void* (*retro_get_memory_data)(unsigned id);
    bool (*retro_load_game)(const struct retro_game_info *game);
    void (*retro_unload_game)(void);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    //void retro_set_rtc(unsigned type,int64_t value);
    void (*retro_set_rtc)(unsigned type,int64_t value);
    bool (*retro_link_set_sockets)(const int socketFds[4], int playerId, int playerCount) = nullptr;
    void (*retro_link_set_netplay_mode)(int mode, int inputDelayFrames) = nullptr;
    int (*retro_link_get_status)(void) = nullptr;
    bool (*retro_get_rom_crc32)(uint32_t* romCrc32) = nullptr;
    size_t (*retro_copy_unpatched_rom)(void* output, size_t capacity) = nullptr;
    bool (*retro_get_bios_info)(uint32_t* crc32, bool* fullBios) = nullptr;
    bool (*retro_link_native_active)(void) = nullptr;
    int64_t (*retro_link_remote_start)(int sequence, int mode, int transferCycles, uint32_t primaryData) = nullptr;
    bool (*retro_link_remote_mode)(int playerId, int mode) = nullptr;
    bool (*retro_link_remote_commit)(int sequence, const uint32_t data[4]) = nullptr;
    void (*retro_link_cancel)(void) = nullptr;
    bool (*retro_link_video_frame_is_stable)(const void *data) = nullptr;
    uint64_t (*retro_console_get_primary)(void) = nullptr;
    uint64_t (*retro_console_create)(const void *romData, size_t romSize,
                                     const void *sramData, size_t sramSize,
                                     uint32_t *romCrc32) = nullptr;
    bool (*retro_console_destroy)(uint64_t handle) = nullptr;
    bool (*retro_console_start_lan)(const int *socketFds, const uint64_t *handles,
                                    unsigned localPlayerId, unsigned playerCount) = nullptr;
    bool (*retro_console_get_rom_crc32)(uint64_t handle, uint32_t *romCrc32) = nullptr;
    bool (*retro_console_cheat_reset)(uint64_t handle) = nullptr;
    bool (*retro_console_cheat_set)(uint64_t handle, unsigned type, bool enabled,
                                    const char *code) = nullptr;
    bool (*retro_console_start_local)(const uint64_t *handles, unsigned playerCount) = nullptr;
    bool (*retro_console_set_foreground)(uint64_t handle) = nullptr;
    uint64_t (*retro_console_get_foreground)(void) = nullptr;
    bool (*retro_console_reset)(uint64_t handle) = nullptr;
    bool (*retro_console_get_pixels)(uint64_t handle,
                                     struct retro_screenshot_info* info) = nullptr;
    size_t (*retro_console_read_sram)(uint64_t handle, void *output, size_t outputSize) = nullptr;
    size_t (*retro_console_serialize_state)(uint64_t handle, void *output,
                                            size_t outputSize) = nullptr;
    bool (*retro_console_unserialize_state)(uint64_t handle, const void *data,
                                            size_t size) = nullptr;
    bool (*retro_console_unserialize_lan_state)(unsigned playerId, const void *data,
                                                size_t size, uint32_t generation) = nullptr;
    bool (*retro_console_reset_lan_player)(unsigned playerId,
                                           uint32_t generation) = nullptr;
    int (*retro_console_poll_lan_barrier)(uint32_t generation) = nullptr;
    bool (*retro_console_unserialize_lan_reset)(unsigned playerId, const void *data,
                                                size_t size, uint32_t generation) = nullptr;
    void (*retro_console_stop_local)(void) = nullptr;

    Core(const std::string& soCorePath);
    ~Core();

private:
    void open(const std::string& soCorePath);
    void close();

    void* libHandle = nullptr;
};

}



#endif //LIBRETRODROID_CORE_H

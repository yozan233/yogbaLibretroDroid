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

#ifndef LIBRETRODROID_LIBRETRODROID_H
#define LIBRETRODROID_LIBRETRODROID_H

#include <jni.h>

#include <EGL/egl.h>

#include <string>
#include <vector>
#include <array>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <optional>
#include <utility>

#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "environment.h"
#include "vfs/vfsfile.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/rect.h"

namespace libretrodroid {

class LibretroDroid {
public:
    static LibretroDroid& getInstance()
    {
        static LibretroDroid instance;
        return instance;
    }
    LibretroDroid(LibretroDroid const&) = delete;
    void operator=(LibretroDroid const&) = delete;

    void setViewport(Rect viewportRect);

private:
    LibretroDroid() {}

public:
    void setCheat(unsigned index, bool enabled, const std::string& code);
    void resetCheat();

    std::pair<int8_t*, size_t> serializeState();
    bool unserializeState(int8_t *data, size_t size);

    std::pair<int8_t *, size_t> serializeSRAM();
    jboolean unserializeSRAM(int8_t *data, size_t size);

    std::pair<int8_t*, jint*> getPixels();

    std::pair<int8_t *, size_t> getMemoryData(unsigned int offset,size_t size);
    void setMemoryData(unsigned int offset,int8_t* data,size_t size);

    //void retro_set_rtc(unsigned type,int64_t value);
    void setRTC(unsigned type,int64_t value);

    void setMultiplayer(int playerId, int playerCount);
    bool configureLinkNetplay(int mode, int inputDelayFrames);
    int getLinkNetplayStatus() const;
    int64_t getRomCrc32() const;
    size_t copyUnpatchedRom(void* output, size_t capacity) const;
    int64_t getBiosIdentity() const;
    bool setMultiplayerSockets(const int socketFds[4], int playerId, int playerCount);
    bool isNativeLinkActive() const;
    int64_t linkRemoteStart(int sequence, int mode, int transferCycles, uint32_t primaryData);
    bool linkRemoteMode(int playerId, int mode);
    bool linkRemoteCommit(int sequence, const uint32_t data[4]);
    void linkCancel();
    bool hasLinkSupport() const;
    int handleLinkExchange(unsigned type, int* args, uint16_t* data);

    uint64_t getPrimaryRomConsole() const;
    uint64_t createRomConsole(const int8_t* romData, size_t romSize,
                              const int8_t* sramData, size_t sramSize);
    bool destroyRomConsole(uint64_t handle);
    int64_t getRomConsoleCrc32(uint64_t handle) const;
    bool startLanLink(const int socketFds[4], const std::vector<uint64_t>& handles,
                      unsigned localPlayerId);
    bool startLocalLink(const std::vector<uint64_t>& handles);
    bool setForegroundRomConsole(uint64_t handle);
    uint64_t getForegroundRomConsole() const;
    bool resetRomConsole(uint64_t handle);
    std::pair<int8_t*, jint*> getRomConsolePixels(uint64_t handle);
    bool resetRomConsoleCheat(uint64_t handle);
    bool setRomConsoleCheat(uint64_t handle, unsigned type, bool enabled,
                            const std::string& code);
    std::pair<int8_t*, size_t> serializeRomConsoleSRAM(uint64_t handle);
    std::pair<int8_t*, size_t> serializeRomConsoleState(uint64_t handle);
    bool unserializeRomConsoleState(uint64_t handle, const int8_t* data, size_t size);
    bool unserializeLanPlayerState(unsigned playerId, const int8_t* data, size_t size,
                                   uint32_t generation);
    bool resetLanPlayer(unsigned playerId, uint32_t generation);
    int pollLanStateBarrier(uint32_t generation);
    bool unserializeLanResetState(unsigned playerId, const int8_t* data,
                                  size_t size, uint32_t generation);
    void stopLocalLink();
    bool hasRomConsoleSupport() const;

    void onSurfaceCreated();
    void onSurfaceChanged(unsigned int width, unsigned int height);

    void create(
        unsigned int GLESVersion,
        const std::string& soFilePath,
        const std::string& systemDir,
        const std::string& savesDir,
        std::vector<Variable> variables,
        const ShaderManager::Config& shaderConfig,
        float refreshRate,
        bool lowLatencyAudio,
        bool enableVirtualFileSystem,
        bool enableMicrophone,
        bool duplicateFrames,
        std::optional<ImmersiveMode::Config> immersiveModeConfig,
        const std::string& language
    );
    void resume();
    void step();
    void stepWithoutVideo();
    void pause();
    bool isRunning();
    void destroy();

    void reset();

    void glJniEnvInit(JNIEnv* env);
    void glJniEnvDestroy();

    void getSystemInfo(struct retro_system_info *system_info);
    void loadGameFromPath(const std::string &gamePath);
    void loadGameFromBytes(const int8_t *data, size_t size);
    void reloadGameFromBytes(const int8_t *data, size_t size);
    void loadGameFromVirtualFiles(std::vector<VFSFile> virtualFiles);

    void onKeyEvent(unsigned int port, int action, int keyCode);
    void onMotionEvent(unsigned int port, unsigned int source, float xAxis, float yAxis);
    void onTouchEvent(float xAxis, float yAxis);

    void refreshAspectRatio();
    float getAspectRatio();
    float getContentFrameRate() const;
    void setAspectRatio(float aspectRatio);

    bool requiresVideoRefresh() const;
    void clearRequiresVideoRefresh();

    std::vector<Variable> getVariables();
    void updateVariable(const Variable& variable);

    std::vector<std::vector<struct Controller>> getControllers();
    void setControllerType(unsigned int port, unsigned int type);

    int availableDisks();
    int currentDisk();
    void changeDisk(unsigned int index);

    void setRumbleEnabled(bool enabled);
    bool isRumbleEnabled() const;
    void handleRumbleUpdates(const std::function<void(int, float, float)> &handler);
//    int handleLinkExchange(unsigned type,int* args,uint16_t* data);

    void setFrameSpeed(float speed);

    void setAudioEnabled(bool enabled);
    void setAudioVolume(int volume);

    void setShaderConfig(ShaderManager::Config shaderConfig);

    void resetGlobalVariables();

    // Handle callbacks
    void handleVideoRefresh(const void *data, unsigned width, unsigned height, size_t pitch);
    size_t handleAudioCallback(const int16_t* data, size_t frames);
    int16_t handleSetInputState(unsigned port, unsigned device, unsigned index, unsigned id);
    uintptr_t handleGetCurrentFrameBuffer();

private:
    void step(bool renderVideo);
    void updateAudioSampleRateMultiplier();
    void releasePendingSoftwareVideoBuffer();
    uint8_t* ensurePendingSoftwareVideoBuffer(size_t size);
    void flushPendingSoftwareVideoFrame();
    float findDefaultAspectRatio(const retro_system_av_info &system_av_info);
    void afterGameLoad();
    const struct retro_memory_descriptor* findMemoryDescriptor(unsigned int offset);

protected:
    static void callback_hw_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch);
    static size_t callback_set_audio_sample_batch(const int16_t* data, size_t frames);
    static void callback_audio_sample(int16_t left, int16_t right);
    static int16_t callback_set_input_state(unsigned port, unsigned device, unsigned index, unsigned id);
    static uintptr_t callback_get_current_framebuffer();
    static void callback_retro_set_input_poll();

private:
    struct PendingKeyEvent {
        unsigned int port;
        int action;
        int keyCode;
    };

    void clearPendingKeyEvents();

    JavaVM* javaVm = nullptr;
    std::mutex keyEventsMutex;
    std::deque<PendingKeyEvent> pendingKeyEvents;
    std::array<std::unordered_set<int>, 4> submittedKeys;
    float frameSpeed = 1.0f;
    float frameAccumulator = 0.0f;
    int audioVolume = 100;
    bool suppressVideoOutput = false;
    bool preferLowLatencyAudio = false;
    bool rumbleEnabled = false;

    ShaderManager::Config fragmentShaderConfig = ShaderManager::Config {
        ShaderManager::Type::SHADER_DEFAULT, { }
    };

    Rect viewportRect = Rect(0.0F, 0.0F, 1.0F, 1.0F);
    float screenRefreshRate = 60.0;
    float contentFrameRate = 0.0;
    int openglESVersion = 2;
    bool skipDuplicateFrames = false;
    bool immersiveModeEnabled = false;
    ImmersiveMode::Config immersiveModeConfig {};

    float defaultAspectRatio = 1.0;
    float overrideAspectRatio = 0.0F;
    bool dirtyVideo = false;

    std::vector<uint8_t> pendingSoftwareVideoFrame;
    uint8_t* pendingSoftwareVideoGuardedData = nullptr;
    void* pendingSoftwareVideoMapping = nullptr;
    size_t pendingSoftwareVideoMappingSize = 0;
    size_t pendingSoftwareVideoGuardedCapacity = 0;
    unsigned pendingSoftwareVideoWidth = 0;
    unsigned pendingSoftwareVideoHeight = 0;
    size_t pendingSoftwareVideoPitch = 0;
    const void* pendingSoftwareVideoData = nullptr;
    bool pendingSoftwareVideoFrameValid = false;
    bool deferredSoftwareVideoLogged = false;
    bool guardedSoftwareVideoFallbackLogged = false;

    jclass linkExchangeClass = nullptr;
    jmethodID linkExchangeMethod = nullptr;

    std::unique_ptr<Core> core;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<Video> video;
    std::unique_ptr<FPSSync> fpsSync;
    std::unique_ptr<Input> input;
    std::unique_ptr<Rumble> rumble;
};

} //namespace libretrodroid

#endif //LIBRETRODROID_LIBRETRODROID_H

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

#include <jni.h>

#include <EGL/egl.h>

#include <string>
#include <limits>
#include <utility>
#include <vector>
#include <unordered_set>

#include <android/input.h>

#ifdef __ANDROID__
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "libretrodroid.h"
#include "utils/libretrodroidexception.h"
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
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/utils.h"
#include "utils/rect.h"
#include "errorcodes.h"
#include "vfs/vfs.h"

namespace libretrodroid {

static bool shouldLogLinkSequence(int sequence) {
    return sequence > 0 && (sequence <= 20 || (sequence & 0x7F) == 0);
}

uintptr_t LibretroDroid::callback_get_current_framebuffer() {
    return LibretroDroid::getInstance().handleGetCurrentFrameBuffer();
}

void LibretroDroid::callback_hw_video_refresh(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    LOGD("hw video refresh callback called %i %i", width, height);
    LibretroDroid::getInstance().handleVideoRefresh(data, width, height, pitch);
}

void LibretroDroid::callback_audio_sample(int16_t left, int16_t right) {
    LOGE("callback audio sample (left, right) has been called");
}

size_t LibretroDroid::callback_set_audio_sample_batch(const int16_t *data, size_t frames) {
    return LibretroDroid::getInstance().handleAudioCallback(data, frames);
}

void LibretroDroid::callback_retro_set_input_poll() {
    auto& instance = LibretroDroid::getInstance();
    if (!instance.input) return;
    std::vector<PendingKeyEvent> events;
    {
        std::lock_guard<std::mutex> lock(instance.keyEventsMutex);
        std::array<std::unordered_set<int>, 4> advancedKeys;
        for (auto it = instance.pendingKeyEvents.begin(); it != instance.pendingKeyEvents.end();) {
            if (advancedKeys[it->port].insert(it->keyCode).second) {
                events.push_back(*it);
                it = instance.pendingKeyEvents.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& event : events) {
        instance.input->onKeyEvent(event.port, event.action, event.keyCode);
    }
}

int16_t LibretroDroid::callback_set_input_state(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    return LibretroDroid::getInstance().handleSetInputState(port, device, index, id);
}

void LibretroDroid::updateAudioSampleRateMultiplier() {
    if (audio) {
        audio->setPlaybackSpeed(frameSpeed);
    }
}

// TODO... Do we really need this?
void LibretroDroid::resetGlobalVariables() {
    releasePendingSoftwareVideoBuffer();
    core = nullptr;
    audio = nullptr;
    video = nullptr;
    fpsSync = nullptr;
    input = nullptr;
    clearPendingKeyEvents();
    rumble = nullptr;
    pendingSoftwareVideoFrame.clear();
    pendingSoftwareVideoData = nullptr;
    pendingSoftwareVideoFrameValid = false;
    deferredSoftwareVideoLogged = false;
    guardedSoftwareVideoFallbackLogged = false;
}

int LibretroDroid::availableDisks() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_num_images()
           : 0;
}

int LibretroDroid::currentDisk() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_image_index()
           : 0;
}

void LibretroDroid::changeDisk(unsigned int index) {
    if (Environment::getInstance().getRetroDiskControlCallback() == nullptr) {
        LOGE("Cannot swap disk. This platform does not support it.");
        return;
    }

    if (index < 0 || index >= Environment::getInstance().getRetroDiskControlCallback()->get_num_images()) {
        LOGE("Requested image index is not valid.");
        return;
    }

    if (Environment::getInstance().getRetroDiskControlCallback()->get_image_index() != index) {
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(true);
        Environment::getInstance().getRetroDiskControlCallback()->set_image_index((unsigned) index);
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(false);
    }
}

void LibretroDroid::updateVariable(const Variable& variable) {
    Environment::getInstance().updateVariable(variable.key, variable.value);
}

std::vector<Variable> LibretroDroid::getVariables() {
    return Environment::getInstance().getVariables();
}

std::vector<std::vector<struct Controller>> LibretroDroid::getControllers() {
    return Environment::getInstance().getControllers();
}

void LibretroDroid::setControllerType(unsigned int port, unsigned int type) {
    core->retro_set_controller_port_device(port, type);
}

bool LibretroDroid::unserializeState(int8_t *data, size_t size) {
    if (!core){
        return false;
    }
    return core->retro_unserialize(data, size);
}

JNIEXPORT jboolean JNICALL LibretroDroid::unserializeSRAM(int8_t* data, size_t size) {
    if (!core || !data || !size) {
        return false;
    }
    size_t sramSize = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *sramState = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

    if (sramState == nullptr) {
        LOGE("Cannot load SRAM: nullptr in retro_get_memory_data");
        return false;
    }

    if (size > sramSize) {
        LOGE("Cannot load SRAM: size mismatch");
        return false;
    }

    memcpy(sramState, data, size);

    return true;
}

std::pair<int8_t*, size_t> LibretroDroid::serializeSRAM() {
    if (!core) {
        return {nullptr, 0};
    }
    // Savestate loading can mask the original libretro save buffer. Clone the
    // primary console's active savedata instead of exporting that stale buffer.
    if (core->retro_console_get_primary && core->retro_console_read_sram) {
        uint64_t handle = core->retro_console_get_primary();
        if (handle) return serializeRomConsoleSRAM(handle);
    }
    size_t size = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    const void* source = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!size || !source) {
        return {nullptr, 0};
    }
    auto* data = new int8_t[size];
    memcpy(data, source, size);

    return std::pair(data, size);
}

const struct retro_memory_descriptor* LibretroDroid::findMemoryDescriptor(unsigned int offset) {
    unsigned base = 0;
    for (unsigned i = 8; i >= 0; i--) {
        unsigned b = i << 24;
        if (offset >= b) {
            base = b;
            break;
        }
    }
    auto desc = Environment::getInstance().getMemoryDescriptor(base);

    if(!desc || !desc->ptr || desc->len == 0){
        return nullptr;
    }

    unsigned memoryOffset = offset - base;
    if (memoryOffset >= desc->len){
        return nullptr;
    }

    size_t memorySize = desc->len - memoryOffset;
    if (memorySize < 0){
        return nullptr;
    }

    return desc;
}

std::pair<int8_t*, size_t> LibretroDroid::getMemoryData(unsigned int offset,size_t size){
    auto desc = findMemoryDescriptor(offset);
    if(!desc || !desc->ptr || offset < desc->start || offset - desc->start >= desc->len){
        return std::pair(nullptr, 0);
    }

    unsigned memoryOffset = offset - desc->start;
    // A zero-length request queries the available range without allocating it.
    if (size == 0) {
        return std::pair(nullptr, desc->len - memoryOffset);
    }
    size_t memorySize = std::min(desc->len - memoryOffset, size);
    auto* data = new int8_t[memorySize];

    memcpy(data, (int8_t*) desc->ptr + memoryOffset, memorySize);

    return std::pair(data, memorySize);
}

void LibretroDroid::setMemoryData(unsigned int offset,int8_t* data,size_t size) {
    auto desc = findMemoryDescriptor(offset);
    if(!desc){
        return;
    }

    unsigned memoryOffset = offset - desc->start;
    size_t memorySize = std::min(desc->len - memoryOffset, size);
    memcpy((int8_t*) desc->ptr + memoryOffset, data, memorySize);
}

void LibretroDroid::onSurfaceChanged(unsigned int width, unsigned int height) {
    LOGD("Performing libretrodroid onSurfaceChanged");
    video->updateScreenSize(width, height);
}

void LibretroDroid::onSurfaceCreated() {
    LOGD("Performing libretrodroid onSurfaceCreated");

    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    video = nullptr;

    Video::RenderingOptions renderingOptions {
        Environment::getInstance().isUseHwAcceleration(),
        system_av_info.geometry.base_width,
        system_av_info.geometry.base_height,
        Environment::getInstance().isUseDepth(),
        Environment::getInstance().isUseStencil(),
        openglESVersion,
        Environment::getInstance().getPixelFormat()
    };

    auto newVideo = new Video(
        renderingOptions,
        fragmentShaderConfig,
        Environment::getInstance().isBottomLeftOrigin(),
        Environment::getInstance().getScreenRotation(),
        skipDuplicateFrames,
        immersiveModeEnabled,
        viewportRect,
        immersiveModeConfig
    );

    video = std::unique_ptr<Video>(newVideo);

    if (Environment::getInstance().getHwContextReset() != nullptr) {
        Environment::getInstance().getHwContextReset()();
    }
}

void LibretroDroid::onMotionEvent(
    unsigned int port,
    unsigned int source,
    float xAxis,
    float yAxis
) {
    LOGD("Received motion event: %d %.2f, %.2f", source, xAxis, yAxis);
    if (input) {
        input->onMotionEvent(port, source, xAxis, yAxis);
    }
}

void LibretroDroid::onTouchEvent(float xAxis, float yAxis) {
    LOGD("Received touch event: %.2f, %.2f", xAxis, yAxis);
    if (input && video) {
        auto [x, y] = video->getLayout().getRelativePosition(xAxis, yAxis);
        input->onMotionEvent(0, Input::MOTION_SOURCE_POINTER, x, y);
    }
}

void LibretroDroid::onKeyEvent(unsigned int port, int action, int keyCode) {
    LOGD("Received key event with action (%d) and keycode (%d)", action, keyCode);
    if (port >= submittedKeys.size() ||
        (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)) return;
    {
        std::lock_guard<std::mutex> lock(keyEventsMutex);
        auto& pressed = submittedKeys[port];
        if (action == AKEY_EVENT_ACTION_DOWN) {
            if (!pressed.insert(keyCode).second) return;
        } else if (pressed.erase(keyCode) == 0) {
            return;
        }
        unsigned pendingForKey = 0;
        for (const auto& event : pendingKeyEvents) {
            if (event.port == port && event.keyCode == keyCode) ++pendingForKey;
        }
        if (pendingForKey >= 8) {
            // Drop one complete old press cycle if inputs arrive faster than
            // the core can poll, while preserving the final pressed state.
            unsigned removed = 0;
            for (auto it = pendingKeyEvents.begin(); it != pendingKeyEvents.end() && removed < 2;) {
                if (it->port == port && it->keyCode == keyCode) {
                    it = pendingKeyEvents.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        // The UI thread can add an edge while a fast-forward step is running.
        pendingKeyEvents.push_back({port, action, keyCode});
    }
}

void LibretroDroid::clearPendingKeyEvents() {
    std::lock_guard<std::mutex> lock(keyEventsMutex);
    pendingKeyEvents.clear();
    for (auto& pressed : submittedKeys) pressed.clear();
}

void LibretroDroid::create(
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
) {
    LOGD("Performing libretrodroid create");

    if (core) {
        throw std::runtime_error("Previous native session has not been destroyed");
    }
    resetGlobalVariables();

    Environment::getInstance().initialize(systemDir, savesDir, &callback_get_current_framebuffer);
    Environment::getInstance().setLanguage(language);
    Environment::getInstance().setEnableVirtualFileSystem(enableVirtualFileSystem);
    Environment::getInstance().setEnableMicrophone(enableMicrophone);

    openglESVersion = GLESVersion;
    screenRefreshRate = refreshRate;
    contentFrameRate = 0.0f;
    skipDuplicateFrames = duplicateFrames;
    immersiveModeEnabled = GLESVersion >= 3 && immersiveModeConfig.has_value();
    this->immersiveModeConfig = immersiveModeConfig.value_or(ImmersiveMode::Config{});
    audioVolume = 100;
    frameSpeed = 1.0f;
    frameAccumulator = 0.0f;

    linkExchangeClass = nullptr;
    linkExchangeMethod = nullptr;
    javaVm = nullptr;

    core = std::make_unique<Core>(soFilePath);

    core->retro_set_video_refresh(&callback_hw_video_refresh);
    core->retro_set_environment(&Environment::callback_environment);
    core->retro_set_audio_sample(&callback_audio_sample);
    core->retro_set_audio_sample_batch(&callback_set_audio_sample_batch);
    core->retro_set_input_poll(&callback_retro_set_input_poll);
    core->retro_set_input_state(&callback_set_input_state);

    std::for_each(variables.begin(), variables.end(), [&](const Variable& v) {
        updateVariable(v);
    });

    /* Link settings describe a live session, not a persistent core option.
     * Environment is process-global, so a quickly recreated Activity must
     * always start detached even if the previous session ended abnormally. */
    Environment::getInstance().updateVariable("mgba_link_multiplayer", "OFF");
    Environment::getInstance().updateVariable("mgba_link_player_id", "0");
    Environment::getInstance().updateVariable("mgba_link_player_count", "1");

    core->retro_init();

    preferLowLatencyAudio = lowLatencyAudio;

    // HW accelerated cores are only supported on opengles 3.
    if (Environment::getInstance().isUseHwAcceleration() && openglESVersion < 3) {
        throw LibretroDroidError("OpenGL ES 3 is required for this Core", ERROR_GL_NOT_COMPATIBLE);
    }

    fragmentShaderConfig = shaderConfig;

    rumble = std::make_unique<Rumble>();
}

void LibretroDroid::getSystemInfo(struct retro_system_info *system_info) {
    core->retro_get_system_info(system_info);
}

void LibretroDroid::loadGameFromPath(const std::string& gamePath) {
    LOGD("Performing libretrodroid loadGameFromPath");
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(gamePath);
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(gamePath);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromBytes(const int8_t *data, size_t size) {
    LOGD("Performing libretrodroid loadGameFromBytes");

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = nullptr;
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        game_info.data = data;
        game_info.size = size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::reloadGameFromBytes(const int8_t *data, size_t size) {
    if (!core) {
        throw std::runtime_error("Cannot reload game without a core");
    }
    core->retro_unload_game();
    core->retro_deinit();
    core->retro_init();
    loadGameFromBytes(data, size);
}

void LibretroDroid::loadGameFromVirtualFiles(std::vector<VFSFile> virtualFiles) {
    LOGD("Performing libretrodroid loadGameFromVirtualFiles");
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    if (virtualFiles.empty()) {
        LOGE("Calling loadGameFromVirtualFiles without any file.");
        throw std::runtime_error("Calling loadGameFromVirtualFiles without any file.");
    }

    std::string firstFilePath = virtualFiles[0].getFileName();
    int firstFileFD = virtualFiles[0].getFD();

    bool loadUsingVFS = system_info.need_fullpath || virtualFiles.size() > 1;

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(firstFilePath);
    game_info.meta = nullptr;

    if (loadUsingVFS) {
        VFS::getInstance().initialize(std::move(virtualFiles));
    }

    if (loadUsingVFS) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(firstFileFD);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::destroy() {
    LOGD("Performing libretrodroid destroy");

    /* Clear runtime link state before unloading the game. This is deliberately
     * independent of the Activity's room cleanup: remote EOF, fast navigation
     * or lifecycle reordering must not leave multiplayer=ON in Environment. */
    if (core && core->retro_link_cancel) {
        core->retro_link_cancel();
    }
    Environment::getInstance().updateVariable("mgba_link_multiplayer", "OFF");
    Environment::getInstance().updateVariable("mgba_link_player_id", "0");
    Environment::getInstance().updateVariable("mgba_link_player_count", "1");
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] lifecycle reset multiplayer=OFF player=0 count=1") */;

    // The detach fallback runs after the old GL thread/context have exited.
    const bool hasGLContext = eglGetCurrentContext() != EGL_NO_CONTEXT;
    if (hasGLContext && Environment::getInstance().getHwContextDestroy() != nullptr) {
        Environment::getInstance().getHwContextDestroy()();
    }

    if (core) {
        core->retro_cheat_reset();
        core->retro_unload_game();
        core->retro_deinit();
    }

    video = nullptr;
    /* Finish explicit GL object destruction while this EGL context is still
     * current. Leaving programs, textures and FBOs for eglTerminate() makes
     * vendor drivers perform all deferred compiler cleanup at once. */
    if (hasGLContext) glFinish();
    core = nullptr;
    rumble = nullptr;
    fpsSync = nullptr;
    audio = nullptr;
    contentFrameRate = 0.0f;
    releasePendingSoftwareVideoBuffer();
    input = nullptr;
    clearPendingKeyEvents();
    pendingSoftwareVideoFrame.clear();
    pendingSoftwareVideoData = nullptr;
    pendingSoftwareVideoFrameValid = false;
    deferredSoftwareVideoLogged = false;
    guardedSoftwareVideoFallbackLogged = false;

    Environment::getInstance().deinitialize();
    VFS::getInstance().deinitialize();
}

void LibretroDroid::glJniEnvInit(JNIEnv* env) {
    if (!env) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] glJniEnvInit rejected: env is null") */;
        return;
    }
    env->GetJavaVM(&javaVm);
    if (linkExchangeClass) {
        env->DeleteGlobalRef(linkExchangeClass);
        linkExchangeClass = nullptr;
    }
    jclass localClass = env->FindClass("com/swordfish/libretrodroid/LibretroDroid");
    if (!localClass) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] LibretroDroid Java class was not found") */;
        env->ExceptionClear();
        return;
    }
    linkExchangeClass = static_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);
    if (linkExchangeClass) {
        linkExchangeMethod = env->GetStaticMethodID(linkExchangeClass, "linkExchangeCallback", "(I[I[S)I");
        if (!linkExchangeMethod) {
            /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] Java linkExchangeCallback method was not found") */;
            env->ExceptionClear();
        }
    }
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] JNI callback initialized class=%p method=%p", linkExchangeClass, linkExchangeMethod) */;
}

void LibretroDroid::releasePendingSoftwareVideoBuffer() {
#ifdef __ANDROID__
    if (pendingSoftwareVideoMapping) {
        munmap(pendingSoftwareVideoMapping, pendingSoftwareVideoMappingSize);
    }
#endif
    pendingSoftwareVideoGuardedData = nullptr;
    pendingSoftwareVideoMapping = nullptr;
    pendingSoftwareVideoMappingSize = 0;
    pendingSoftwareVideoGuardedCapacity = 0;
}

uint8_t* LibretroDroid::ensurePendingSoftwareVideoBuffer(size_t size) {
    if (pendingSoftwareVideoGuardedData && size <= pendingSoftwareVideoGuardedCapacity) {
        return pendingSoftwareVideoGuardedData;
    }
#ifdef __ANDROID__
    releasePendingSoftwareVideoBuffer();
    long pageSizeValue = sysconf(_SC_PAGESIZE);
    if (pageSizeValue <= 0) {
        return nullptr;
    }
    const size_t pageSize = static_cast<size_t>(pageSizeValue);
    const size_t writableSize = (size + pageSize - 1) / pageSize * pageSize;
    const size_t mappingSize = writableSize + pageSize * 2;
    void* mapping = mmap(nullptr, mappingSize, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return nullptr;
    }
    void* writable = static_cast<uint8_t*>(mapping) + pageSize;
    if (mprotect(writable, writableSize, PROT_READ | PROT_WRITE) != 0) {
        munmap(mapping, mappingSize);
        return nullptr;
    }
    pendingSoftwareVideoMapping = mapping;
    pendingSoftwareVideoMappingSize = mappingSize;
    pendingSoftwareVideoGuardedData = static_cast<uint8_t*>(writable);
    pendingSoftwareVideoGuardedCapacity = writableSize;
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaVideo] guarded frontend staging buffer bytes=%zu capacity=%zu page=%zu",
         size, writableSize, pageSize) */;
    return pendingSoftwareVideoGuardedData;
#else
    (void) size;
    return nullptr;
#endif
}

void LibretroDroid::glJniEnvDestroy() {
    JNIEnv* env = nullptr;
    if (javaVm && javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && linkExchangeClass) {
        env->DeleteGlobalRef(linkExchangeClass);
    }
    linkExchangeClass = nullptr;
    linkExchangeMethod = nullptr;
    javaVm = nullptr;
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] JNI callback destroyed") */;
}

void LibretroDroid::resume() {
    LOGD("Performing libretrodroid resume");

    clearPendingKeyEvents();
    input = std::make_unique<Input>();

    frameAccumulator = 0.0f;
    fpsSync->reset();
    if (audioVolume > 0) {
        audio->start();
    }
    refreshAspectRatio();
}

void LibretroDroid::pause() {
    LOGD("Performing libretrodroid pause");
    if (audioVolume > 0) {
        audio->stop();
    }

    input = nullptr;
    clearPendingKeyEvents();
}

bool LibretroDroid::isRunning() {
    return input != nullptr;
}

void LibretroDroid::step() {
    step(true);
}

void LibretroDroid::stepWithoutVideo() {
    step(false);
}

void LibretroDroid::step(bool renderVideo) {
    LOGD("Stepping into retro_run()");

    suppressVideoOutput = !renderVideo;
    if (suppressVideoOutput) {
        pendingSoftwareVideoData = nullptr;
        pendingSoftwareVideoFrameValid = false;
    }

    unsigned frames = 1;
    if (fpsSync) {
        unsigned requestedFrames = fpsSync->advanceFrames();

        // If the application runs too slow it's better to just skip those frames.
        frames = std::min(requestedFrames, 2u);
    }

    for (size_t i = 0; i < frames; i++) {
        frameAccumulator += frameSpeed;
        unsigned int runsThisFrame = static_cast<unsigned int>(frameAccumulator);
        frameAccumulator -= runsThisFrame;
        for (unsigned int r = 0; r < runsThisFrame; r++) {
            core->retro_run();
            if (renderVideo) {
                flushPendingSoftwareVideoFrame();
            }
        }
    }

    suppressVideoOutput = false;

    if (renderVideo && video && !video->rendersInVideoCallback()) {
        video->renderFrame();
    }

    if (fpsSync) {
        fpsSync->wait();
    }

    if (renderVideo && rumble && rumbleEnabled) {
        rumble->fetchFromEnvironment();
    }

    // Some games override the core geometry at runtime. These fields get updated in retro_run().
    if (renderVideo && video && Environment::getInstance().isGameGeometryUpdated()) {
        Environment::getInstance().clearGameGeometryUpdated();

        video->updateRendererSize(
            Environment::getInstance().getGameGeometryWidth(),
            Environment::getInstance().getGameGeometryHeight()
        );

        dirtyVideo = true;
    }

    if (renderVideo && video && Environment::getInstance().isScreenRotationUpdated()) {
        Environment::getInstance().clearScreenRotationUpdated();

        video->updateRotation(Environment::getInstance().getScreenRotation());
    }
}

float LibretroDroid::getAspectRatio() {
    if (overrideAspectRatio > 0) {
        return overrideAspectRatio;
    }
    float gameAspectRatio = Environment::getInstance().retrieveGameSpecificAspectRatio();
    return gameAspectRatio > 0 ? gameAspectRatio : defaultAspectRatio;
}

float LibretroDroid::getContentFrameRate() const {
    return contentFrameRate;
}

void LibretroDroid::setAspectRatio(float aspectRatio) {
    overrideAspectRatio = aspectRatio;
    refreshAspectRatio();
}

void LibretroDroid::refreshAspectRatio() {
    if (video != nullptr) {
        video->updateAspectRatio(getAspectRatio());
    }
}

void LibretroDroid::setRumbleEnabled(bool enabled) {
    rumbleEnabled = enabled;
}

bool LibretroDroid::isRumbleEnabled() const {
    return rumbleEnabled;
}

void LibretroDroid::setFrameSpeed(float speed) {
    frameSpeed = speed;
    frameAccumulator = 0.0f;
    updateAudioSampleRateMultiplier();
}

void LibretroDroid::setAudioEnabled(bool enabled) {
    setAudioVolume(enabled ? 100 : 0);
}

void LibretroDroid::setAudioVolume(int volume) {
    int previousVolume = audioVolume;
    audioVolume = std::max(0, std::min(100, volume));
    if (audio && previousVolume > 0 && audioVolume == 0) {
        /* A virtual pause drops newly generated PCM. Discard the old FIFO and
         * controller history as well so unmuting cannot replay a stale tail. */
        audio->resetBuffer();
    }
    if (isRunning()){
        if (audioVolume > 0) {
            audio->start();
        } else {
            audio->stop();
        }
    }
}

void LibretroDroid::setShaderConfig(ShaderManager::Config shaderConfig) {
    fragmentShaderConfig = std::move(shaderConfig);
    if (video) {
        video->updateShaderType(fragmentShaderConfig);
    }
}

void LibretroDroid::handleVideoRefresh(
    const void *data,
    unsigned int width,
    unsigned int height,
    size_t pitch
) {
    if (suppressVideoOutput) {
        pendingSoftwareVideoData = nullptr;
        pendingSoftwareVideoFrameValid = false;
        return;
    }

    if (!video) {
        pendingSoftwareVideoData = nullptr;
        pendingSoftwareVideoFrameValid = false;
        return;
    }

    if (video->rendersInVideoCallback()) {
        video->onNewFrame(data, width, height, pitch);
        video->renderFrame();
        return;
    }

    if (!data || !width || !height || !pitch) {
        return;
    }

    const size_t bytesPerPixel = Environment::getInstance().getPixelFormat() == RETRO_PIXEL_FORMAT_XRGB8888
        ? 4u : 2u;
    constexpr size_t maxDeferredSoftwareFrameBytes = 128u * 1024u * 1024u;
    const size_t sizeMax = std::numeric_limits<size_t>::max();
    if (width > sizeMax / bytesPerPixel || pitch < static_cast<size_t>(width) * bytesPerPixel ||
        height > sizeMax / pitch || pitch * static_cast<size_t>(height) > maxDeferredSoftwareFrameBytes) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaVideo] rejected software frame data=%p size=%ux%u pitch=%zu bpp=%zu",
             data, width, height, pitch, bytesPerPixel) */;
        pendingSoftwareVideoData = nullptr;
        pendingSoftwareVideoFrameValid = false;
        return;
    }

    /* Never expose a core-owned framebuffer or its padded row layout to a GL
     * driver. Some affected devices later treated repeated RGB565 pixels as
     * native pointers. Pack only the active pixels into frontend-owned memory
     * while retro_run() is still on the stack. */
    const size_t packedPitch = static_cast<size_t>(width) * bytesPerPixel;
    const size_t packedFrameBytes = packedPitch * static_cast<size_t>(height);
    uint8_t* packedData = ensurePendingSoftwareVideoBuffer(packedFrameBytes);
    if (!packedData) {
        pendingSoftwareVideoFrame.resize(packedFrameBytes);
        packedData = pendingSoftwareVideoFrame.data();
        if (!guardedSoftwareVideoFallbackLogged) {
            /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaVideo] guarded frontend staging unavailable; using heap fallback") */;
            guardedSoftwareVideoFallbackLogged = true;
        }
    }

    const auto* source = static_cast<const uint8_t*>(data);
    for (unsigned int y = 0; y < height; ++y) {
        memcpy(packedData + static_cast<size_t>(y) * packedPitch,
               source + static_cast<size_t>(y) * pitch,
               packedPitch);
    }
    pendingSoftwareVideoData = packedData;
    pendingSoftwareVideoWidth = width;
    pendingSoftwareVideoHeight = height;
    pendingSoftwareVideoPitch = packedPitch;
    pendingSoftwareVideoFrameValid = true;
    if (!deferredSoftwareVideoLogged) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaVideo] software frame packed and deferred sourcePitch=%zu packedPitch=%zu",
             pitch, packedPitch) */;
        deferredSoftwareVideoLogged = true;
    }
}

void LibretroDroid::flushPendingSoftwareVideoFrame() {
    if (!pendingSoftwareVideoFrameValid) {
        return;
    }
    if (video) {
        video->onNewFrame(
            pendingSoftwareVideoData,
            pendingSoftwareVideoWidth,
            pendingSoftwareVideoHeight,
            pendingSoftwareVideoPitch
        );
    }
    pendingSoftwareVideoData = nullptr;
    pendingSoftwareVideoFrameValid = false;
}

size_t LibretroDroid::handleAudioCallback(const int16_t *data, size_t frames) {
    if (audio && audioVolume > 0) {
        if (audioVolume == 100) {
            audio->write(data, frames);
        } else {
            // 按比例缩放音量
            std::vector<int16_t> scaled(frames * 2);
            float factor = audioVolume / 100.0f;
            for (size_t i = 0; i < frames * 2; i++) {
                scaled[i] = static_cast<int16_t>(data[i] * factor);
            }
            audio->write(scaled.data(), frames);
        }
    }
    return frames;
}

int16_t LibretroDroid::handleSetInputState(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    if (input) {
        return input->getInputState(port, device, index, id);
    }
    return 0;
}

uintptr_t LibretroDroid::handleGetCurrentFrameBuffer() {
    if (video) {
        return video->getCurrentFramebuffer();
    }
    return 0;
}

void LibretroDroid::reset() {
    frameAccumulator = 0.0f;
    core->retro_reset();
}

std::pair<int8_t*, size_t> LibretroDroid::serializeState() {
    if (!core){
        return std::pair(nullptr, 0);
    }

    size_t size = core->retro_serialize_size();
    auto data = new int8_t[size];

    if (!core->retro_serialize(data, size)) {
        delete[] data;
        return std::pair(nullptr, 0);
    }

    return std::pair(data, size);
}

void LibretroDroid::resetCheat() {
    core->retro_cheat_reset();
}

void LibretroDroid::setCheat(unsigned index, bool enabled, const std::string& code) {
    core->retro_cheat_set(index, enabled, Utils::cloneToCString(code));
}

bool LibretroDroid::requiresVideoRefresh() const {
    return dirtyVideo;
}

void LibretroDroid::clearRequiresVideoRefresh() {
    dirtyVideo = false;
}

void LibretroDroid::afterGameLoad() {
    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);
    contentFrameRate = static_cast<float>(system_av_info.timing.fps);

    fpsSync = std::make_unique<FPSSync>(system_av_info.timing.fps, screenRefreshRate);

    double inputSampleRate = system_av_info.timing.sample_rate * fpsSync->getTimeStretchFactor();

    const auto roundedInputSampleRate = (int32_t) std::lround(inputSampleRate);
    if (!audio || !audio->resetForContent(
            roundedInputSampleRate,
            system_av_info.timing.fps
        )) {
        audio = std::make_unique<Audio>(
            roundedInputSampleRate,
            system_av_info.timing.fps,
            preferLowLatencyAudio
        );
    } else {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("Reusing audio stream for reloaded content") */;
    }

    updateAudioSampleRateMultiplier();

    defaultAspectRatio = findDefaultAspectRatio(system_av_info);
}

std::pair<int8_t*, jint*> LibretroDroid::getPixels() {
    if(!core){
        return std::pair(nullptr, nullptr);
    }

    struct retro_screenshot_info info {};
    core->retro_get_pixels(&info);

    if (info.pixels == nullptr) {
        return std::pair(nullptr, nullptr);
    }

    auto* size = new int[]{
        static_cast<jint>(info.width),
        static_cast<jint>(info.height),
        static_cast<jint>(info.size),
        static_cast<jint>(info.stride),
        static_cast<jint>(info.format)
    };

    return std::pair((int8_t*) info.pixels, size);
}

//void retro_set_rtc(unsigned type,int64_t value);
void LibretroDroid::setRTC(unsigned type,int64_t value) {
    core->retro_set_rtc(type, value);
}

void LibretroDroid::setMultiplayer(int playerId, int playerCount) {
    bool enabled = playerCount >= 2 && playerCount <= 4 && playerId >= 0 && playerId < playerCount && hasLinkSupport();
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] setMultiplayer request player=%d count=%d support=%d enabled=%d",
         playerId, playerCount, hasLinkSupport(), enabled) */;
    Environment::getInstance().updateVariable("mgba_link_multiplayer", enabled ? "ON" : "OFF");
    Environment::getInstance().updateVariable("mgba_link_player_id", std::to_string(enabled ? playerId : 0));
    Environment::getInstance().updateVariable("mgba_link_player_count", std::to_string(enabled ? playerCount : 1));
    if (!enabled && core && core->retro_link_cancel) {
        core->retro_link_cancel();
    }
}

bool LibretroDroid::configureLinkNetplay(int mode, int inputDelayFrames) {
    if (!core || !core->retro_link_set_netplay_mode || mode < 0 || mode > 1 ||
        inputDelayFrames < 2 || inputDelayFrames > 30) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] netplay config rejected mode=%d delay=%d symbol=%p",
             mode, inputDelayFrames,
             core ? reinterpret_cast<void*>(core->retro_link_set_netplay_mode) : nullptr) */;
        return false;
    }
    core->retro_link_set_netplay_mode(mode, inputDelayFrames);
    if (audio) {
        audio->setNetworkJitterBuffering(mode == 1);
        audio->resetBuffer();
    }
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] netplay config mode=%d delay=%d singleton=1", mode, inputDelayFrames) */;
    return true;
}

int LibretroDroid::getLinkNetplayStatus() const {
    return core && core->retro_link_get_status ? core->retro_link_get_status() : 0;
}

size_t LibretroDroid::copyUnpatchedRom(void* output, size_t capacity) const {
    return core && core->retro_copy_unpatched_rom ?
        core->retro_copy_unpatched_rom(output, capacity) : 0;
}

int64_t LibretroDroid::getBiosIdentity() const {
    uint32_t crc32 = 0;
    bool fullBios = false;
    if (!core || !core->retro_get_bios_info ||
        !core->retro_get_bios_info(&crc32, &fullBios)) {
        return -1;
    }
    return static_cast<int64_t>(crc32) | (static_cast<int64_t>(fullBios) << 32);
}

int64_t LibretroDroid::getRomCrc32() const {
    uint32_t romCrc32 = 0;
    if (!core || !core->retro_get_rom_crc32 ||
        !core->retro_get_rom_crc32(&romCrc32)) {
        return -1;
    }
    return static_cast<int64_t>(romCrc32);
}

bool LibretroDroid::setMultiplayerSockets(const int socketFds[4], int playerId, int playerCount) {
    bool valid = socketFds && playerCount >= 2 && playerCount <= 4 &&
        playerId >= 0 && playerId < playerCount && core && core->retro_link_set_sockets;
    if (!valid) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] native socket request rejected player=%d count=%d symbol=%p",
             playerId, playerCount, core ? reinterpret_cast<void*>(core->retro_link_set_sockets) : nullptr) */;
        return false;
    }
    bool started = core->retro_link_set_sockets(socketFds, playerId, playerCount);
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] native socket request player=%d count=%d started=%d fds=%d,%d,%d,%d",
         playerId, playerCount, started, socketFds[0], socketFds[1], socketFds[2], socketFds[3]) */;
    if (started) {
        setMultiplayer(playerId, playerCount);
    }
    return started;
}

bool LibretroDroid::isNativeLinkActive() const {
    return core && core->retro_link_native_active && core->retro_link_native_active();
}

int64_t LibretroDroid::linkRemoteStart(int sequence, int mode, int transferCycles, uint32_t primaryData) {
    if (!core || !core->retro_link_remote_start) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] remote START rejected seq=%d: core symbol unavailable", sequence) */;
        return -1;
    }
    int64_t result = core->retro_link_remote_start(sequence, mode, transferCycles, primaryData);
    if (result < 0) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaLink] remote START rejected seq=%d mode=%d cycles=%d primary=%08X",
             sequence, mode, transferCycles, primaryData) */;
    } else if (shouldLogLinkSequence(sequence)) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] remote START accepted seq=%d local=%08X", sequence, static_cast<uint32_t>(result)) */;
    }
    return result;
}

bool LibretroDroid::linkRemoteMode(int playerId, int mode) {
    bool updated = core && core->retro_link_remote_mode && core->retro_link_remote_mode(playerId, mode);
    if (updated) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] remote MODE accepted player=%d mode=%d", playerId, mode) */;
    } else {
        /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaLink] remote MODE rejected player=%d mode=%d", playerId, mode) */;
    }
    return updated;
}

bool LibretroDroid::linkRemoteCommit(int sequence, const uint32_t data[4]) {
    bool committed = core && core->retro_link_remote_commit && core->retro_link_remote_commit(sequence, data);
    if (!committed) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaLink] remote COMMIT rejected seq=%d", sequence) */;
    } else if (shouldLogLinkSequence(sequence)) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] remote COMMIT accepted seq=%d data=%08X,%08X,%08X,%08X",
             sequence, data[0], data[1], data[2], data[3]) */;
    }
    return committed;
}

void LibretroDroid::linkCancel() {
    /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaLink] cancel requested") */;
    if (core && core->retro_link_cancel) {
        core->retro_link_cancel();
    }
}

bool LibretroDroid::hasLinkSupport() const {
    bool nativeSupport = core && core->retro_link_set_sockets && core->retro_get_rom_crc32 &&
        core->retro_link_native_active && core->retro_link_cancel;
    bool legacySupport = core && core->retro_link_remote_start && core->retro_link_remote_mode &&
        core->retro_link_remote_commit && core->retro_link_cancel;
    return nativeSupport || legacySupport;
}

bool LibretroDroid::hasRomConsoleSupport() const {
    return core && core->retro_console_get_primary && core->retro_console_create &&
        core->retro_console_start_lan && core->retro_console_get_rom_crc32 &&
        core->retro_console_start_local &&
        core->retro_console_set_foreground && core->retro_console_get_foreground &&
        core->retro_console_read_sram && core->retro_console_serialize_state &&
        core->retro_console_unserialize_state && core->retro_console_stop_local;
}

uint64_t LibretroDroid::getPrimaryRomConsole() const {
    return hasRomConsoleSupport() ? core->retro_console_get_primary() : 0;
}

uint64_t LibretroDroid::createRomConsole(const int8_t* romData, size_t romSize,
                                         const int8_t* sramData, size_t sramSize) {
    if (!hasRomConsoleSupport() || !romData || !romSize) {
        return 0;
    }
    uint32_t romCrc32 = 0;
    uint64_t handle = core->retro_console_create(romData, romSize, sramData, sramSize,
                                                  &romCrc32);
    /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] console create handle=%08X:%08X romSize=%zu crc32=%08X sramSize=%zu",
         static_cast<uint32_t>(handle >> 32), static_cast<uint32_t>(handle), romSize,
         romCrc32, sramSize) */;
    return handle;
}

bool LibretroDroid::destroyRomConsole(uint64_t handle) {
    return hasRomConsoleSupport() && core->retro_console_destroy && handle &&
        core->retro_console_destroy(handle);
}

int64_t LibretroDroid::getRomConsoleCrc32(uint64_t handle) const {
    uint32_t romCrc32 = 0;
    if (!hasRomConsoleSupport() || !handle ||
        !core->retro_console_get_rom_crc32(handle, &romCrc32)) {
        return -1;
    }
    return static_cast<int64_t>(romCrc32);
}

bool LibretroDroid::startLanLink(const int socketFds[4],
                                const std::vector<uint64_t>& handles,
                                unsigned localPlayerId) {
    if (!hasRomConsoleSupport() || !socketFds || handles.size() < 2 ||
        handles.size() > 4 || localPlayerId >= handles.size()) {
        return false;
    }
    bool started = core->retro_console_start_lan(socketFds, handles.data(),
        localPlayerId, static_cast<unsigned>(handles.size()));
    if (started) {
        setMultiplayer(static_cast<int>(localPlayerId), static_cast<int>(handles.size()));
    }
    return started;
}

bool LibretroDroid::startLocalLink(const std::vector<uint64_t>& handles) {
    if (!hasRomConsoleSupport() || handles.size() < 2 || handles.size() > 4) {
        return false;
    }
    return core->retro_console_start_local(handles.data(),
                                            static_cast<unsigned>(handles.size()));
}

bool LibretroDroid::setForegroundRomConsole(uint64_t handle) {
    return hasRomConsoleSupport() && handle && core->retro_console_set_foreground(handle);
}

uint64_t LibretroDroid::getForegroundRomConsole() const {
    return hasRomConsoleSupport() ? core->retro_console_get_foreground() : 0;
}

bool LibretroDroid::resetRomConsole(uint64_t handle) {
    return hasRomConsoleSupport() && core->retro_console_reset && handle &&
        core->retro_console_reset(handle);
}

std::pair<int8_t*, jint*> LibretroDroid::getRomConsolePixels(uint64_t handle) {
    if (!hasRomConsoleSupport() || !core->retro_console_get_pixels || !handle) {
        return {nullptr, nullptr};
    }

    struct retro_screenshot_info info {};
    if (!core->retro_console_get_pixels(handle, &info) || !info.pixels) {
        return {nullptr, nullptr};
    }

    auto* size = new int[]{
        static_cast<jint>(info.width),
        static_cast<jint>(info.height),
        static_cast<jint>(info.size),
        static_cast<jint>(info.stride),
        static_cast<jint>(info.format)
    };
    return {static_cast<int8_t*>(info.pixels), size};
}

bool LibretroDroid::resetRomConsoleCheat(uint64_t handle) {
    return core && core->retro_console_cheat_reset && handle &&
        core->retro_console_cheat_reset(handle);
}

bool LibretroDroid::setRomConsoleCheat(uint64_t handle, unsigned type, bool enabled,
                                      const std::string& code) {
    return core && core->retro_console_cheat_set && handle &&
        core->retro_console_cheat_set(handle, type, enabled, code.c_str());
}

std::pair<int8_t*, size_t> LibretroDroid::serializeRomConsoleSRAM(uint64_t handle) {
    if (!core || !core->retro_console_read_sram || !handle) {
        return {nullptr, 0};
    }
    size_t size = core->retro_console_read_sram(handle, nullptr, 0);
    if (!size) {
        return {nullptr, 0};
    }
    auto* data = new int8_t[size];
    size_t copied = core->retro_console_read_sram(handle, data, size);
    if (copied != size) {
        delete[] data;
        return {nullptr, 0};
    }
    return {data, size};
}

std::pair<int8_t*, size_t> LibretroDroid::serializeRomConsoleState(uint64_t handle) {
    if (!hasRomConsoleSupport() || !handle) {
        return {nullptr, 0};
    }
    size_t size = core->retro_console_serialize_state(handle, nullptr, 0);
    if (!size) {
        return {nullptr, 0};
    }
    auto* data = new int8_t[size];
    size_t copied = core->retro_console_serialize_state(handle, data, size);
    if (copied != size) {
        delete[] data;
        return {nullptr, 0};
    }
    return {data, size};
}

bool LibretroDroid::unserializeRomConsoleState(uint64_t handle, const int8_t* data,
                                               size_t size) {
    return hasRomConsoleSupport() && handle && data && size &&
        core->retro_console_unserialize_state(handle, data, size);
}

bool LibretroDroid::unserializeLanPlayerState(unsigned playerId, const int8_t* data,
                                              size_t size, uint32_t generation) {
    return core && core->retro_console_unserialize_lan_state && data && size && generation &&
        core->retro_console_unserialize_lan_state(playerId, data, size, generation);
}

bool LibretroDroid::resetLanPlayer(unsigned playerId, uint32_t generation) {
    return core && core->retro_console_reset_lan_player && generation &&
        core->retro_console_reset_lan_player(playerId, generation);
}

int LibretroDroid::pollLanStateBarrier(uint32_t generation) {
    return core && core->retro_console_poll_lan_barrier && generation
        ? core->retro_console_poll_lan_barrier(generation) : -1;
}

bool LibretroDroid::unserializeLanResetState(unsigned playerId, const int8_t* data,
                                             size_t size, uint32_t generation) {
    return core && core->retro_console_unserialize_lan_reset && data && size && generation &&
        core->retro_console_unserialize_lan_reset(playerId, data, size, generation);
}

void LibretroDroid::stopLocalLink() {
    if (hasRomConsoleSupport()) {
        core->retro_console_stop_local();
    }
    if (audio) {
        /* Native teardown clears mGBA's PCM, while this clears the independent
         * frontend FIFO and PI controller used by Oboe. */
        audio->resetBuffer();
        /* Keep the old mode until resetBuffer() snapshots its diagnostics, then
         * return subsequent standalone callbacks to the ordinary target. */
        audio->setNetworkJitterBuffering(false);
    }
}

int LibretroDroid::handleLinkExchange(unsigned type, int* args, uint16_t* data) {
    JNIEnv* env = nullptr;
    if (!javaVm || !linkExchangeClass || !linkExchangeMethod ||
        javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] core callback rejected type=%u: JNI callback is unavailable", type) */;
        return 0;
    }

    int sequence = args ? args[0] : -1;
    bool logDetails = type != 2 && shouldLogLinkSequence(sequence);
    if (logDetails) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] core callback enter type=%u seq=%d mode=%d cycles=%d count=%d",
             type, sequence, args ? args[1] : -1, args ? args[2] : -1, args ? args[3] : -1) */;
    }

    jintArray argsArray = env->NewIntArray(10);
    jshortArray dataArray = env->NewShortArray(4);
    if (!argsArray || !dataArray) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] core callback seq=%d: JNI array allocation failed", sequence) */;
        if (argsArray) env->DeleteLocalRef(argsArray);
        if (dataArray) env->DeleteLocalRef(dataArray);
        return 0;
    }

    jint zeroArgs[10] = {};
    jshort zeroData[4] = {};
    env->SetIntArrayRegion(argsArray, 0, 10, args ? reinterpret_cast<jint*>(args) : zeroArgs);
    env->SetShortArrayRegion(dataArray, 0, 4, data ? reinterpret_cast<jshort*>(data) : zeroData);

    jint result = env->CallStaticIntMethod(linkExchangeClass, linkExchangeMethod, static_cast<jint>(type), argsArray, dataArray);
    if (env->ExceptionCheck()) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGE("[GbaLink] Java callback threw for seq=%d", sequence) */;
        /* LINK_DIAGNOSTIC_DISABLED: env->ExceptionDescribe() */;
        env->ExceptionClear();
        result = 0;
    } else {
        if (args) {
            env->GetIntArrayRegion(argsArray, 0, 10, reinterpret_cast<jint*>(args));
        }
        if (data) {
            env->GetShortArrayRegion(dataArray, 0, 4, reinterpret_cast<jshort*>(data));
        }
    }

    env->DeleteLocalRef(argsArray);
    env->DeleteLocalRef(dataArray);
    if (result <= 0) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGW("[GbaLink] core callback failed seq=%d result=%d", sequence, result) */;
    } else if (logDetails) {
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("[GbaLink] core callback complete seq=%d result=%d data=%04X,%04X,%04X,%04X argsData=%08X,%08X,%08X,%08X",
             sequence, result,
             data ? data[0] : 0, data ? data[1] : 0, data ? data[2] : 0, data ? data[3] : 0,
             args ? static_cast<uint32_t>(args[4]) : 0, args ? static_cast<uint32_t>(args[5]) : 0,
             args ? static_cast<uint32_t>(args[6]) : 0, args ? static_cast<uint32_t>(args[7]) : 0) */;
    }
    return result;
}

float LibretroDroid::findDefaultAspectRatio(const retro_system_av_info& system_av_info) {
    float result = system_av_info.geometry.aspect_ratio;
    if (result < 0) {
        result =
            (float) system_av_info.geometry.base_width / (float) system_av_info.geometry.base_height;
    }
    return result;
}

void LibretroDroid::handleRumbleUpdates(const std::function<void(int, float, float)> &handler) {
    if (rumble && rumbleEnabled) {
        rumble->handleRumbleUpdates(handler);
    }
}

void LibretroDroid::setViewport(Rect viewportRect) {
    this->viewportRect = viewportRect;

    if (video != nullptr) {
        video->updateViewportSize(viewportRect);
    }
}

} //namespace libretrodroid
